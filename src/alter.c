/*
** 2005 February 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains C code routines that used to generate VDBE code
** that implements the ALTER TABLE command.
*/
#include "sqliteInt.h"

/*
** The code in this file only exists if we are not omitting the
** ALTER TABLE logic from the build.
*/
#ifndef SQLITE_OMIT_ALTERTABLE

/*
** Parameter zName is the name of a table that is about to be altered
** (either with ALTER TABLE ... RENAME TO or ALTER TABLE ... ADD COLUMN).
** If the table is a system table, this function leaves an error message
** in pParse->zErr (system tables may not be altered) and returns non-zero.
**
** Or, if zName is not a system table, zero is returned.
*/
static int isAlterableTable(Parse *pParse, Table *pTab){
  if( 0==sqlite3StrNICmp(pTab->zName, "sqlite_", 7)
#ifndef SQLITE_OMIT_VIRTUALTABLE
   || (pTab->tabFlags & TF_Eponymous)!=0
   || ( (pTab->tabFlags & TF_Shadow)!=0
        && sqlite3ReadOnlyShadowTables(pParse->db)
   )
#endif
  ){
    sqlite3ErrorMsg(pParse, "table %s may not be altered", pTab->zName);
    return 1;
  }
  return 0;
}

/*
** Generate code to verify that the schemas of database zDb and, if
** bTemp is not true, database "temp", can still be parsed. This is
** called at the end of the generation of an ALTER TABLE ... RENAME ...
** statement to ensure that the operation has not rendered any schema
** objects unusable.
*/
static void renameTestSchema(
  Parse *pParse,                  /* Parse context */
  const char *zDb,                /* Name of db to verify schema of */
  int bTemp,                      /* True if this is the temp db */
  const char *zWhen,              /* "when" part of error message */
  int bNoDQS                      /* Do not allow DQS in the schema */
){
  pParse->colNamesSet = 1;
  sqlite3NestedParse(pParse,
      "SELECT 1 "
      "FROM \"%w\"." LEGACY_SCHEMA_TABLE " "
      "WHERE name NOT LIKE 'sqliteX_%%' ESCAPE 'X'"
      " AND sql NOT LIKE 'create virtual%%'"
      " AND sqlite_rename_test(%Q, sql, type, name, %d, %Q, %d)=NULL ",
      zDb,
      zDb, bTemp, zWhen, bNoDQS
  );

  if( bTemp==0 ){
    sqlite3NestedParse(pParse,
        "SELECT 1 "
        "FROM temp." LEGACY_SCHEMA_TABLE " "
        "WHERE name NOT LIKE 'sqliteX_%%' ESCAPE 'X'"
        " AND sql NOT LIKE 'create virtual%%'"
        " AND sqlite_rename_test(%Q, sql, type, name, 1, %Q, %d)=NULL ",
        zDb, zWhen, bNoDQS
    );
  }
}

/*
** Generate VM code to replace any double-quoted strings (but not double-quoted
** identifiers) within the "sql" column of the sqlite_schema table in
** database zDb with their single-quoted equivalents. If argument bTemp is
** not true, similarly update all SQL statements in the sqlite_schema table
** of the temp db.
*/
static void renameFixQuotes(Parse *pParse, const char *zDb, int bTemp){
  sqlite3NestedParse(pParse,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE
      " SET sql = sqlite_rename_quotefix(%Q, sql)"
      "WHERE name NOT LIKE 'sqliteX_%%' ESCAPE 'X'"
      " AND sql NOT LIKE 'create virtual%%'" , zDb, zDb
  );
  if( bTemp==0 ){
    sqlite3NestedParse(pParse,
      "UPDATE temp." LEGACY_SCHEMA_TABLE
      " SET sql = sqlite_rename_quotefix('temp', sql)"
      "WHERE name NOT LIKE 'sqliteX_%%' ESCAPE 'X'"
      " AND sql NOT LIKE 'create virtual%%'"
    );
  }
}

/*
** Generate code to reload the schema for database iDb. And, if iDb!=1, for
** the temp database as well.
*/
static void renameReloadSchema(Parse *pParse, int iDb, u16 p5){
  Vdbe *v = pParse->pVdbe;
  if( v ){
    sqlite3ChangeCookie(pParse, iDb);
    sqlite3VdbeAddParseSchemaOp(pParse->pVdbe, iDb, 0, p5);
    if( iDb!=1 ) sqlite3VdbeAddParseSchemaOp(pParse->pVdbe, 1, 0, p5);
  }
}

/* Reload the cache from the stored text; non-zero if it will not load. */
static int alterRefreshSchema(Parse *pParse){
  sqlite3 *db = pParse->db;
  u64 savedFlags = db->flags;
  int i, rc;
  if( db->init.busy || db->nSchemaLock ) return 0;
  for(i=0; i<db->nDb; i++){
    if( db->aDb[i].pSchema ) sqlite3ResetOneSchema(db, i);
  }
  db->flags &= ~(u64)SQLITE_NoSchemaError;
  rc = sqlite3ReadSchema(pParse);
  db->flags = savedFlags;
  if( rc ) pParse->rc = rc;
  return rc;
}

/*
** Generate code to implement the "ALTER TABLE xxx RENAME TO yyy"
** command.
*/
void sqlite3AlterRenameTable(
  Parse *pParse,            /* Parser context. */
  SrcList *pSrc,            /* The table to rename. */
  Token *pName              /* The new table name. */
){
  int iDb;                  /* Database that contains the table */
  char *zDb;                /* Name of database iDb */
  Table *pTab;              /* Table being renamed */
  char *zName = 0;          /* NULL-terminated version of pName */
  sqlite3 *db = pParse->db; /* Database connection */
  int nTabName;             /* Number of UTF-8 characters in zTabName */
  const char *zTabName;     /* Original name of the table */
  Vdbe *v;
  VTable *pVTab = 0;        /* Non-zero if this is a v-tab with an xRename() */

  if( NEVER(db->mallocFailed) ) goto exit_rename_table;
  assert( pSrc->nSrc==1 );
  assert( sqlite3BtreeHoldsAllMutexes(pParse->db) );

  pTab = sqlite3LocateTableItem(pParse, 0, &pSrc->a[0]);
  if( !pTab ) goto exit_rename_table;
  iDb = sqlite3SchemaToIndex(pParse->db, pTab->pSchema);
  zDb = db->aDb[iDb].zDbSName;

  /* Get a NULL terminated version of the new table name. */
  zName = sqlite3NameFromToken(db, pName);
  if( !zName ) goto exit_rename_table;

  /* Check that a table or index named 'zName' does not already exist
  ** in database iDb. If so, this is an error.
  */
  if( sqlite3FindTable(db, zName, zDb)
   || sqlite3FindIndex(db, zName, zDb)
   || sqlite3IsShadowTableOf(db, pTab, zName)
  ){
    sqlite3ErrorMsg(pParse,
        "there is already another table or index with this name: %s", zName);
    goto exit_rename_table;
  }

  /* Make sure it is not a system table being altered, or a reserved name
  ** that the table is being renamed to.
  */
  if( SQLITE_OK!=isAlterableTable(pParse, pTab) ){
    goto exit_rename_table;
  }
  if( SQLITE_OK!=sqlite3CheckObjectName(pParse,zName,"table",zName) ){
    goto exit_rename_table;
  }

#ifndef SQLITE_OMIT_VIEW
  if( IsView(pTab) ){
    sqlite3ErrorMsg(pParse, "view %s may not be altered", pTab->zName);
    goto exit_rename_table;
  }
#endif

#ifndef SQLITE_OMIT_AUTHORIZATION
  /* Invoke the authorization callback. */
  if( sqlite3AuthCheck(pParse, SQLITE_ALTER_TABLE, zDb, pTab->zName, 0) ){
    goto exit_rename_table;
  }
#endif

#ifndef SQLITE_OMIT_VIRTUALTABLE
  if( sqlite3ViewGetColumnNames(pParse, pTab) ){
    goto exit_rename_table;
  }
  if( IsVirtual(pTab) ){
    pVTab = sqlite3GetVTable(db, pTab);
    if( pVTab->pVtab->pModule->xRename==0 ){
      pVTab = 0;
    }
  }
#endif

  /* Begin a transaction for database iDb. Then modify the schema cookie
  ** (since the ALTER TABLE modifies the schema). Call sqlite3MayAbort(),
  ** as the scalar functions (e.g. sqlite_rename_table()) invoked by the
  ** nested SQL may raise an exception.  */
  v = sqlite3GetVdbe(pParse);
  if( v==0 ){
    goto exit_rename_table;
  }
  sqlite3MayAbort(pParse);

  /* figure out how many UTF-8 characters are in zName */
  zTabName = pTab->zName;
  nTabName = sqlite3Utf8CharLen(zTabName, -1);

  /* Rewrite all CREATE TABLE, INDEX, TRIGGER or VIEW statements in
  ** the schema to use the new table name.  */
  sqlite3NestedParse(pParse,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = sqlite_rename_table(%Q, type, name, sql, %Q, %Q, %d) "
      "WHERE (type!='index' OR tbl_name=%Q COLLATE nocase)"
      "AND   name NOT LIKE 'sqliteX_%%' ESCAPE 'X'"
      , zDb, zDb, zTabName, zName, (iDb==1), zTabName
  );

  /* Update the tbl_name and name columns of the sqlite_schema table
  ** as required.  */
  sqlite3NestedParse(pParse,
      "UPDATE %Q." LEGACY_SCHEMA_TABLE " SET "
          "tbl_name = %Q, "
          "name = CASE "
            "WHEN type='table' THEN %Q "
            "WHEN name LIKE 'sqliteX_autoindex%%' ESCAPE 'X' "
            "     AND type='index' THEN "
             "'sqlite_autoindex_' || %Q || substr(name,%d+18) "
            "ELSE name END "
      "WHERE tbl_name=%Q COLLATE nocase AND "
          "(type='table' OR type='index' OR type='trigger');",
      zDb,
      zName, zName, zName,
      nTabName, zTabName
  );

#ifndef SQLITE_OMIT_AUTOINCREMENT
  /* If the sqlite_sequence table exists in this database, then update
  ** it with the new table name.
  */
  if( sqlite3FindTable(db, "sqlite_sequence", zDb) ){
    sqlite3NestedParse(pParse,
        "UPDATE \"%w\".sqlite_sequence set name = %Q WHERE name = %Q",
        zDb, zName, pTab->zName);
  }
#endif

  /* If the table being renamed is not itself part of the temp database,
  ** edit view and trigger definitions within the temp database
  ** as required.  */
  if( iDb!=1 ){
    sqlite3NestedParse(pParse,
        "UPDATE sqlite_temp_schema SET "
            "sql = sqlite_rename_table(%Q, type, name, sql, %Q, %Q, 1), "
            "tbl_name = "
              "CASE WHEN tbl_name=%Q COLLATE nocase AND "
              "  sqlite_rename_test(%Q, sql, type, name, 1, 'after rename', 0) "
              "THEN %Q ELSE tbl_name END "
            "WHERE type IN ('view', 'trigger')"
        , zDb, zTabName, zName, zTabName, zDb, zName);
  }

  /* If this is a virtual table, invoke the xRename() function if
  ** one is defined. The xRename() callback will modify the names
  ** of any resources used by the v-table implementation (including other
  ** SQLite tables) that are identified by the name of the virtual table.
  */
#ifndef SQLITE_OMIT_VIRTUALTABLE
  if( pVTab ){
    int i = ++pParse->nMem;
    sqlite3VdbeLoadString(v, i, zName);
    sqlite3VdbeAddOp4(v, OP_VRename, i, 0, 0,(const char*)pVTab, P4_VTAB);
  }
#endif

  renameReloadSchema(pParse, iDb, INITFLAG_AlterRename);
  renameTestSchema(pParse, zDb, iDb==1, "after rename", 0);

exit_rename_table:
  sqlite3SrcListDelete(db, pSrc);
  sqlite3DbFree(db, zName);
}

/*
** Write code that will raise an error if the table described by
** zDb and zTab is not empty.
*/
static void sqlite3ErrorIfNotEmpty(
  Parse *pParse,        /* Parsing context */
  const char *zDb,      /* Schema holding the table */
  const char *zTab,     /* Table to check for empty */
  const char *zErr      /* Error message text */
){
  sqlite3NestedParse(pParse,
     "SELECT raise(ABORT,%Q) FROM \"%w\".\"%w\"",
     zErr, zDb, zTab
  );
}

/*
** zCol is a column name used in an ALTER TABLE DROP, ADD or RENAME COLUMN
** operation. zOp identifies the specific operation - "drop", "add", "rename
** to" or "rename from". pTab is the table being altered.
**
** If pTab has a rowid and zCol is a rowid alias, then SQLITE_ERROR is 
** returned and an error message left in pParse. Or, if zCol is not an alias 
** for "rowid" or pTab is not an intkey table, then SQLITE_OK is returned.
*/
static int isRowidAlias(
  Parse *pParse, 
  Table *pTab, 
  const char *zCol, 
  const char *zOp
){
  if( HasRowid(pTab) && sqlite3IsRowid(zCol) ){
    sqlite3ErrorMsg(pParse, "cannot %s rowid alias: %s", zOp, zCol);
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}



/*
** This function is called after an "ALTER TABLE ... ADD" statement
** has been parsed. Argument pColDef contains the text of the new
** column definition.
**
** The Table structure pParse->pNewTable was extended to include
** the new column during parsing.
*/
void sqlite3AlterFinishAddColumn(Parse *pParse, Token *pColDef){
  Table *pNew;              /* Copy of pParse->pNewTable */
  Table *pTab;              /* Table being altered */
  int iDb;                  /* Database number */
  const char *zDb;          /* Database name */
  const char *zTab;         /* Table name */
  char *zCol;               /* Null-terminated column definition */
  Column *pCol;             /* The new column */
  Expr *pDflt;              /* Default value for the new column */
  sqlite3 *db;              /* The database connection; */
  Vdbe *v;                  /* The prepared statement under construction */
  int r1;                   /* Temporary registers */

  db = pParse->db;
  assert( db->pParse==pParse );
  if( pParse->nErr ) return;
  assert( db->mallocFailed==0 );
  pNew = pParse->pNewTable;
  assert( pNew );

  assert( sqlite3BtreeHoldsAllMutexes(db) );
  iDb = sqlite3SchemaToIndex(db, pNew->pSchema);
  zDb = db->aDb[iDb].zDbSName;
  zTab = &pNew->zName[16];  /* Skip the "sqlite_altertab_" prefix on the name */
  pCol = &pNew->aCol[pNew->nCol-1];
  pDflt = sqlite3ColumnExpr(pNew, pCol);
  pTab = sqlite3FindTable(db, zTab, zDb);
  assert( pTab );

#ifndef SQLITE_OMIT_AUTHORIZATION
  /* Invoke the authorization callback. */
  if( sqlite3AuthCheck(pParse, SQLITE_ALTER_TABLE, zDb, pTab->zName, 0) ){
    return;
  }
#endif


  /* Check that the new column is not specified as PRIMARY KEY or UNIQUE,
  ** or a rowid alias. If there is a NOT NULL constraint, then the default
  ** value for the column must not be NULL.
  */
  if( pCol->colFlags & COLFLAG_PRIMKEY ){
    sqlite3ErrorMsg(pParse, "Cannot add a PRIMARY KEY column");
    return;
  }
  if( pNew->pIndex ){
    sqlite3ErrorMsg(pParse,
         "Cannot add a UNIQUE column");
    return;
  }
  if( isRowidAlias(pParse, pTab, pCol->zCnName, "add") ) return;
  if( (pCol->colFlags & COLFLAG_GENERATED)==0 ){
    /* If the default value for the new column was specified with a
    ** literal NULL, then set pDflt to 0. This simplifies checking
    ** for an SQL NULL default below.
    */
    assert( pDflt==0 || pDflt->op==TK_SPAN );
    if( pDflt && pDflt->pLeft->op==TK_NULL ){
      pDflt = 0;
    }
    assert( IsOrdinaryTable(pNew) );
    if( (db->flags&SQLITE_ForeignKeys) && pNew->u.tab.pFKey && pDflt ){
      sqlite3ErrorIfNotEmpty(pParse, zDb, zTab,
          "Cannot add a REFERENCES column with non-NULL default value");
    }
    if( pCol->notNull && !pDflt ){
      sqlite3ErrorIfNotEmpty(pParse, zDb, zTab,
          "Cannot add a NOT NULL column with default value NULL");
    }


    /* Ensure the default expression is something that sqlite3ValueFromExpr()
    ** can handle (i.e. not CURRENT_TIME etc.)
    */
    if( pDflt ){
      sqlite3_value *pVal = 0;
      int rc;
      rc = sqlite3ValueFromExpr(db, pDflt, SQLITE_UTF8, SQLITE_AFF_BLOB, &pVal);
      assert( rc==SQLITE_OK || rc==SQLITE_NOMEM );
      if( rc!=SQLITE_OK ){
        assert( db->mallocFailed == 1 );
        return;
      }
      if( !pVal ){
        sqlite3ErrorIfNotEmpty(pParse, zDb, zTab,
           "Cannot add a column with non-constant default");
      }
      sqlite3ValueFree(pVal);
    }
  }else if( pCol->colFlags & COLFLAG_STORED ){
    sqlite3ErrorIfNotEmpty(pParse, zDb, zTab, "cannot add a STORED column");
  }


  /* Modify the CREATE TABLE statement. */
  zCol = sqlite3DbStrNDup(db, (char*)pColDef->z, pColDef->n);
  if( zCol ){
    char *zEnd = &zCol[pColDef->n-1];
    while( zEnd>zCol && (*zEnd==';' || sqlite3Isspace(*zEnd)) ){
      *zEnd-- = '\0';
    }
    /* substr() operations on characters, but addColOffset is in bytes. So we
    ** have to use printf() to translate between these units: */
    assert( IsOrdinaryTable(pTab) );
    assert( IsOrdinaryTable(pNew) );
    sqlite3NestedParse(pParse,
        "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
          "sql = printf('%%.%ds, ',sql) || %Q"
          " || substr(sql,1+length(printf('%%.%ds',sql))) "
        "WHERE type = 'table' AND name = %Q",
      zDb, pNew->u.tab.addColOffset, zCol, pNew->u.tab.addColOffset,
      zTab
    );
    sqlite3DbFree(db, zCol);
  }

  v = sqlite3GetVdbe(pParse);
  if( v ){
    /* Make sure the schema version is at least 3.  But do not upgrade
    ** from less than 3 to 4, as that will corrupt any preexisting DESC
    ** index.
    */
    r1 = sqlite3GetTempReg(pParse);
    sqlite3VdbeAddOp3(v, OP_ReadCookie, iDb, r1, BTREE_FILE_FORMAT);
    sqlite3VdbeUsesBtree(v, iDb);
    sqlite3VdbeAddOp2(v, OP_AddImm, r1, -2);
    sqlite3VdbeAddOp2(v, OP_IfPos, r1, sqlite3VdbeCurrentAddr(v)+2);
    VdbeCoverage(v);
    sqlite3VdbeAddOp3(v, OP_SetCookie, iDb, BTREE_FILE_FORMAT, 3);
    sqlite3ReleaseTempReg(pParse, r1);

    /* Reload the table definition */
    renameReloadSchema(pParse, iDb, INITFLAG_AlterAdd);

    /* Verify that constraints are still satisfied */
    if( pNew->pCheck!=0
     || (pCol->notNull && (pCol->colFlags & COLFLAG_GENERATED)!=0)
     || (pTab->tabFlags & TF_Strict)!=0
    ){
      pParse->colNamesSet = 1;
      sqlite3NestedParse(pParse,
        "SELECT CASE WHEN quick_check GLOB 'CHECK*'"
        " THEN raise(ABORT,'CHECK constraint failed')"
        " WHEN quick_check GLOB 'non-* value in*'"
        " THEN raise(ABORT,'type mismatch on DEFAULT')"
        " ELSE raise(ABORT,'NOT NULL constraint failed')"
        " END"
        "  FROM pragma_quick_check(%Q,%Q)"
        " WHERE quick_check GLOB 'CHECK*'"
        " OR quick_check GLOB 'NULL*'"
        " OR quick_check GLOB 'non-* value in*'",
        zTab, zDb
      );
    }
  }
}

/*
** This function is called by the parser after the table-name in
** an "ALTER TABLE <table-name> ADD" statement is parsed. Argument
** pSrc is the full-name of the table being altered.
**
** This routine makes a (partial) copy of the Table structure
** for the table being altered and sets Parse.pNewTable to point
** to it. Routines called by the parser as the column definition
** is parsed (i.e. sqlite3AddColumn()) add the new Column data to
** the copy. The copy of the Table structure is deleted by tokenize.c
** after parsing is finished.
**
** Routine sqlite3AlterFinishAddColumn() will be called to complete
** coding the "ALTER TABLE ... ADD" statement.
*/
void sqlite3AlterBeginAddColumn(Parse *pParse, SrcList *pSrc){
  Table *pNew;
  Table *pTab;
  int iDb;
  int i;
  int nAlloc;
  sqlite3 *db = pParse->db;

  /* Look up the table being altered. */
  assert( pParse->pNewTable==0 );
  assert( sqlite3BtreeHoldsAllMutexes(db) );
  if( NEVER(db->mallocFailed) ) goto exit_begin_add_column;
  pTab = sqlite3LocateTableItem(pParse, 0, &pSrc->a[0]);
  if( !pTab ) goto exit_begin_add_column;

#ifndef SQLITE_OMIT_VIRTUALTABLE
  if( IsVirtual(pTab) ){
    sqlite3ErrorMsg(pParse, "virtual tables may not be altered");
    goto exit_begin_add_column;
  }
#endif

  /* Make sure this is not an attempt to ALTER a view. */
  if( IsView(pTab) ){
    sqlite3ErrorMsg(pParse, "Cannot add a column to a view");
    goto exit_begin_add_column;
  }
  if( SQLITE_OK!=isAlterableTable(pParse, pTab) ){
    goto exit_begin_add_column;
  }

  sqlite3MayAbort(pParse);
  assert( IsOrdinaryTable(pTab) );
  assert( pTab->u.tab.addColOffset>0 );
  iDb = sqlite3SchemaToIndex(db, pTab->pSchema);

  /* Put a copy of the Table struct in Parse.pNewTable for the
  ** sqlite3AddColumn() function and friends to modify.  But modify
  ** the name by adding an "sqlite_altertab_" prefix.  By adding this
  ** prefix, we insure that the name will not collide with an existing
  ** table because user table are not allowed to have the "sqlite_"
  ** prefix on their name.
  */
  pNew = (Table*)sqlite3DbMallocZero(db, sizeof(Table));
  if( !pNew ) goto exit_begin_add_column;
  pParse->pNewTable = pNew;
  pNew->nTabRef = 1;
  pNew->nCol = pTab->nCol;
  assert( pNew->nCol>0 );
  nAlloc = (((pNew->nCol-1)/8)*8)+8;
  assert( nAlloc>=pNew->nCol && nAlloc%8==0 && nAlloc-pNew->nCol<8 );
  pNew->aCol = (Column*)sqlite3DbMallocZero(db, sizeof(Column)*(u32)nAlloc);
  pNew->zName = sqlite3MPrintf(db, "sqlite_altertab_%s", pTab->zName);
  if( !pNew->aCol || !pNew->zName ){
    assert( db->mallocFailed );
    goto exit_begin_add_column;
  }
  memcpy(pNew->aCol, pTab->aCol, sizeof(Column)*(size_t)pNew->nCol);
  for(i=0; i<pNew->nCol; i++){
    Column *pCol = &pNew->aCol[i];
    pCol->zCnName = sqlite3DbStrDup(db, pCol->zCnName);
    pCol->hName = sqlite3StrIHash(pCol->zCnName);
  }
  assert( IsOrdinaryTable(pNew) );
  pNew->u.tab.pDfltList = sqlite3ExprListDup(db, pTab->u.tab.pDfltList, 0);
  pNew->pSchema = db->aDb[iDb].pSchema;
  pNew->u.tab.addColOffset = pTab->u.tab.addColOffset;
  assert( pNew->nTabRef==1 );

exit_begin_add_column:
  sqlite3SrcListDelete(db, pSrc);
  return;
}

/*
** Parameter pTab is the subject of an ALTER TABLE ... RENAME COLUMN
** command. This function checks if the table is a view or virtual
** table (columns of views or virtual tables may not be renamed). If so,
** it loads an error message into pParse and returns non-zero.
**
** Or, if pTab is not a view or virtual table, zero is returned.
*/
#if !defined(SQLITE_OMIT_VIEW) || !defined(SQLITE_OMIT_VIRTUALTABLE)
static int isRealTable(Parse *pParse, Table *pTab, int iOp){
  const char *zType = 0;
#ifndef SQLITE_OMIT_VIEW
  if( IsView(pTab) ){
    zType = "view";
  }
#endif
#ifndef SQLITE_OMIT_VIRTUALTABLE
  if( IsVirtual(pTab) ){
    zType = "virtual table";
  }
#endif
  if( zType ){
    const char *azMsg[] = {
      "rename columns of", "drop column from", "edit constraints of",
      "set table options on"
    };
    assert( iOp>=0 && iOp<ArraySize(azMsg) );
    sqlite3ErrorMsg(pParse, "cannot %s %s \"%s\"",
        azMsg[iOp], zType, pTab->zName
    );
    return 1;
  }
  return 0;
}
#else /* !defined(SQLITE_OMIT_VIEW) || !defined(SQLITE_OMIT_VIRTUALTABLE) */
# define isRealTable(x,y,z) (0)
#endif

/*
** Handles the following parser reduction:
**
**  cmd ::= ALTER TABLE pSrc RENAME COLUMN pOld TO pNew
*/
void sqlite3AlterRenameColumn(
  Parse *pParse,                  /* Parsing context */
  SrcList *pSrc,                  /* Table being altered.  pSrc->nSrc==1 */
  Token *pOld,                    /* Name of column being changed */
  Token *pNew                     /* New column name */
){
  sqlite3 *db = pParse->db;       /* Database connection */
  Table *pTab;                    /* Table being updated */
  int iCol;                       /* Index of column being renamed */
  char *zOld = 0;                 /* Old column name */
  char *zNew = 0;                 /* New column name */
  const char *zDb;                /* Name of schema containing the table */
  int iSchema;                    /* Index of the schema */
  int bQuote;                     /* True to quote the new name */

  /* Locate the table to be altered */
  pTab = sqlite3LocateTableItem(pParse, 0, &pSrc->a[0]);
  if( !pTab ) goto exit_rename_column;

  /* Cannot alter a system table */
  if( SQLITE_OK!=isAlterableTable(pParse, pTab) ) goto exit_rename_column;
  if( SQLITE_OK!=isRealTable(pParse, pTab, 0) ) goto exit_rename_column;

  /* Which schema holds the table to be altered */ 
  iSchema = sqlite3SchemaToIndex(db, pTab->pSchema);
  assert( iSchema>=0 );
  zDb = db->aDb[iSchema].zDbSName;

#ifndef SQLITE_OMIT_AUTHORIZATION
  /* Invoke the authorization callback. */
  if( sqlite3AuthCheck(pParse, SQLITE_ALTER_TABLE, zDb, pTab->zName, 0) ){
    goto exit_rename_column;
  }
#endif

  /* Make sure the old name really is a column name in the table to be
  ** altered.  Set iCol to be the index of the column being renamed */
  zOld = sqlite3NameFromToken(db, pOld);
  if( !zOld ) goto exit_rename_column;
  iCol = sqlite3ColumnIndex(pTab, zOld);
  if( iCol<0 ){
    sqlite3ErrorMsg(pParse, "no such column: \"%T\"", pOld);
    goto exit_rename_column;
  }

  /* Ensure the schema contains no double-quoted strings */
  renameTestSchema(pParse, zDb, iSchema==1, "", 0);
  renameFixQuotes(pParse, zDb, iSchema==1);

  /* Do the rename operation using a recursive UPDATE statement that
  ** uses the sqlite_rename_column() SQL function to compute the new
  ** CREATE statement text for the sqlite_schema table.
  */
  sqlite3MayAbort(pParse);
  zNew = sqlite3NameFromToken(db, pNew);
  if( !zNew ) goto exit_rename_column;
  if( isRowidAlias(pParse, pTab, zOld, "rename from") ) goto exit_rename_column;
  if( isRowidAlias(pParse, pTab, zNew, "rename to") ) goto exit_rename_column;
  assert( pNew->n>0 );
  bQuote = sqlite3Isquote(pNew->z[0]);
  sqlite3NestedParse(pParse,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = sqlite_rename_column(sql, type, name, %Q, %Q, %d, %Q, %d, %d) "
      "WHERE name NOT LIKE 'sqliteX_%%' ESCAPE 'X' "
      " AND (type != 'index' OR tbl_name = %Q)",
      zDb,
      zDb, pTab->zName, iCol, zNew, bQuote, iSchema==1,
      pTab->zName
  );

  sqlite3NestedParse(pParse,
      "UPDATE temp." LEGACY_SCHEMA_TABLE " SET "
      "sql = sqlite_rename_column(sql, type, name, %Q, %Q, %d, %Q, %d, 1) "
      "WHERE type IN ('trigger', 'view')",
      zDb, pTab->zName, iCol, zNew, bQuote
  );

  /* Drop and reload the database schema. */
  renameReloadSchema(pParse, iSchema, INITFLAG_AlterRename);
  renameTestSchema(pParse, zDb, iSchema==1, "after rename", 1);

 exit_rename_column:
  sqlite3SrcListDelete(db, pSrc);
  sqlite3DbFree(db, zOld);
  sqlite3DbFree(db, zNew);
  return;
}

/*
** Each RenameToken object maps an element of the parse tree into
** the token that generated that element.  The parse tree element
** might be one of:
**
**     *  A pointer to an Expr that represents an ID
**     *  The name of a table column in Column.zName
**
** A list of RenameToken objects can be constructed during parsing.
** Each new object is created by sqlite3RenameTokenMap().
** As the parse tree is transformed, the sqlite3RenameTokenRemap()
** routine is used to keep the mapping current.
**
** After the parse finishes, renameTokenFind() routine can be used
** to look up the actual token value that created some element in
** the parse tree.
*/
struct RenameToken {
  const void *p;         /* Parse tree element created by token t */
  Token t;               /* The token that created parse tree element p */
  RenameToken *pNext;    /* Next is a list of all RenameToken objects */
};

struct ParseLoc {
  u8 eType;
  int iCol;
  Token t;
  ParseLoc *pNext;
};

/*
** The context of an ALTER TABLE RENAME COLUMN operation that gets passed
** down into the Walker.
*/
typedef struct RenameCtx RenameCtx;
struct RenameCtx {
  RenameToken *pList;             /* List of tokens to overwrite */
  int nList;                      /* Number of tokens in pList */
  int iCol;                       /* Index of column being renamed */
  Table *pTab;                    /* Table being ALTERed */
  const char *zOld;               /* Old column name */
};

#ifdef SQLITE_DEBUG
/*
** This function is only for debugging. It performs two tasks:
**
**   1. Checks that pointer pPtr does not already appear in the
**      rename-token list.
**
**   2. Dereferences each pointer in the rename-token list.
**
** The second is most effective when debugging under valgrind or
** address-sanitizer or similar. If any of these pointers no longer
** point to valid objects, an exception is raised by the memory-checking
** tool.
**
** The point of this is to prevent comparisons of invalid pointer values.
** Even though this always seems to work, it is undefined according to the
** C standard. Example of undefined comparison:
**
**     sqlite3_free(x);
**     if( x==y ) ...
**
** Technically, as x no longer points into a valid object or to the byte
** following a valid object, it may not be used in comparison operations.
*/
static void renameTokenCheckAll(Parse *pParse, const void *pPtr){
  assert( pParse==pParse->db->pParse );
  assert( pParse->db->mallocFailed==0 || pParse->nErr!=0 );
  if( pParse->nErr==0 ){
    const RenameToken *p;
    u32 i = 1;
    for(p=pParse->pRename; p; p=p->pNext){
      if( p->p ){
        assert( p->p!=pPtr );
        i += *(u8*)(p->p) | 1;
      }
    }
    assert( i>0 );
  }
}
#else
# define renameTokenCheckAll(x,y)
#endif

/*
** Remember that the parser tree element pPtr was created using
** the token pToken.
**
** In other words, construct a new RenameToken object and add it
** to the list of RenameToken objects currently being built up
** in pParse->pRename.
**
** The pPtr argument is returned so that this routine can be used
** with tail recursion in tokenExpr() routine, for a small performance
** improvement.
*/
const void *sqlite3RenameTokenMap(
  Parse *pParse,
  const void *pPtr,
  const Token *pToken
){
  RenameToken *pNew;
  assert( pPtr || pParse->db->mallocFailed );
  renameTokenCheckAll(pParse, pPtr);
  if( ALWAYS(pParse->eParseMode!=PARSE_MODE_UNMAP) ){
    pNew = sqlite3DbMallocZero(pParse->db, sizeof(RenameToken));
    if( pNew ){
      pNew->p = pPtr;
      pNew->t = *pToken;
      pNew->pNext = pParse->pRename;
      pParse->pRename = pNew;
    }
  }

  return pPtr;
}

/*
** It is assumed that there is already a RenameToken object associated
** with parse tree element pFrom. This function remaps the associated token
** to parse tree element pTo.
*/
void sqlite3RenameTokenRemap(Parse *pParse, const void *pTo, const void *pFrom){
  RenameToken *p;
  renameTokenCheckAll(pParse, pTo);
  for(p=pParse->pRename; p; p=p->pNext){
    if( p->p==pFrom ){
      p->p = pTo;
      break;
    }
  }
}

/*
** Walker callback used by sqlite3RenameExprUnmap().
*/
static int renameUnmapExprCb(Walker *pWalker, Expr *pExpr){
  Parse *pParse = pWalker->pParse;
  sqlite3RenameTokenRemap(pParse, 0, (const void*)pExpr);
  if( ExprUseYTab(pExpr) ){
    sqlite3RenameTokenRemap(pParse, 0, (const void*)&pExpr->y.pTab);
  }
  return WRC_Continue;
}

/*
** Iterate through the Select objects that are part of WITH clauses attached
** to select statement pSelect.
*/
static void renameWalkWith(Walker *pWalker, Select *pSelect){
  With *pWith = pSelect->pWith;
  if( pWith ){
    Parse *pParse = pWalker->pParse;
    int i;
    With *pCopy = 0;
    assert( pWith->nCte>0 );
    if( (pWith->a[0].pSelect->selFlags & SF_Expanded)==0 ){
      /* Push a copy of the With object onto the with-stack. We use a copy
      ** here as the original will be expanded and resolved (flags SF_Expanded
      ** and SF_Resolved) below. And the parser code that uses the with-stack
      ** fails if the Select objects on it have already been expanded and
      ** resolved.  */
      pCopy = sqlite3WithDup(pParse->db, pWith);
      pCopy = sqlite3WithPush(pParse, pCopy, 1);
    }
    for(i=0; i<pWith->nCte; i++){
      Select *p = pWith->a[i].pSelect;
      NameContext sNC;
      memset(&sNC, 0, sizeof(sNC));
      sNC.pParse = pParse;
      if( pCopy ) sqlite3SelectPrep(sNC.pParse, p, &sNC);
      if( sNC.pParse->db->mallocFailed ) return;
      sqlite3WalkSelect(pWalker, p);
      sqlite3RenameExprlistUnmap(pParse, pWith->a[i].pCols);
    }
    if( pCopy && pParse->pWith==pCopy ){
      pParse->pWith = pCopy->pOuter;
    }
  }
}

/*
** Unmap all tokens in the IdList object passed as the second argument.
*/
static void unmapColumnIdlistNames(
  Parse *pParse,
  const IdList *pIdList
){
  int ii;
  assert( pIdList!=0 );
  for(ii=0; ii<pIdList->nId; ii++){
    sqlite3RenameTokenRemap(pParse, 0, (const void*)pIdList->a[ii].zName);
  }
}

/*
** Walker callback used by sqlite3RenameExprUnmap().
*/
static int renameUnmapSelectCb(Walker *pWalker, Select *p){
  Parse *pParse = pWalker->pParse;
  int i;
  if( pParse->nErr ) return WRC_Abort;
  testcase( p->selFlags & SF_View );
  testcase( p->selFlags & SF_CopyCte );
  if( p->selFlags & (SF_View|SF_CopyCte) ){
    return WRC_Prune;
  }
  if( ALWAYS(p->pEList) ){
    ExprList *pList = p->pEList;
    for(i=0; i<pList->nExpr; i++){
      if( pList->a[i].zEName && pList->a[i].fg.eEName==ENAME_NAME ){
        sqlite3RenameTokenRemap(pParse, 0, (void*)pList->a[i].zEName);
      }
    }
  }
  if( ALWAYS(p->pSrc) ){  /* Every Select as a SrcList, even if it is empty */
    SrcList *pSrc = p->pSrc;
    for(i=0; i<pSrc->nSrc; i++){
      sqlite3RenameTokenRemap(pParse, 0, (void*)pSrc->a[i].zName);
      if( pSrc->a[i].fg.isUsing==0 ){
        sqlite3WalkExpr(pWalker, pSrc->a[i].u3.pOn);
      }else{
        unmapColumnIdlistNames(pParse, pSrc->a[i].u3.pUsing);
      }
    }
  }

  renameWalkWith(pWalker, p);
  return WRC_Continue;
}

/*
** Remove all nodes that are part of expression pExpr from the rename list.
*/
void sqlite3RenameExprUnmap(Parse *pParse, Expr *pExpr){
  u8 eMode = pParse->eParseMode;
  Walker sWalker;
  memset(&sWalker, 0, sizeof(Walker));
  sWalker.pParse = pParse;
  sWalker.xExprCallback = renameUnmapExprCb;
  sWalker.xSelectCallback = renameUnmapSelectCb;
  pParse->eParseMode = PARSE_MODE_UNMAP;
  sqlite3WalkExpr(&sWalker, pExpr);
  pParse->eParseMode = eMode;
}

/*
** Remove all nodes that are part of expression-list pEList from the
** rename list.
*/
void sqlite3RenameExprlistUnmap(Parse *pParse, ExprList *pEList){
  if( pEList ){
    int i;
    Walker sWalker;
    memset(&sWalker, 0, sizeof(Walker));
    sWalker.pParse = pParse;
    sWalker.xExprCallback = renameUnmapExprCb;
    sqlite3WalkExprList(&sWalker, pEList);
    for(i=0; i<pEList->nExpr; i++){
      if( ALWAYS(pEList->a[i].fg.eEName==ENAME_NAME) ){
        sqlite3RenameTokenRemap(pParse, 0, (void*)pEList->a[i].zEName);
      }
    }
  }
}

/*
** Free the list of RenameToken objects given in the second argument
*/
static void renameTokenFree(sqlite3 *db, RenameToken *pToken){
  RenameToken *pNext;
  RenameToken *p;
  for(p=pToken; p; p=pNext){
    pNext = p->pNext;
    sqlite3DbFree(db, p);
  }
}

/*
** Search the Parse object passed as the first argument for a RenameToken
** object associated with parse tree element pPtr. If found, return a pointer
** to it. Otherwise, return NULL.
**
** If the second argument passed to this function is not NULL and a matching
** RenameToken object is found, remove it from the Parse object and add it to
** the list maintained by the RenameCtx object.
*/
static RenameToken *renameTokenFind(
  Parse *pParse,
  struct RenameCtx *pCtx,
  const void *pPtr
){
  RenameToken **pp;
  if( NEVER(pPtr==0) ){
    return 0;
  }
  for(pp=&pParse->pRename; (*pp); pp=&(*pp)->pNext){
    if( (*pp)->p==pPtr ){
      RenameToken *pToken = *pp;
      if( pCtx ){
        *pp = pToken->pNext;
        pToken->pNext = pCtx->pList;
        pCtx->pList = pToken;
        pCtx->nList++;
      }
      return pToken;
    }
  }
  return 0;
}

/*
** This is a Walker select callback. It does nothing. It is only required
** because without a dummy callback, sqlite3WalkExpr() and similar do not
** descend into sub-select statements.
*/
static int renameColumnSelectCb(Walker *pWalker, Select *p){
  if( p->selFlags & (SF_View|SF_CopyCte) ){
    testcase( p->selFlags & SF_View );
    testcase( p->selFlags & SF_CopyCte );
    return WRC_Prune;
  }
  renameWalkWith(pWalker, p);
  return WRC_Continue;
}

/*
** This is a Walker expression callback.
**
** For every TK_COLUMN node in the expression tree, search to see
** if the column being references is the column being renamed by an
** ALTER TABLE statement.  If it is, then attach its associated
** RenameToken object to the list of RenameToken objects being
** constructed in RenameCtx object at pWalker->u.pRename.
*/
static int renameColumnExprCb(Walker *pWalker, Expr *pExpr){
  RenameCtx *p = pWalker->u.pRename;
  if( pExpr->op==TK_TRIGGER
   && pExpr->iColumn==p->iCol
   && pWalker->pParse->pTriggerTab==p->pTab
  ){
    renameTokenFind(pWalker->pParse, p, (void*)pExpr);
  }else if( pExpr->op==TK_COLUMN
   && pExpr->iColumn==p->iCol
   && ALWAYS(ExprUseYTab(pExpr))
   && p->pTab==pExpr->y.pTab
  ){
    renameTokenFind(pWalker->pParse, p, (void*)pExpr);
  }
  return WRC_Continue;
}

/*
** The RenameCtx contains a list of tokens that reference a column that
** is being renamed by an ALTER TABLE statement.  Return the "last"
** RenameToken in the RenameCtx and remove that RenameToken from the
** RenameContext.  "Last" means the last RenameToken encountered when
** the input SQL is parsed from left to right.  Repeated calls to this routine
** return all column name tokens in the order that they are encountered
** in the SQL statement.
*/
static RenameToken *renameColumnTokenNext(RenameCtx *pCtx){
  RenameToken *pBest = pCtx->pList;
  RenameToken *pToken;
  RenameToken **pp;

  for(pToken=pBest->pNext; pToken; pToken=pToken->pNext){
    if( pToken->t.z>pBest->t.z ) pBest = pToken;
  }
  for(pp=&pCtx->pList; *pp!=pBest; pp=&(*pp)->pNext);
  *pp = pBest->pNext;

  return pBest;
}

/*
** Set the error message of the context passed as the first argument to
** the result of formatting zFmt using printf() style formatting.
*/
static void errorMPrintf(sqlite3_context *pCtx, const char *zFmt, ...){
  sqlite3 *db = sqlite3_context_db_handle(pCtx);
  char *zErr = 0;
  va_list ap;
  va_start(ap, zFmt);
  zErr = sqlite3VMPrintf(db, zFmt, ap);
  va_end(ap);
  if( zErr ){
    sqlite3_result_error(pCtx, zErr, -1);
    sqlite3DbFree(db, zErr);
  }else{
    sqlite3_result_error_nomem(pCtx);
  }
}

/*
** An error occurred while parsing or otherwise processing a database
** object (either pParse->pNewTable, pNewIndex or pNewTrigger) as part of an
** ALTER TABLE RENAME COLUMN program. The error message emitted by the
** sub-routine is currently stored in pParse->zErrMsg. This function
** adds context to the error message and then stores it in pCtx.
*/
static void renameColumnParseError(
  sqlite3_context *pCtx,
  const char *zWhen,
  sqlite3_value *pType,
  sqlite3_value *pObject,
  Parse *pParse
){
  const char *zT = (const char*)sqlite3_value_text(pType);
  const char *zN = (const char*)sqlite3_value_text(pObject);
  char *zErr;

  zErr = sqlite3MPrintf(pParse->db, "error in %s %s%s%s: %s",
      zT, zN, (zWhen[0] ? " " : ""), zWhen,
      pParse->zErrMsg
  );
  sqlite3_result_error(pCtx, zErr, -1);
  sqlite3DbFree(pParse->db, zErr);
}

/*
** For each name in the the expression-list pEList (i.e. each
** pEList->a[i].zName) that matches the string in zOld, extract the
** corresponding rename-token from Parse object pParse and add it
** to the RenameCtx pCtx.
*/
static void renameColumnElistNames(
  Parse *pParse,
  RenameCtx *pCtx,
  const ExprList *pEList,
  const char *zOld
){
  if( pEList ){
    int i;
    for(i=0; i<pEList->nExpr; i++){
      const char *zName = pEList->a[i].zEName;
      if( ALWAYS(pEList->a[i].fg.eEName==ENAME_NAME)
       && ALWAYS(zName!=0)
       && 0==sqlite3_stricmp(zName, zOld)
      ){
        renameTokenFind(pParse, pCtx, (const void*)zName);
      }
    }
  }
}

/*
** For each name in the the id-list pIdList (i.e. each pIdList->a[i].zName)
** that matches the string in zOld, extract the corresponding rename-token
** from Parse object pParse and add it to the RenameCtx pCtx.
*/
static void renameColumnIdlistNames(
  Parse *pParse,
  RenameCtx *pCtx,
  const IdList *pIdList,
  const char *zOld
){
  if( pIdList ){
    int i;
    for(i=0; i<pIdList->nId; i++){
      const char *zName = pIdList->a[i].zName;
      if( 0==sqlite3_stricmp(zName, zOld) ){
        renameTokenFind(pParse, pCtx, (const void*)zName);
      }
    }
  }
}


/*
** Parse the SQL statement zSql using Parse object (*p). The Parse object
** is initialized by this function before it is used.
*/
static int renameParseSql(
  Parse *p,                       /* Memory to use for Parse object */
  const char *zDb,                /* Name of schema SQL belongs to */
  sqlite3 *db,                    /* Database handle */
  const char *zSql,               /* SQL to parse */
  int bTemp                       /* True if SQL is from temp schema */
){
  int rc;
  u64 flags;

  sqlite3ParseObjectInit(p, db);
  if( zSql==0 ){
    return SQLITE_NOMEM;
  }
  if( sqlite3StrNICmp(zSql,"CREATE ",7)!=0 ){
    return SQLITE_CORRUPT_BKPT;
  }
  if( bTemp ){
    db->init.iDb = 1;
  }else{
    int iDb = sqlite3FindDbName(db, zDb);
    assert( iDb>=0 && iDb<=0xff );
    db->init.iDb = (u8)iDb;
  }
  p->eParseMode = PARSE_MODE_RENAME;
  p->db = db;
  p->nQueryLoop = 1;
  flags = db->flags;
  testcase( (db->flags & SQLITE_Comments)==0 && strstr(zSql," /* ")!=0 );
  db->flags |= SQLITE_Comments;
  rc = sqlite3RunParser(p, zSql);
  db->flags = flags;
  if( db->mallocFailed ) rc = SQLITE_NOMEM;
  if( rc==SQLITE_OK
   && NEVER(p->pNewTable==0 && p->pNewIndex==0 && p->pNewTrigger==0)
  ){
    rc = SQLITE_CORRUPT_BKPT;
  }

#ifdef SQLITE_DEBUG
  /* Ensure that all mappings in the Parse.pRename list really do map to
  ** a part of the input string.  */
  if( rc==SQLITE_OK ){
    int nSql = sqlite3Strlen30(zSql);
    RenameToken *pToken;
    ParseLoc *pLoc;
    for(pToken=p->pRename; pToken; pToken=pToken->pNext){
      assert( pToken->t.z>=zSql && &pToken->t.z[pToken->t.n]<=&zSql[nSql] );
    }
    for(pLoc=p->pLoc; pLoc; pLoc=pLoc->pNext){
      assert( pLoc->t.z>=zSql && &pLoc->t.z[pLoc->t.n]<=&zSql[nSql] );
      assert( pLoc->eType!=PARSELOC_NotNull || pLoc->t.n>0 );
    }
  }
#endif

  db->init.iDb = 0;
  return rc;
}

/*
** This function edits SQL statement zSql, replacing each token identified
** by the linked list pRename with the text of zNew. If argument bQuote is
** true, then zNew is always quoted first. If no error occurs, the result
** is loaded into context object pCtx as the result.
**
** Or, if an error occurs (i.e. an OOM condition), an error is left in
** pCtx and an SQLite error code returned.
*/
static int renameEditSql(
  sqlite3_context *pCtx,          /* Return result here */
  RenameCtx *pRename,             /* Rename context */
  const char *zSql,               /* SQL statement to edit */
  const char *zNew,               /* New token text */
  int bQuote                      /* True to always quote token */
){
  i64 nNew = sqlite3Strlen30(zNew);
  i64 nSql = sqlite3Strlen30(zSql);
  sqlite3 *db = sqlite3_context_db_handle(pCtx);
  int rc = SQLITE_OK;
  char *zQuot = 0;
  char *zOut;
  i64 nQuot = 0;
  char *zBuf1 = 0;
  char *zBuf2 = 0;

  if( zNew ){
    /* Set zQuot to point to a buffer containing a quoted copy of the
    ** identifier zNew. If the corresponding identifier in the original
    ** ALTER TABLE statement was quoted (bQuote==1), then set zNew to
    ** point to zQuot so that all substitutions are made using the
    ** quoted version of the new column name.  */
    zQuot = sqlite3MPrintf(db, "\"%w\" ", zNew);
    if( zQuot==0 ){
      return SQLITE_NOMEM;
    }else{
      nQuot = sqlite3Strlen30(zQuot)-1;
    }

    assert( nQuot>=nNew && nSql>=0 && nNew>=0 );
    zOut = sqlite3DbMallocZero(db, (u64)nSql + pRename->nList*(u64)nQuot + 1);
  }else{
    assert( nSql>0 );
    zOut = (char*)sqlite3DbMallocZero(db, (2*(u64)nSql + 1) * 3);
    if( zOut ){
      zBuf1 = &zOut[nSql*2+1];
      zBuf2 = &zOut[nSql*4+2];
    }
  }

  /* At this point pRename->pList contains a list of RenameToken objects
  ** corresponding to all tokens in the input SQL that must be replaced
  ** with the new column name, or with single-quoted versions of themselves.
  ** All that remains is to construct and return the edited SQL string. */
  if( zOut ){
    i64 nOut = nSql;
    assert( nSql>0 );
    memcpy(zOut, zSql, (size_t)nSql);
    while( pRename->pList ){
      int iOff;                   /* Offset of token to replace in zOut */
      i64 nReplace;
      const char *zReplace;
      RenameToken *pBest = renameColumnTokenNext(pRename);

      if( zNew ){
        if( bQuote==0 && sqlite3IsIdChar(*(u8*)pBest->t.z) ){
          nReplace = nNew;
          zReplace = zNew;
        }else{
          nReplace = nQuot;
          zReplace = zQuot;
          if( pBest->t.z[pBest->t.n]=='"' ) nReplace++;
        }
      }else{
        /* Dequote the double-quoted token. Then requote it again, this time
        ** using single quotes. If the character immediately following the
        ** original token within the input SQL was a single quote ('), then
        ** add another space after the new, single-quoted version of the
        ** token. This is so that (SELECT "string"'alias') maps to
        ** (SELECT 'string' 'alias'), and not (SELECT 'string''alias').  */
        memcpy(zBuf1, pBest->t.z, pBest->t.n);
        zBuf1[pBest->t.n] = 0;
        sqlite3Dequote(zBuf1);
        assert( nSql < 0x15555554 /* otherwise malloc would have failed */ );
        sqlite3_snprintf((int)(nSql*2), zBuf2, "%Q%s", zBuf1,
            pBest->t.z[pBest->t.n]=='\'' ? " " : ""
        );
        zReplace = zBuf2;
        nReplace = sqlite3Strlen30(zReplace);
      }

      iOff = (int)(pBest->t.z - zSql);
      if( pBest->t.n!=nReplace ){
        memmove(&zOut[iOff + nReplace], &zOut[iOff + pBest->t.n],
            nOut - (iOff + pBest->t.n)
        );
        nOut += nReplace - pBest->t.n;
        zOut[nOut] = '\0';
      }
      memcpy(&zOut[iOff], zReplace, nReplace);
      sqlite3DbFree(db, pBest);
    }

    sqlite3_result_text(pCtx, zOut, -1, SQLITE_TRANSIENT);
    sqlite3DbFree(db, zOut);
  }else{
    rc = SQLITE_NOMEM;
  }

  sqlite3_free(zQuot);
  return rc;
}

/*
** Set all pEList->a[].fg.eEName fields in the expression-list to val.
*/
static void renameSetENames(ExprList *pEList, int val){
  assert( val==ENAME_NAME || val==ENAME_TAB || val==ENAME_SPAN );
  if( pEList ){
    int i;
    for(i=0; i<pEList->nExpr; i++){
      assert( val==ENAME_NAME || pEList->a[i].fg.eEName==ENAME_NAME );
      pEList->a[i].fg.eEName = val&0x3;
    }
  }
}

/*
** Resolve all symbols in the trigger at pParse->pNewTrigger, assuming
** it was read from the schema of database zDb. Return SQLITE_OK if
** successful. Otherwise, return an SQLite error code and leave an error
** message in the Parse object.
*/
static int renameResolveTrigger(Parse *pParse){
  sqlite3 *db = pParse->db;
  Trigger *pNew = pParse->pNewTrigger;
  TriggerStep *pStep;
  NameContext sNC;
  int rc = SQLITE_OK;

  memset(&sNC, 0, sizeof(sNC));
  sNC.pParse = pParse;
  assert( pNew->pTabSchema );
  pParse->pTriggerTab = sqlite3FindTable(db, pNew->table,
      db->aDb[sqlite3SchemaToIndex(db, pNew->pTabSchema)].zDbSName
  );
  pParse->eTriggerOp = pNew->op;
  /* ALWAYS() because if the table of the trigger does not exist, the
  ** error would have been hit before this point */
  if( ALWAYS(pParse->pTriggerTab) ){
    rc = sqlite3ViewGetColumnNames(pParse, pParse->pTriggerTab)!=0;
  }

  /* Resolve symbols in WHEN clause */
  if( rc==SQLITE_OK && pNew->pWhen ){
    rc = sqlite3ResolveExprNames(&sNC, pNew->pWhen);
  }

  for(pStep=pNew->step_list; rc==SQLITE_OK && pStep; pStep=pStep->pNext){
    if( pStep->pSelect ){
      sqlite3SelectPrep(pParse, pStep->pSelect, &sNC);
      if( pParse->nErr ) rc = pParse->rc;
    }
    if( rc==SQLITE_OK && pStep->pSrc ){
      SrcList *pSrc = sqlite3SrcListDup(db, pStep->pSrc, 0);
      if( pSrc ){
        Select *pSel = sqlite3SelectNew(
            pParse, pStep->pExprList, pSrc, 0, 0, 0, 0, 0, 0
        );
        if( pSel==0 ){
          pStep->pExprList = 0;
          pSrc = 0;
          rc = SQLITE_NOMEM;
        }else{
          /* pStep->pExprList contains an expression-list used for an UPDATE
          ** statement. So the a[].zEName values are the RHS of the
          ** "<col> = <expr>" clauses of the UPDATE statement. So, before
          ** running SelectPrep(), change all the eEName values in
          ** pStep->pExprList to ENAME_SPAN (from their current value of
          ** ENAME_NAME). This is to prevent any ids in ON() clauses that are
          ** part of pSrc from being incorrectly resolved against the
          ** a[].zEName values as if they were column aliases.  */
          renameSetENames(pStep->pExprList, ENAME_SPAN);
          sqlite3SelectPrep(pParse, pSel, 0);
          renameSetENames(pStep->pExprList, ENAME_NAME);
          rc = pParse->nErr ? SQLITE_ERROR : SQLITE_OK;
          assert( pStep->pExprList==0 || pStep->pExprList==pSel->pEList );
          assert( pSrc==pSel->pSrc );
          if( pStep->pExprList ) pSel->pEList = 0;
          pSel->pSrc = 0;
          sqlite3SelectDelete(db, pSel);
        }
        if( ALWAYS(pStep->pSrc) ){
          int i;
          for(i=0; i<pStep->pSrc->nSrc && rc==SQLITE_OK; i++){
            SrcItem *p = &pStep->pSrc->a[i];
            if( p->fg.isSubquery ){
              assert( p->u4.pSubq!=0 );
              sqlite3SelectPrep(pParse, p->u4.pSubq->pSelect, 0);
            }
          }
        }

        if(  db->mallocFailed ){
          rc = SQLITE_NOMEM;
        }
        sNC.pSrcList = pSrc;
        if( rc==SQLITE_OK && pStep->pWhere ){
          rc = sqlite3ResolveExprNames(&sNC, pStep->pWhere);
        }
        if( rc==SQLITE_OK ){
          rc = sqlite3ResolveExprListNames(&sNC, pStep->pExprList);
        }
        assert( !pStep->pUpsert || (!pStep->pWhere && !pStep->pExprList) );
        if( pStep->pUpsert && rc==SQLITE_OK ){
          Upsert *pUpsert = pStep->pUpsert;
          pUpsert->pUpsertSrc = pSrc;
          sNC.uNC.pUpsert = pUpsert;
          sNC.ncFlags = NC_UUpsert;
          rc = sqlite3ResolveExprListNames(&sNC, pUpsert->pUpsertTarget);
          if( rc==SQLITE_OK ){
            ExprList *pUpsertSet = pUpsert->pUpsertSet;
            rc = sqlite3ResolveExprListNames(&sNC, pUpsertSet);
          }
          if( rc==SQLITE_OK ){
            rc = sqlite3ResolveExprNames(&sNC, pUpsert->pUpsertWhere);
          }
          if( rc==SQLITE_OK ){
            rc = sqlite3ResolveExprNames(&sNC, pUpsert->pUpsertTargetWhere);
          }
          sNC.ncFlags = 0;
        }
        sNC.pSrcList = 0;
        sqlite3SrcListDelete(db, pSrc);
      }else{
        rc = SQLITE_NOMEM;
      }
    }
  }
  return rc;
}

/*
** Invoke sqlite3WalkExpr() or sqlite3WalkSelect() on all Select or Expr
** objects that are part of the trigger passed as the second argument.
*/
static void renameWalkTrigger(Walker *pWalker, Trigger *pTrigger){
  TriggerStep *pStep;

  /* Find tokens to edit in WHEN clause */
  sqlite3WalkExpr(pWalker, pTrigger->pWhen);

  /* Find tokens to edit in trigger steps */
  for(pStep=pTrigger->step_list; pStep; pStep=pStep->pNext){
    sqlite3WalkSelect(pWalker, pStep->pSelect);
    sqlite3WalkExpr(pWalker, pStep->pWhere);
    sqlite3WalkExprList(pWalker, pStep->pExprList);
    if( pStep->pUpsert ){
      Upsert *pUpsert = pStep->pUpsert;
      sqlite3WalkExprList(pWalker, pUpsert->pUpsertTarget);
      sqlite3WalkExprList(pWalker, pUpsert->pUpsertSet);
      sqlite3WalkExpr(pWalker, pUpsert->pUpsertWhere);
      sqlite3WalkExpr(pWalker, pUpsert->pUpsertTargetWhere);
    }
    if( pStep->pSrc ){
      int i;
      SrcList *pSrc = pStep->pSrc;
      for(i=0; i<pSrc->nSrc; i++){
        if( pSrc->a[i].fg.isSubquery ){
          assert( pSrc->a[i].u4.pSubq!=0 );
          sqlite3WalkSelect(pWalker, pSrc->a[i].u4.pSubq->pSelect);
        }
      }
    }
  }
}

/*
** Free the contents of Parse object (*pParse). Do not free the memory
** occupied by the Parse object itself.
*/
static void renameParseCleanup(Parse *pParse){
  sqlite3 *db = pParse->db;
  Index *pIdx;
  if( pParse->pVdbe ){
    sqlite3VdbeFinalize(pParse->pVdbe);
  }
  sqlite3DeleteTable(db, pParse->pNewTable);
  while( (pIdx = pParse->pNewIndex)!=0 ){
    pParse->pNewIndex = pIdx->pNext;
    sqlite3FreeIndex(db, pIdx);
  }
  sqlite3DeleteTrigger(db, pParse->pNewTrigger);
  sqlite3DbFree(db, pParse->zErrMsg);
  renameTokenFree(db, pParse->pRename);
  sqlite3ParseLocFree(db, pParse->pLoc);
  sqlite3ParseObjectReset(pParse);
}

/*
** SQL function:
**
**     sqlite_rename_column(SQL,TYPE,OBJ,DB,TABLE,COL,NEWNAME,QUOTE,TEMP)
**
**   0. zSql:     SQL statement to rewrite
**   1. type:     Type of object ("table", "view" etc.)
**   2. object:   Name of object
**   3. Database: Database name (e.g. "main")
**   4. Table:    Table name
**   5. iCol:     Index of column to rename
**   6. zNew:     New column name
**   7. bQuote:   Non-zero if the new column name should be quoted.
**   8. bTemp:    True if zSql comes from temp schema
**
** Do a column rename operation on the CREATE statement given in zSql.
** The iCol-th column (left-most is 0) of table zTable is renamed from zCol
** into zNew.  The name should be quoted if bQuote is true.
**
** This function is used internally by the ALTER TABLE RENAME COLUMN command.
** It is only accessible to SQL created using sqlite3NestedParse().  It is
** not reachable from ordinary SQL passed into sqlite3_prepare() unless the
** SQLITE_TESTCTRL_INTERNAL_FUNCTIONS test setting is enabled.
*/
static void renameColumnFunc(
  sqlite3_context *context,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(context);
  RenameCtx sCtx;
  const char *zSql = (const char*)sqlite3_value_text(argv[0]);
  const char *zDb = (const char*)sqlite3_value_text(argv[3]);
  const char *zTable = (const char*)sqlite3_value_text(argv[4]);
  int iCol = sqlite3_value_int(argv[5]);
  const char *zNew = (const char*)sqlite3_value_text(argv[6]);
  int bQuote = sqlite3_value_int(argv[7]);
  int bTemp = sqlite3_value_int(argv[8]);
  const char *zOld;
  int rc;
  Parse sParse;
  Walker sWalker;
  Index *pIdx;
  int i;
  Table *pTab;
#ifndef SQLITE_OMIT_AUTHORIZATION
  sqlite3_xauth xAuth = db->xAuth;
#endif

  UNUSED_PARAMETER(NotUsed);
  if( zSql==0 ) return;
  if( zTable==0 ) return;
  if( zNew==0 ) return;
  if( iCol<0 ) return;
  sqlite3BtreeEnterAll(db);
  pTab = sqlite3FindTable(db, zTable, zDb);
  if( pTab==0 || iCol>=pTab->nCol ){
    sqlite3BtreeLeaveAll(db);
    return;
  }
  zOld = pTab->aCol[iCol].zCnName;
  memset(&sCtx, 0, sizeof(sCtx));
  sCtx.iCol = ((iCol==pTab->iPKey) ? -1 : iCol);

#ifndef SQLITE_OMIT_AUTHORIZATION
  db->xAuth = 0;
#endif
  rc = renameParseSql(&sParse, zDb, db, zSql, bTemp);

  /* Find tokens that need to be replaced. */
  memset(&sWalker, 0, sizeof(Walker));
  sWalker.pParse = &sParse;
  sWalker.xExprCallback = renameColumnExprCb;
  sWalker.xSelectCallback = renameColumnSelectCb;
  sWalker.u.pRename = &sCtx;

  sCtx.pTab = pTab;
  if( rc!=SQLITE_OK ) goto renameColumnFunc_done;
  if( sParse.pNewTable ){
    if( IsView(sParse.pNewTable) ){
      Select *pSelect = sParse.pNewTable->u.view.pSelect;
      pSelect->selFlags &= ~(u32)SF_View;
      sParse.rc = SQLITE_OK;
      sqlite3SelectPrep(&sParse, pSelect, 0);
      rc = (db->mallocFailed ? SQLITE_NOMEM : sParse.rc);
      if( rc==SQLITE_OK ){
        sqlite3WalkSelect(&sWalker, pSelect);
      }
      if( rc!=SQLITE_OK ) goto renameColumnFunc_done;
    }else if( IsOrdinaryTable(sParse.pNewTable) ){
      /* A regular table */
      int bFKOnly = sqlite3_stricmp(zTable, sParse.pNewTable->zName);
      FKey *pFKey;
      sCtx.pTab = sParse.pNewTable;
      if( bFKOnly==0 ){
        if( iCol<sParse.pNewTable->nCol ){
          renameTokenFind(
              &sParse, &sCtx, (void*)sParse.pNewTable->aCol[iCol].zCnName
          );
        }
        if( sCtx.iCol<0 ){
          renameTokenFind(&sParse, &sCtx, (void*)&sParse.pNewTable->iPKey);
        }
        sqlite3WalkExprList(&sWalker, sParse.pNewTable->pCheck);
        for(pIdx=sParse.pNewTable->pIndex; pIdx; pIdx=pIdx->pNext){
          sqlite3WalkExprList(&sWalker, pIdx->aColExpr);
        }
        for(pIdx=sParse.pNewIndex; pIdx; pIdx=pIdx->pNext){
          sqlite3WalkExprList(&sWalker, pIdx->aColExpr);
        }
#ifndef SQLITE_OMIT_GENERATED_COLUMNS
        for(i=0; i<sParse.pNewTable->nCol; i++){
          Expr *pExpr = sqlite3ColumnExpr(sParse.pNewTable,
                                                  &sParse.pNewTable->aCol[i]);
          sqlite3WalkExpr(&sWalker, pExpr);
        }
#endif
      }

      assert( IsOrdinaryTable(sParse.pNewTable) );
      for(pFKey=sParse.pNewTable->u.tab.pFKey; pFKey; pFKey=pFKey->pNextFrom){
        for(i=0; i<pFKey->nCol; i++){
          if( bFKOnly==0 && pFKey->aCol[i].iFrom==iCol ){
            renameTokenFind(&sParse, &sCtx, (void*)&pFKey->aCol[i]);
          }
          if( 0==sqlite3_stricmp(pFKey->zTo, zTable)
           && 0==sqlite3_stricmp(pFKey->aCol[i].zCol, zOld)
          ){
            renameTokenFind(&sParse, &sCtx, (void*)pFKey->aCol[i].zCol);
          }
        }
      }
    }
  }else if( sParse.pNewIndex ){
    sqlite3WalkExprList(&sWalker, sParse.pNewIndex->aColExpr);
    sqlite3WalkExpr(&sWalker, sParse.pNewIndex->pPartIdxWhere);
  }else{
    /* A trigger */
    TriggerStep *pStep;
    rc = renameResolveTrigger(&sParse);
    if( rc!=SQLITE_OK ) goto renameColumnFunc_done;

    for(pStep=sParse.pNewTrigger->step_list; pStep; pStep=pStep->pNext){
      if( pStep->pSrc ){
        Table *pTarget = sqlite3LocateTableItem(&sParse, 0, &pStep->pSrc->a[0]);
        if( pTarget==pTab ){
          if( pStep->pUpsert ){
            ExprList *pUpsertSet = pStep->pUpsert->pUpsertSet;
            renameColumnElistNames(&sParse, &sCtx, pUpsertSet, zOld);
          }
          renameColumnIdlistNames(&sParse, &sCtx, pStep->pIdList, zOld);
          renameColumnElistNames(&sParse, &sCtx, pStep->pExprList, zOld);
        }
      }
    }

    /* Find tokens to edit in UPDATE OF clause */
    if( sParse.pTriggerTab==pTab ){
      renameColumnIdlistNames(&sParse, &sCtx,sParse.pNewTrigger->pColumns,zOld);
    }

    /* Find tokens to edit in various expressions and selects */
    renameWalkTrigger(&sWalker, sParse.pNewTrigger);
  }

  assert( rc==SQLITE_OK );
  rc = renameEditSql(context, &sCtx, zSql, zNew, bQuote);

renameColumnFunc_done:
  if( rc!=SQLITE_OK ){
    if( rc==SQLITE_ERROR && sqlite3WritableSchema(db) ){
      sqlite3_result_value(context, argv[0]);
    }else if( sParse.zErrMsg ){
      renameColumnParseError(context, "", argv[1], argv[2], &sParse);
    }else{
      sqlite3_result_error_code(context, rc);
    }
  }

  renameParseCleanup(&sParse);
  renameTokenFree(db, sCtx.pList);
#ifndef SQLITE_OMIT_AUTHORIZATION
  db->xAuth = xAuth;
#endif
  sqlite3BtreeLeaveAll(db);
}

/*
** Walker expression callback used by "RENAME TABLE".
*/
static int renameTableExprCb(Walker *pWalker, Expr *pExpr){
  RenameCtx *p = pWalker->u.pRename;
  if( pExpr->op==TK_COLUMN
   && ALWAYS(ExprUseYTab(pExpr))
   && p->pTab==pExpr->y.pTab
  ){
    renameTokenFind(pWalker->pParse, p, (void*)&pExpr->y.pTab);
  }
  return WRC_Continue;
}

/*
** Walker select callback used by "RENAME TABLE".
*/
static int renameTableSelectCb(Walker *pWalker, Select *pSelect){
  int i;
  RenameCtx *p = pWalker->u.pRename;
  SrcList *pSrc = pSelect->pSrc;
  if( pSelect->selFlags & (SF_View|SF_CopyCte) ){
    testcase( pSelect->selFlags & SF_View );
    testcase( pSelect->selFlags & SF_CopyCte );
    return WRC_Prune;
  }
  if( NEVER(pSrc==0) ){
    assert( pWalker->pParse->db->mallocFailed );
    return WRC_Abort;
  }
  for(i=0; i<pSrc->nSrc; i++){
    SrcItem *pItem = &pSrc->a[i];
    if( pItem->pSTab==p->pTab ){
      renameTokenFind(pWalker->pParse, p, pItem->zName);
    }
  }
  renameWalkWith(pWalker, pSelect);

  return WRC_Continue;
}


/*
** This C function implements an SQL user function that is used by SQL code
** generated by the ALTER TABLE ... RENAME command to modify the definition
** of any foreign key constraints that use the table being renamed as the
** parent table. It is passed three arguments:
**
**   0: The database containing the table being renamed.
**   1. type:     Type of object ("table", "view" etc.)
**   2. object:   Name of object
**   3: The complete text of the schema statement being modified,
**   4: The old name of the table being renamed, and
**   5: The new name of the table being renamed.
**   6: True if the schema statement comes from the temp db.
**
** It returns the new schema statement. For example:
**
** sqlite_rename_table('main', 'CREATE TABLE t1(a REFERENCES t2)','t2','t3',0)
**       -> 'CREATE TABLE t1(a REFERENCES t3)'
*/
static void renameTableFunc(
  sqlite3_context *context,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *zDb = (const char*)sqlite3_value_text(argv[0]);
  const char *zInput = (const char*)sqlite3_value_text(argv[3]);
  const char *zOld = (const char*)sqlite3_value_text(argv[4]);
  const char *zNew = (const char*)sqlite3_value_text(argv[5]);
  int bTemp = sqlite3_value_int(argv[6]);
  UNUSED_PARAMETER(NotUsed);

  if( zInput && zOld && zNew ){
    Parse sParse;
    int rc;
    int bQuote = 1;
    RenameCtx sCtx;
    Walker sWalker;

#ifndef SQLITE_OMIT_AUTHORIZATION
    sqlite3_xauth xAuth = db->xAuth;
    db->xAuth = 0;
#endif

    sqlite3BtreeEnterAll(db);

    memset(&sCtx, 0, sizeof(RenameCtx));
    sCtx.pTab = sqlite3FindTable(db, zOld, zDb);
    memset(&sWalker, 0, sizeof(Walker));
    sWalker.pParse = &sParse;
    sWalker.xExprCallback = renameTableExprCb;
    sWalker.xSelectCallback = renameTableSelectCb;
    sWalker.u.pRename = &sCtx;

    rc = renameParseSql(&sParse, zDb, db, zInput, bTemp);

    if( rc==SQLITE_OK ){
      int isLegacy = (db->flags & SQLITE_LegacyAlter);
      if( sParse.pNewTable ){
        Table *pTab = sParse.pNewTable;

        if( IsView(pTab) ){
          if( isLegacy==0 ){
            Select *pSelect = pTab->u.view.pSelect;
            NameContext sNC;
            memset(&sNC, 0, sizeof(sNC));
            sNC.pParse = &sParse;

            assert( pSelect->selFlags & SF_View );
            pSelect->selFlags &= ~(u32)SF_View;
            sqlite3SelectPrep(&sParse, pTab->u.view.pSelect, &sNC);
            if( sParse.nErr ){
              rc = sParse.rc;
            }else{
              sqlite3WalkSelect(&sWalker, pTab->u.view.pSelect);
            }
          }
        }else{
          /* Modify any FK definitions to point to the new table. */
#ifndef SQLITE_OMIT_FOREIGN_KEY
          if( (isLegacy==0 || (db->flags & SQLITE_ForeignKeys))
           && !IsVirtual(pTab)
          ){
            FKey *pFKey;
            assert( IsOrdinaryTable(pTab) );
            for(pFKey=pTab->u.tab.pFKey; pFKey; pFKey=pFKey->pNextFrom){
              if( sqlite3_stricmp(pFKey->zTo, zOld)==0 ){
                renameTokenFind(&sParse, &sCtx, (void*)pFKey->zTo);
              }
            }
          }
#endif

          /* If this is the table being altered, fix any table refs in CHECK
          ** expressions. Also update the name that appears right after the
          ** "CREATE [VIRTUAL] TABLE" bit. */
          if( sqlite3_stricmp(zOld, pTab->zName)==0 ){
            sCtx.pTab = pTab;
            if( isLegacy==0 ){
              sqlite3WalkExprList(&sWalker, pTab->pCheck);
            }
            renameTokenFind(&sParse, &sCtx, pTab->zName);
          }
        }
      }

      else if( sParse.pNewIndex ){
        renameTokenFind(&sParse, &sCtx, sParse.pNewIndex->zName);
        if( isLegacy==0 ){
          sqlite3WalkExpr(&sWalker, sParse.pNewIndex->pPartIdxWhere);
        }
      }

#ifndef SQLITE_OMIT_TRIGGER
      else{
        Trigger *pTrigger = sParse.pNewTrigger;
        TriggerStep *pStep;
        if( 0==sqlite3_stricmp(sParse.pNewTrigger->table, zOld)
            && sCtx.pTab->pSchema==pTrigger->pTabSchema
          ){
          renameTokenFind(&sParse, &sCtx, sParse.pNewTrigger->table);
        }

        if( isLegacy==0 ){
          rc = renameResolveTrigger(&sParse);
          if( rc==SQLITE_OK ){
            renameWalkTrigger(&sWalker, pTrigger);
            for(pStep=pTrigger->step_list; pStep; pStep=pStep->pNext){
              if( pStep->pSrc ){
                int i;
                for(i=0; i<pStep->pSrc->nSrc; i++){
                  SrcItem *pItem = &pStep->pSrc->a[i];
                  if( 0==sqlite3_stricmp(pItem->zName, zOld) ){
                    renameTokenFind(&sParse, &sCtx, pItem->zName);
                  }
                }
              }
            }
          }
        }
      }
#endif
    }

    if( rc==SQLITE_OK ){
      rc = renameEditSql(context, &sCtx, zInput, zNew, bQuote);
    }
    if( rc!=SQLITE_OK ){
      if( rc==SQLITE_ERROR && sqlite3WritableSchema(db) ){
        sqlite3_result_value(context, argv[3]);
      }else if( sParse.zErrMsg ){
        renameColumnParseError(context, "", argv[1], argv[2], &sParse);
      }else{
        sqlite3_result_error_code(context, rc);
      }
    }

    renameParseCleanup(&sParse);
    renameTokenFree(db, sCtx.pList);
    sqlite3BtreeLeaveAll(db);
#ifndef SQLITE_OMIT_AUTHORIZATION
    db->xAuth = xAuth;
#endif
  }

  return;
}

static int renameQuotefixExprCb(Walker *pWalker, Expr *pExpr){
  if( pExpr->op==TK_STRING && (pExpr->flags & EP_DblQuoted) ){
    renameTokenFind(pWalker->pParse, pWalker->u.pRename, (const void*)pExpr);
  }
  return WRC_Continue;
}

/* SQL function: sqlite_rename_quotefix(DB,SQL)
**
** Rewrite the DDL statement "SQL" so that any string literals that use
** double-quotes use single quotes instead.
**
** Two arguments must be passed:
**
**   0: Database name ("main", "temp" etc.).
**   1: SQL statement to edit.
**
** The returned value is the modified SQL statement. For example, given
** the database schema:
**
**   CREATE TABLE t1(a, b, c);
**
**   SELECT sqlite_rename_quotefix('main',
**       'CREATE VIEW v1 AS SELECT "a", "string" FROM t1'
**   );
**
** returns the string:
**
**   CREATE VIEW v1 AS SELECT "a", 'string' FROM t1
**
** If there is a error in the input SQL, then raise an error, except
** if PRAGMA writable_schema=ON, then just return the input string
** unmodified following an error.
*/
static void renameQuotefixFunc(
  sqlite3_context *context,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(context);
  char const *zDb = (const char*)sqlite3_value_text(argv[0]);
  char const *zInput = (const char*)sqlite3_value_text(argv[1]);

#ifndef SQLITE_OMIT_AUTHORIZATION
  sqlite3_xauth xAuth = db->xAuth;
  db->xAuth = 0;
#endif

  sqlite3BtreeEnterAll(db);

  UNUSED_PARAMETER(NotUsed);
  if( zDb && zInput ){
    int rc;
    Parse sParse;
    rc = renameParseSql(&sParse, zDb, db, zInput, 0);

    if( rc==SQLITE_OK ){
      RenameCtx sCtx;
      Walker sWalker;

      /* Walker to find tokens that need to be replaced. */
      memset(&sCtx, 0, sizeof(RenameCtx));
      memset(&sWalker, 0, sizeof(Walker));
      sWalker.pParse = &sParse;
      sWalker.xExprCallback = renameQuotefixExprCb;
      sWalker.xSelectCallback = renameColumnSelectCb;
      sWalker.u.pRename = &sCtx;

      if( sParse.pNewTable ){
        if( IsView(sParse.pNewTable) ){
          Select *pSelect = sParse.pNewTable->u.view.pSelect;
          pSelect->selFlags &= ~(u32)SF_View;
          sParse.rc = SQLITE_OK;
          sqlite3SelectPrep(&sParse, pSelect, 0);
          rc = (db->mallocFailed ? SQLITE_NOMEM : sParse.rc);
          if( rc==SQLITE_OK ){
            sqlite3WalkSelect(&sWalker, pSelect);
          }
        }else{
          int i;
          sqlite3WalkExprList(&sWalker, sParse.pNewTable->pCheck);
#ifndef SQLITE_OMIT_GENERATED_COLUMNS
          for(i=0; i<sParse.pNewTable->nCol; i++){
            sqlite3WalkExpr(&sWalker,
               sqlite3ColumnExpr(sParse.pNewTable,
                                         &sParse.pNewTable->aCol[i]));
          }
#endif /* SQLITE_OMIT_GENERATED_COLUMNS */
        }
      }else if( sParse.pNewIndex ){
        sqlite3WalkExprList(&sWalker, sParse.pNewIndex->aColExpr);
        sqlite3WalkExpr(&sWalker, sParse.pNewIndex->pPartIdxWhere);
      }else{
#ifndef SQLITE_OMIT_TRIGGER
        rc = renameResolveTrigger(&sParse);
        if( rc==SQLITE_OK ){
          renameWalkTrigger(&sWalker, sParse.pNewTrigger);
        }
#endif /* SQLITE_OMIT_TRIGGER */
      }

      if( rc==SQLITE_OK ){
        rc = renameEditSql(context, &sCtx, zInput, 0, 0);
      }
      renameTokenFree(db, sCtx.pList);
    }
    if( rc!=SQLITE_OK ){
      if( sqlite3WritableSchema(db) && rc==SQLITE_ERROR ){
        sqlite3_result_value(context, argv[1]);
      }else{
        sqlite3_result_error_code(context, rc);
      }
    }
    renameParseCleanup(&sParse);
  }

#ifndef SQLITE_OMIT_AUTHORIZATION
  db->xAuth = xAuth;
#endif

  sqlite3BtreeLeaveAll(db);
}

/* Function:  sqlite_rename_test(DB,SQL,TYPE,NAME,ISTEMP,WHEN,DQS)
**
** An SQL user function that checks that there are no parse or symbol
** resolution problems in a CREATE TRIGGER|TABLE|VIEW|INDEX statement.
** After an ALTER TABLE .. RENAME operation is performed and the schema
** reloaded, this function is called on each SQL statement in the schema
** to ensure that it is still usable.
**
**   0: Database name ("main", "temp" etc.).
**   1: SQL statement.
**   2: Object type ("view", "table", "trigger" or "index").
**   3: Object name.
**   4: True if object is from temp schema.
**   5: "when" part of error message.
**   6: True to disable the DQS quirk when parsing SQL.
**
** The return value is computed as follows:
**
**   A. If an error is seen and not in PRAGMA writable_schema=ON mode,
**      then raise the error.
**   B. Else if a trigger is created and the the table that the trigger is
**      attached to is in database zDb, then return 1.
**   C. Otherwise return NULL.
*/
static void renameTableTest(
  sqlite3_context *context,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(context);
  char const *zDb = (const char*)sqlite3_value_text(argv[0]);
  char const *zInput = (const char*)sqlite3_value_text(argv[1]);
  int bTemp = sqlite3_value_int(argv[4]);
  int isLegacy = (db->flags & SQLITE_LegacyAlter);
  char const *zWhen = (const char*)sqlite3_value_text(argv[5]);
  int bNoDQS = sqlite3_value_int(argv[6]);

#ifndef SQLITE_OMIT_AUTHORIZATION
  sqlite3_xauth xAuth = db->xAuth;
  db->xAuth = 0;
#endif

  UNUSED_PARAMETER(NotUsed);

  if( zDb && zInput ){
    int rc;
    Parse sParse;
    u64 flags = db->flags;
    if( bNoDQS ) db->flags &= ~(SQLITE_DqsDML|SQLITE_DqsDDL);
    rc = renameParseSql(&sParse, zDb, db, zInput, bTemp);
    db->flags = flags;
    if( rc==SQLITE_OK ){
      if( isLegacy==0 && sParse.pNewTable && IsView(sParse.pNewTable) ){
        NameContext sNC;
        memset(&sNC, 0, sizeof(sNC));
        sNC.pParse = &sParse;
        sqlite3SelectPrep(&sParse, sParse.pNewTable->u.view.pSelect, &sNC);
        if( sParse.nErr ) rc = sParse.rc;
      }

      else if( sParse.pNewTrigger ){
        if( isLegacy==0 ){
          rc = renameResolveTrigger(&sParse);
        }
        if( rc==SQLITE_OK ){
          int i1 = sqlite3SchemaToIndex(db, sParse.pNewTrigger->pTabSchema);
          int i2 = sqlite3FindDbName(db, zDb);
          if( i1==i2 ){
            /* Handle output case B */
            sqlite3_result_int(context, 1);
          }
        }
      }
    }

    if( rc!=SQLITE_OK && zWhen && !sqlite3WritableSchema(db) ){
      /* Output case A */
      renameColumnParseError(context, zWhen, argv[2], argv[3],&sParse);
    }
    renameParseCleanup(&sParse);
  }

#ifndef SQLITE_OMIT_AUTHORIZATION
  db->xAuth = xAuth;
#endif
}


/*
** Return the number of bytes until the end of the next non-whitespace and
** non-comment token.  For the purpose of this function, a "(" token includes
** all of the bytes through and including the matching ")", or until the
** first illegal token, whichever comes first.
**
** Write the token type into *piToken.
**
** The value returned is the number of bytes in the token itself plus
** the number of bytes of leading whitespace and comments skipped plus
** all bytes through the next matching ")" if the token is TK_LP.
**
** Example:    (Note: '.' used in place of '*' in the example z[] text)
**
**                                    ,--------- *piToken := TK_RP
**                                    v
**    z[] = " /.comment./ --comment\n (two three four) five"
**          |                                        |
**          |<-------------------------------------->|
**                              |
**                              `--- return value
*/
static int getConstraintToken(const u8 *z, int *piToken){
  int iOff = 0;
  int t = 0;
  do {
    iOff += sqlite3GetToken(&z[iOff], &t);
  }while( t==TK_SPACE || t==TK_COMMENT );

  *piToken = t;

  if( t==TK_LP ){
    int nNest = 1;
    while( nNest>0 ){
      iOff += sqlite3GetToken(&z[iOff], &t);
      if( t==TK_LP ){
        nNest++;
      }else if( t==TK_RP ){
        t = TK_LP;
        nNest--;
      }else if( t==TK_ILLEGAL ){
        break;
      }
    }
  }

  *piToken = t;
  return iOff;
}

/*
** The implementation of internal UDF sqlite_drop_column().
**
** Arguments:
**
**  argv[0]: An integer - the index of the schema containing the table
**  argv[1]: CREATE TABLE statement to modify.
**  argv[2]: An integer - the index of the column to remove.
**
** The value returned is a string containing the CREATE TABLE statement
** with column argv[2] removed.
*/
static void dropColumnFunc(
  sqlite3_context *context,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(context);
  int iSchema = sqlite3_value_int(argv[0]);
  const char *zSql = (const char*)sqlite3_value_text(argv[1]);
  int iCol = sqlite3_value_int(argv[2]);
  const char *zDb = db->aDb[iSchema].zDbSName;
  int rc;
  Parse sParse;
  RenameToken *pCol;
  Table *pTab;
  const char *zEnd;
  char *zNew = 0;

#ifndef SQLITE_OMIT_AUTHORIZATION
  sqlite3_xauth xAuth = db->xAuth;
  db->xAuth = 0;
#endif

  UNUSED_PARAMETER(NotUsed);
  rc = renameParseSql(&sParse, zDb, db, zSql, iSchema==1);
  if( rc!=SQLITE_OK ) goto drop_column_done;
  pTab = sParse.pNewTable;
  if( pTab==0 || pTab->nCol==1 || iCol>=pTab->nCol ){
    /* This can happen if the sqlite_schema table is corrupt */
    rc = SQLITE_CORRUPT_BKPT;
    goto drop_column_done;
  }

  if( iCol<pTab->nCol-1 ){
    RenameToken *pEnd;
    pCol = renameTokenFind(&sParse, 0, (void*)pTab->aCol[iCol].zCnName);
    pEnd = renameTokenFind(&sParse, 0, (void*)pTab->aCol[iCol+1].zCnName);
    zEnd = (const char*)pEnd->t.z;
  }else{
    int eTok;
    assert( IsOrdinaryTable(pTab) );
    assert( iCol!=0 );
    /* Point pCol->t.z at the "," immediately preceding the definition of
    ** the column being dropped. To do this, start at the name of the 
    ** previous column, and tokenize until the next ",".  */
    pCol = renameTokenFind(&sParse, 0, (void*)pTab->aCol[iCol-1].zCnName);
    do {
      pCol->t.z += getConstraintToken((const u8*)pCol->t.z, &eTok);
    }while( eTok!=TK_COMMA );
    pCol->t.z--;
    zEnd = (const char*)&zSql[pTab->u.tab.addColOffset];
  }

  zNew = sqlite3MPrintf(db, "%.*s%s", pCol->t.z-zSql, zSql, zEnd);
  sqlite3_result_text(context, zNew, -1, SQLITE_TRANSIENT);
  sqlite3_free(zNew);

drop_column_done:
  renameParseCleanup(&sParse);
#ifndef SQLITE_OMIT_AUTHORIZATION
  db->xAuth = xAuth;
#endif
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(context, rc);
  }
}

/*
** This function is called by the parser upon parsing an
**
**     ALTER TABLE pSrc DROP COLUMN pName
**
** statement. Argument pSrc contains the possibly qualified name of the
** table being edited, and token pName the name of the column to drop.
*/
void sqlite3AlterDropColumn(Parse *pParse, SrcList *pSrc, const Token *pName){
  sqlite3 *db = pParse->db;       /* Database handle */
  Table *pTab;                    /* Table to modify */
  int iDb;                        /* Index of db containing pTab in aDb[] */
  const char *zDb;                /* Database containing pTab ("main" etc.) */
  char *zCol = 0;                 /* Name of column to drop */
  int iCol;                       /* Index of column zCol in pTab->aCol[] */

  /* Look up the table being altered. */
  assert( pParse->pNewTable==0 );
  assert( sqlite3BtreeHoldsAllMutexes(db) );
  if( NEVER(db->mallocFailed) ) goto exit_drop_column;
  pTab = sqlite3LocateTableItem(pParse, 0, &pSrc->a[0]);
  if( !pTab ) goto exit_drop_column;

  /* Make sure this is not an attempt to ALTER a view, virtual table or
  ** system table. */
  if( SQLITE_OK!=isAlterableTable(pParse, pTab) ) goto exit_drop_column;
  if( SQLITE_OK!=isRealTable(pParse, pTab, 1) ) goto exit_drop_column;

  /* Find the index of the column being dropped. */
  zCol = sqlite3NameFromToken(db, pName);
  if( zCol==0 ){
    assert( db->mallocFailed );
    goto exit_drop_column;
  }
  iCol = sqlite3ColumnIndex(pTab, zCol);
  if( iCol<0 ){
    sqlite3ErrorMsg(pParse, "no such column: \"%T\"", pName);
    goto exit_drop_column;
  }
  if( isRowidAlias(pParse, pTab, zCol, "drop") ) goto exit_drop_column;

  /* Do not allow the user to drop a PRIMARY KEY column or a column
  ** constrained by a UNIQUE constraint.  */
  if( pTab->aCol[iCol].colFlags & (COLFLAG_PRIMKEY|COLFLAG_UNIQUE) ){
    sqlite3ErrorMsg(pParse, "cannot drop %s column: \"%s\"",
        (pTab->aCol[iCol].colFlags&COLFLAG_PRIMKEY) ? "PRIMARY KEY" : "UNIQUE",
        zCol
    );
    goto exit_drop_column;
  }

  /* Do not allow the number of columns to go to zero */
  if( pTab->nCol<=1 ){
    sqlite3ErrorMsg(pParse, "cannot drop column \"%s\": no other columns exist",zCol);
    goto exit_drop_column;
  }

  /* Edit the sqlite_schema table */
  iDb = sqlite3SchemaToIndex(db, pTab->pSchema);
  assert( iDb>=0 );
  zDb = db->aDb[iDb].zDbSName;
#ifndef SQLITE_OMIT_AUTHORIZATION
  /* Invoke the authorization callback. */
  if( sqlite3AuthCheck(pParse, SQLITE_ALTER_TABLE, zDb, pTab->zName, zCol) ){
    goto exit_drop_column;
  }
#endif
  renameTestSchema(pParse, zDb, iDb==1, "", 0);
  renameFixQuotes(pParse, zDb, iDb==1);
  sqlite3NestedParse(pParse,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = sqlite_drop_column(%d, sql, %d) "
      "WHERE (type=='table' AND tbl_name=%Q COLLATE nocase)"
      , zDb, iDb, iCol, pTab->zName
  );

  /* Drop and reload the database schema. */
  renameReloadSchema(pParse, iDb, INITFLAG_AlterDrop);
  renameTestSchema(pParse, zDb, iDb==1, "after drop column", 1);

  /* Edit rows of table on disk */
  if( pParse->nErr==0 && (pTab->aCol[iCol].colFlags & COLFLAG_VIRTUAL)==0 ){
    int i;
    int addr;
    int reg;
    int regRec;
    Index *pPk = 0;
    int nField = 0;               /* Number of non-virtual columns after drop */
    int iCur;
    Vdbe *v = sqlite3GetVdbe(pParse);
    iCur = pParse->nTab++;
    sqlite3OpenTable(pParse, iCur, iDb, pTab, OP_OpenWrite);
    addr = sqlite3VdbeAddOp1(v, OP_Rewind, iCur); VdbeCoverage(v);
    reg = ++pParse->nMem;
    if( HasRowid(pTab) ){
      sqlite3VdbeAddOp2(v, OP_Rowid, iCur, reg);
      pParse->nMem += pTab->nCol;
    }else{
      pPk = sqlite3PrimaryKeyIndex(pTab);
      pParse->nMem += pPk->nColumn;
      for(i=0; i<pPk->nKeyCol; i++){
        sqlite3VdbeAddOp3(v, OP_Column, iCur, i, reg+i+1);
      }
      nField = pPk->nKeyCol;
    }
    regRec = ++pParse->nMem;
    for(i=0; i<pTab->nCol; i++){
      if( i!=iCol && (pTab->aCol[i].colFlags & COLFLAG_VIRTUAL)==0 ){
        int regOut;
        if( pPk ){
          int iPos = sqlite3TableColumnToIndex(pPk, i);
          int iColPos = sqlite3TableColumnToIndex(pPk, iCol);
          if( iPos<pPk->nKeyCol ) continue;
          regOut = reg+1+iPos-(iPos>iColPos);
        }else{
          regOut = reg+1+nField;
        }
        if( i==pTab->iPKey ){
          sqlite3VdbeAddOp2(v, OP_Null, 0, regOut);
        }else{
          char aff = pTab->aCol[i].affinity;
          if( aff==SQLITE_AFF_REAL ){
            pTab->aCol[i].affinity = SQLITE_AFF_NUMERIC;
          }
          sqlite3ExprCodeGetColumnOfTable(v, pTab, iCur, i, regOut);
          pTab->aCol[i].affinity = aff;
        }
        nField++;
      }
    }
    if( nField==0 ){
      /* dbsqlfuzz 5f09e7bcc78b4954d06bf9f2400d7715f48d1fef */
      pParse->nMem++;
      sqlite3VdbeAddOp2(v, OP_Null, 0, reg+1);
      nField = 1;
    }
    sqlite3VdbeAddOp3(v, OP_MakeRecord, reg+1, nField, regRec);
    if( pPk ){
      sqlite3VdbeAddOp4Int(v, OP_IdxInsert, iCur, regRec, reg+1, pPk->nKeyCol);
    }else{
      sqlite3VdbeAddOp3(v, OP_Insert, iCur, regRec, reg);
    }
    sqlite3VdbeChangeP5(v, OPFLAG_SAVEPOSITION);

    sqlite3VdbeAddOp2(v, OP_Next, iCur, addr+1); VdbeCoverage(v);
    sqlite3VdbeJumpHere(v, addr);
  }

exit_drop_column:
  sqlite3DbFree(db, zCol);
  sqlite3SrcListDelete(db, pSrc);
}

/*
** Return the number of bytes of leading whitespace/comments in string z[].
*/
static int getWhitespace(const u8 *z){
  int nRet = 0;
  while( 1 ){
    int t = 0;
    int n = sqlite3GetToken(&z[nRet], &t);
    if( t!=TK_SPACE && t!=TK_COMMENT ) break;
    nRet += n;
  }
  return nRet;
}


/* Length of zStart..zEnd with trailing whitespace and comments removed. */
static int notNullRtrim(const char *zStart, const char *zEnd){
  int nMax = (int)(zEnd - zStart);
  int iOff = 0;
  int nRet = 0;

  while( iOff<nMax ){
    int t = 0;
    int n = sqlite3GetToken((const u8*)&zStart[iOff], &t);
    if( n<=0 || t==TK_ILLEGAL ) break;
    if( t!=TK_SPACE && t!=TK_COMMENT ) nRet = iOff + n;
    iOff += n;
  }
  return nRet;
}

/* Record a constraint's extent, taking in its CONSTRAINT name and lookahead. */
void sqlite3ConsLocAdd(
  Parse *pParse,
  u8 eType,
  int iCol,
  const char *zStart,
  const char *zEnd
){
  const char *zKw;
  const char *zLimit;

  assert( pParse->isCreate );
  assert( zStart!=0 && zEnd!=0 && zEnd>zStart );

  zKw = pParse->u1.cr.zConsKw;
  if( zKw!=0 ){
    const char *zGap = pParse->u1.cr.zConsEnd;
    assert( zGap!=0 );
    if( zGap<=zStart
     && &zGap[getWhitespace((const u8*)zGap)]==zStart
    ){
      zStart = zKw;
    }
  }

  zLimit = pParse->sLastToken.z;
  if( zLimit==0 || zLimit<zEnd ) zLimit = zEnd;
  zEnd = &zStart[notNullRtrim(zStart, zLimit)];

  sqlite3ParseLocAdd(pParse, eType, iCol, zStart, zEnd);
}

/* Extend the current column's extent over the constraint just parsed. */
void sqlite3ColDefLocExtend(Parse *pParse){
  Table *p = pParse->pNewTable;
  ParseLoc *pLoc;
  const char *zLimit;

  assert( IN_RENAME_OBJECT );
  if( p==0 || p->nCol<=0 ) return;
  for(pLoc=pParse->pLoc; pLoc; pLoc=pLoc->pNext){
    if( pLoc->eType==PARSELOC_ColDef && pLoc->iCol==p->nCol-1 ) break;
  }
  if( pLoc==0 ) return;
  zLimit = pParse->sLastToken.z;
  if( zLimit==0 || zLimit<=pLoc->t.z ) return;
  pLoc->t.n = (unsigned)notNullRtrim(pLoc->t.z, zLimit);
}

/* Record where a column-constraint keyword sits, for the ALTER that drops it. */
void sqlite3ColConsLocAdd(Parse *pParse, u8 eType, Token *pKw, int bCol){
  Table *p = pParse->pNewTable;
  int iCol = -1;
  assert( IN_RENAME_OBJECT );
  if( p==0 ) return;
  if( bCol ){
    if( p->nCol<=0 ) return;
    iCol = p->nCol-1;
  }
  sqlite3ConsLocAdd(pParse, eType, iCol, pKw->z, &pKw->z[pKw->n]);
}

/* Extend the newest FOREIGN KEY extent over a trailing DEFERRABLE clause. */
void sqlite3FkLocExtend(Parse *pParse, const char *zEnd){
  ParseLoc *p;
  const char *z;
  int t = 0;

  assert( IN_RENAME_OBJECT );
  if( zEnd==0 ) return;
  for(p=pParse->pLoc; p; p=p->pNext){
    if( p->eType==PARSELOC_ForeignKey ) break;
  }
  if( p==0 ) return;

  z = &p->t.z[p->t.n];
  if( z>zEnd ) return;
  z += getWhitespace((const u8*)z);
  sqlite3GetToken((const u8*)z, &t);
  if( t!=TK_DEFERRABLE && t!=TK_NOT ) return;
  p->t.n = (unsigned)notNullRtrim(p->t.z, zEnd);
}

/* Record one position within the text being parsed. */
void sqlite3ParseLocAdd(
  Parse *pParse,
  u8 eType,
  int iCol,
  const char *zStart,
  const char *zEnd
){
  ParseLoc *pNew;

  assert( IN_RENAME_OBJECT );
  assert( zStart!=0 && zEnd!=0 && zEnd>=zStart );

  pNew = sqlite3DbMallocZero(pParse->db, sizeof(ParseLoc));
  if( pNew==0 ) return;
  pNew->eType = eType;
  pNew->iCol = iCol;
  pNew->t.z = zStart;
  pNew->t.n = (unsigned)(zEnd - zStart);
  pNew->pNext = pParse->pLoc;
  pParse->pLoc = pNew;
}

/* Free a list of ParseLoc objects. */
void sqlite3ParseLocFree(sqlite3 *db, ParseLoc *pLoc){
  while( pLoc ){
    ParseLoc *pNext = pLoc->pNext;
    sqlite3DbFree(db, pLoc);
    pLoc = pNext;
  }
}

/*
** Argument z points into the body of a constraint - specifically the 
** second token of the constraint definition.  For a named constraint,
** z points to the second token of the constraint definition. For an 
** unnamed NOT NULL constraint, z points to the first byte past the NOT 
** keyword.
**
** Argument eTok may be the token value of the first token of the constraint
** (e.g. TK_CHECK or TK_REFERENCES) or zero. If it is either TK_REFERENCES
** or TK_FOREIGN, special parsing is enabled to find the end of the foreign-key
** constraint definition.
**
** Return the number of bytes until the end of the constraint. 
*/
static int getConstraint(const u8 *z, int eTok){
  int iOff = 0;
  int t = 0;

#ifndef SQLITE_OMIT_FOREIGN_KEY
  if( eTok==TK_FOREIGN ){
    /* For a FOREIGN KEY constraint, use getConstraint() to parse everything
    ** up to the REFERENCES keyword. Then getConstraintToken() to consume
    ** the TK_REFERENCES token itself. Then fall through to the special
    ** handling for TK_REFERENCES below.  */
    iOff = getConstraint(z, 0);
    iOff += getConstraintToken(&z[iOff], &eTok);
  }

  if( eTok==TK_REFERENCES ){
    /* REFERENCES is followed by a table name. Gobble this up here in
    ** case the table name is a fallback token like TK_GENERATED. */
    iOff += getConstraintToken(&z[iOff], &t);
  }
#endif

  /* Now, the current constraint proceeds until the next occurence of one 
  ** of the following tokens: 
  **
  **   CONSTRAINT, PRIMARY, NOT, UNIQUE, CHECK, DEFAULT, 
  **   COLLATE, REFERENCES, FOREIGN, GENERATED, AS, RP, or COMMA
  **
  ** Also exit the loop if ILLEGAL turns up.
  */
  while( 1 ){
    int n = getConstraintToken(&z[iOff], &t);
    if( t==TK_CONSTRAINT || t==TK_PRIMARY || t==TK_NOT || t==TK_UNIQUE
     || t==TK_CHECK || t==TK_DEFAULT || t==TK_COLLATE || t==TK_REFERENCES
     || t==TK_FOREIGN || t==TK_RP || t==TK_COMMA || t==TK_ILLEGAL
     || t==TK_AS || t==TK_GENERATED
    ){
      break;
    }
    iOff += n;
  }
  
  return iOff;
}

/*
** Compare two constraint names.
**
** Summary:   *pRes := zQuote != zCmp
**
** Details:
** Compare the (possibly quoted) constraint name zQuote[0..nQuote-1]
** against zCmp[].  Write zero into *pRes if they are the same and
** non-zero if they differ.  Normally return SQLITE_OK, except if there
** is an OOM, set the OOM error condition on ctx and return SQLITE_NOMEM.
*/
static int quotedCompare(
  sqlite3_context *ctx,  /* Function context on which to report errors */
  int t,                 /* Token type */
  const u8 *zQuote,      /* Possibly quoted text.  Not zero-terminated. */
  int nQuote,            /* Length of zQuote in bytes */
  const u8 *zCmp,        /* Zero-terminated, unquoted name to compare against */
  int *pRes              /* OUT: Set to 0 if equal, non-zero if unequal */
){
  char *zCopy = 0;       /* De-quoted, zero-terminated copy of zQuote[] */

  if( t==TK_ILLEGAL ){
    *pRes = 1;
    return SQLITE_OK;
  }
  zCopy = sqlite3MallocZero(nQuote+1);
  if( zCopy==0 ){
    sqlite3_result_error_nomem(ctx);
    return SQLITE_NOMEM_BKPT;
  }
  memcpy(zCopy, zQuote, nQuote);
  sqlite3Dequote(zCopy);
  *pRes = sqlite3_stricmp((const char*)zCopy, (const char*)zCmp);
  sqlite3_free(zCopy);
  return SQLITE_OK;
}

/*
** zSql[] is a CREATE TABLE statement, supposedly.  Find the offset
** into zSql[] of the first character past the first "(" and write
** that offset into *piOff and return SQLITE_OK.  Or, if not found,
** set the SQLITE_CORRUPT error code and return SQLITE_ERROR.
*/
static int skipCreateTable(sqlite3_context *ctx, const u8 *zSql, int *piOff){
  int iOff = 0;

  if( zSql==0 ) return SQLITE_ERROR;

  /* Jump past the "CREATE TABLE" bit. */
  while( 1 ){
    int t = 0;
    iOff += sqlite3GetToken(&zSql[iOff], &t);
    if( t==TK_LP ) break;
    if( t==TK_ILLEGAL ){
      sqlite3_result_error_code(ctx, SQLITE_CORRUPT_BKPT);
      return SQLITE_ERROR;
    }
  }

  *piOff = iOff;
  return SQLITE_OK;
}

/*
** Internal SQL function sqlite3_drop_constraint():  Given an input
** CREATE TABLE statement, return a revised CREATE TABLE statement
** with a constraint removed.  Two forms, depending on the datatype
** of argv[2]:
**
**   sqlite_drop_constraint(SQL, INT)  -- Omit NOT NULL from the INT-th column
**   sqlite_drop_constraint(SQL, TEXT) -- OMIT constraint with name TEXT
**
** In the first case, the left-most column is 0.
*/
static void dropConstraintFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  const u8 *zSql = sqlite3_value_text(argv[0]);
  const u8 *zCons = 0;
  int iNotNull = -1;
  int ii;
  int iOff = 0;
  int iStart = 0;
  int iEnd = 0;
  char *zNew = 0;
  int t = 0;
  sqlite3 *db;
  UNUSED_PARAMETER(NotUsed);

  if( zSql==0 ) return;

  /* Jump past the "CREATE TABLE" bit. */
  if( skipCreateTable(ctx, zSql, &iOff) ) return;

  if( sqlite3_value_type(argv[1])==SQLITE_INTEGER ){
    iNotNull = sqlite3_value_int(argv[1]);
  }else{
    zCons = sqlite3_value_text(argv[1]);
  }

  /* Search for the named constraint within column definitions. */
  for(ii=0; iEnd==0; ii++){
  
    /* Now parse the column or table constraint definition. Search
    ** for the token CONSTRAINT if this is a DROP CONSTRAINT command, or
    ** NOT in the right column if this is a DROP NOT NULL. */
    while( 1 ){
      iStart = iOff;
      iOff += getConstraintToken(&zSql[iOff], &t);
      if( t==TK_CONSTRAINT && (zCons || iNotNull==ii) ){
        /* Check if this is the constraint we are searching for. */
        int nTok = 0;
        int cmp = 1;

        /* Skip past any whitespace. */
        iOff += getWhitespace(&zSql[iOff]);

        /* Compare the next token - which may be quoted - with the name of
        ** the constraint being dropped.  */
        nTok = getConstraintToken(&zSql[iOff], &t);
        if( zCons ){
          if( quotedCompare(ctx, t, &zSql[iOff], nTok, zCons, &cmp) ) return;
        }
        iOff += nTok;

        /* The next token is usually the first token of the constraint
        ** definition. This is enough to tell the type of the constraint - 
        ** TK_NOT means it is a NOT NULL, TK_CHECK a CHECK constraint etc.
        **
        ** There is also the chance that the next token is TK_CONSTRAINT
        ** (or TK_DEFAULT or TK_COLLATE), for example if a table has been
        ** created as follows:
        **
        **    CREATE TABLE t1(cols, CONSTRAINT one CONSTRAINT two NOT NULL);
        **
        ** In this case, allow the "CONSTRAINT one" bit to be dropped by
        ** this command if that is what is requested, or to advance to
        ** the next iteration of the loop with &zSql[iOff] still pointing
        ** to the CONSTRAINT keyword.  */
        nTok = getConstraintToken(&zSql[iOff], &t);
        if( t==TK_CONSTRAINT || t==TK_DEFAULT || t==TK_COLLATE 
         || t==TK_COMMA || t==TK_RP || t==TK_GENERATED || t==TK_AS 
        ){
          t = TK_CHECK;
        }else{
          iOff += nTok;
          iOff += getConstraint(&zSql[iOff], t);
        }

        if( cmp==0 || (iNotNull>=0 && t==TK_NOT) ){
          if( t!=TK_NOT && t!=TK_CHECK && t!=TK_REFERENCES && t!=TK_FOREIGN ){
            errorMPrintf(ctx, "constraint may not be dropped: %s", zCons);
            return;
          }
          iEnd = iOff;
          break;
        }

      }else if( t==TK_NOT && iNotNull==ii ){
        iEnd = iOff + getConstraint(&zSql[iOff], 0);
        break;
      }else if( t==TK_RP || t==TK_ILLEGAL ){
        iEnd = -1;
        break;
      }else if( t==TK_COMMA ){
        break;
      }
    }
  }

  /* If the constraint has not been found it is an error. */
  if( iEnd<=0 ){
    if( zCons ){
      errorMPrintf(ctx, "no such constraint: %s", zCons);
    }else{
      /* SQLite follows postgres in that a DROP NOT NULL on a column that is
      ** not NOT NULL is not an error. So just return the original SQL here. */
      sqlite3_result_text(ctx, (const char*)zSql, -1, SQLITE_TRANSIENT);
    }
  }else{

    /* Figure out if an extra space should be inserted after the constraint
    ** is removed. And if an additional comma preceding the constraint 
    ** should be removed. */
    const char *zSpace = " ";
    iEnd += getWhitespace(&zSql[iEnd]);
    sqlite3GetToken(&zSql[iEnd], &t);
    if( t==TK_RP || t==TK_COMMA ){
      zSpace = "";
      if( zSql[iStart-1]==',' ) iStart--;
    }

    db = sqlite3_context_db_handle(ctx);
    zNew = sqlite3MPrintf(db, "%.*s%s%s", iStart, zSql, zSpace, &zSql[iEnd]);
    sqlite3_result_text(ctx, zNew, -1, SQLITE_DYNAMIC);
  }
}


/* True if column iCol is part of pTab's PRIMARY KEY. */
static int alterColInPk(Table *pTab, int iCol){
  Index *pPk;
  int i;
  if( pTab->iPKey==iCol ) return 1;
  pPk = sqlite3PrimaryKeyIndex(pTab);
  if( pPk ){
    for(i=0; i<pPk->nKeyCol; i++){
      if( pPk->aiColumn[i]==iCol ) return 1;
    }
  }
  return 0;
}

/* Index of the column named zCol in pTab, or -1. */
static int alterColumnIndex(Table *pTab, const char *zCol){
  int i;
  if( pTab==0 || zCol==0 ) return -1;
  for(i=0; i<pTab->nCol; i++){
    if( sqlite3_stricmp(pTab->aCol[i].zCnName, zCol)==0 ) return i;
  }
  return -1;
}

/* Cut the clause at pLoc out of zOut and return the new length. */
static int alterExciseClause(
  char *zOut,
  int nOut,
  const char *zSql,
  const Token *pLoc
){
  int iStart = (int)(pLoc->z - zSql);
  int iEnd = iStart + (int)pLoc->n;
  int t = 0;

  assert( iStart>=0 && iEnd<=nOut );
  iEnd += getWhitespace((const u8*)&zOut[iEnd]);
  sqlite3GetToken((const u8*)&zOut[iEnd], &t);
  while( iStart>0 && sqlite3Isspace(zOut[iStart-1]) ) iStart--;
  if( t==TK_RP || t==TK_COMMA ){
    if( iStart>0 && zOut[iStart-1]==',' ){
      iStart--;
      while( iStart>0 && sqlite3Isspace(zOut[iStart-1]) ) iStart--;
    }
  }else{
    zOut[iStart] = ' ';
    iStart++;
  }
  assert( iStart<=iEnd );

  memmove(&zOut[iStart], &zOut[iEnd], (size_t)(nOut-iEnd)+1);
  return nOut - (iEnd - iStart);
}

typedef struct AlterEdit AlterEdit;
struct AlterEdit {
  sqlite3 *db;
  Parse sParse;
  Table *pTab;
  const char *zSql;
  char *zOut;
  int nOut;
  int rc;
  int bParsed;
#ifndef SQLITE_OMIT_AUTHORIZATION
  sqlite3_xauth xAuth;
#endif
};

/* Open an edit of zSql: reparse it with the authorizer off. */
static int alterEditBegin(
  AlterEdit *p,
  sqlite3 *db,
  int iSchema,
  const char *zSql
){
  memset(p, 0, sizeof(*p));
  p->db = db;
  p->zSql = zSql;
  p->rc = SQLITE_OK;
#ifndef SQLITE_OMIT_AUTHORIZATION
  p->xAuth = db->xAuth;
  db->xAuth = 0;
#endif

  if( zSql==0 || iSchema<0 || iSchema>=db->nDb ) return 0;

  if( renameParseSql(&p->sParse, db->aDb[iSchema].zDbSName, db, zSql,
                     iSchema==1) )
  {
    p->bParsed = 1;
    p->rc = SQLITE_CORRUPT_BKPT;
    return 0;
  }
  p->bParsed = 1;
  p->pTab = p->sParse.pNewTable;
  if( p->pTab==0 || !IsOrdinaryTable(p->pTab) ){
    p->rc = SQLITE_CORRUPT_BKPT;
    return 0;
  }
  return 1;
}

/* Give the edit a private copy of the statement to cut and splice. */
static int alterEditCopy(AlterEdit *p){
  assert( p->zOut==0 );
  p->nOut = sqlite3Strlen30(p->zSql);
  p->zOut = sqlite3DbMallocRaw(p->db, (i64)p->nOut+1);
  if( p->zOut==0 ){
    p->rc = SQLITE_NOMEM_BKPT;
    return 0;
  }
  memcpy(p->zOut, p->zSql, (size_t)p->nOut+1);
  return 1;
}

/* Close an edit, giving back what it borrowed and reporting any error. */
static void alterEditFinish(AlterEdit *p, sqlite3_context *ctx){
  if( p->bParsed ) renameParseCleanup(&p->sParse);
  sqlite3DbFree(p->db, p->zOut);
#ifndef SQLITE_OMIT_AUTHORIZATION
  p->db->xAuth = p->xAuth;
#endif
  if( p->rc!=SQLITE_OK ) sqlite3_result_error_code(ctx, p->rc);
}

/* sqlite_drop_colcons(ISCHEMA,SQL,ICOL,ETYPE): drop constraints of one kind. */
static void dropColConsFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  int iSchema = sqlite3_value_int(argv[0]);
  const char *zSql = (const char*)sqlite3_value_text(argv[1]);
  int iCol = sqlite3_value_int(argv[2]);
  int eType = sqlite3_value_int(argv[3]);
  AlterEdit x;

  UNUSED_PARAMETER(NotUsed);
  if( iCol<0 && eType!=PARSELOC_Check ) return;
  if( !alterEditBegin(&x, db, iSchema, zSql) ) goto drop_col_cons_done;
  if( iCol>=x.pTab->nCol ){
    x.rc = SQLITE_CORRUPT_BKPT;
    goto drop_col_cons_done;
  }

  if( !alterEditCopy(&x) ) goto drop_col_cons_done;

  while( 1 ){
    ParseLoc *p;
    ParseLoc *pBest = 0;

    for(p=x.sParse.pLoc; p; p=p->pNext){
      if( p->eType!=eType || p->iCol!=iCol ) continue;
      if( pBest==0 || p->t.z>pBest->t.z ) pBest = p;
    }
    if( pBest==0 ) break;
    pBest->eType = 0;
    x.nOut = alterExciseClause(x.zOut, x.nOut, x.zSql, &pBest->t);
  }

  sqlite3_result_text(ctx, x.zOut, x.nOut, SQLITE_TRANSIENT);

drop_col_cons_done:
  alterEditFinish(&x, ctx);
}

/* sqlite_insert_constraint(ISCHEMA,SQL,TEXT,ICOL): splice a constraint in. */
static void insertConstraintFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  int iSchema = sqlite3_value_int(argv[0]);
  const char *zSql = (const char*)sqlite3_value_text(argv[1]);
  const char *zCons = (const char*)sqlite3_value_text(argv[2]);
  int iCol = sqlite3_value_int(argv[3]);
  AlterEdit x;
  int iOff;

  UNUSED_PARAMETER(NotUsed);
  if( zCons==0 ) return;
  if( !alterEditBegin(&x, db, iSchema, zSql) ) goto insert_cons_done;

  if( iCol>=x.pTab->nCol ){
    x.rc = SQLITE_CORRUPT_BKPT;
    goto insert_cons_done;
  }

  if( iCol<0 ){
    if( x.sParse.sColListEnd.z==0 ) goto insert_cons_corrupt;
    iOff = (int)(x.sParse.sColListEnd.z - zSql);
  }else{
    ParseLoc *p;
    for(p=x.sParse.pLoc; p; p=p->pNext){
      if( p->eType==PARSELOC_ColDef && p->iCol==iCol ) break;
    }
    if( p==0 ) goto insert_cons_corrupt;
    iOff = (int)(p->t.z - zSql) + (int)p->t.n;
  }
  x.zOut = sqlite3MPrintf(db, "%.*s%s%s%s", iOff, zSql,
                          iCol<0 ? ", " : " ", zCons, &zSql[iOff]);
  if( x.zOut==0 ){
    x.rc = SQLITE_NOMEM_BKPT;
    goto insert_cons_done;
  }
  sqlite3_result_text(ctx, x.zOut, -1, SQLITE_TRANSIENT);
  goto insert_cons_done;

insert_cons_corrupt:
  x.rc = SQLITE_CORRUPT_BKPT;

insert_cons_done:
  alterEditFinish(&x, ctx);
}

/*
** Find a column named pCol in table pTab. If successful, set output 
** parameter *piCol to the index of the column in the table and return
** SQLITE_OK. Otherwise, set *piCol to -1 and return an SQLite error
** code.
*/
static int alterFindCol(Parse *pParse, Table *pTab, Token *pCol, int *piCol){
  sqlite3 *db = pParse->db;
  char *zName = sqlite3NameFromToken(db, pCol);
  int rc = SQLITE_NOMEM;
  int iCol = -1;

  if( zName ){
    iCol = sqlite3ColumnIndex(pTab, zName);
    if( iCol<0 ){
      sqlite3ErrorMsg(pParse, "no such column: %s", zName);
      rc = SQLITE_ERROR;
    }else{
      rc = SQLITE_OK;
    }
  }

#ifndef SQLITE_OMIT_AUTHORIZATION
  if( rc==SQLITE_OK ){
    const char *zDb = db->aDb[sqlite3SchemaToIndex(db, pTab->pSchema)].zDbSName;
    const char *zCol = pTab->aCol[iCol].zCnName;
    if( sqlite3AuthCheck(pParse, SQLITE_ALTER_TABLE, zDb, pTab->zName, zCol) ){
      pTab = 0;
    }
  }
#endif

  sqlite3DbFree(db, zName);
  *piCol = iCol;
  return rc;
}


/*
** Find the table named by the first entry in source list pSrc. If successful,
** return a pointer to the Table structure and set output variable (*pzDb)
** to point to the name of the database containin the table (i.e. "main",
** "temp" or the name of an attached database). 
**
** If the table cannot be located, return NULL. The value of the two output
** parameters is undefined in this case.
*/
static Table *alterFindTable(
  Parse *pParse,        /* Parsing context */
  SrcList *pSrc,        /* Name of the table to look for */
  int *piDb,            /* OUT: write the iDb here */
  const char **pzDb,    /* OUT: write name of schema here */
  int bAuth,            /* Do ALTER TABLE authorization checks if true */
  int iOp               /* isRealTable() operation code for error messages */
){
  sqlite3 *db = pParse->db;
  Table *pTab = 0;
  assert( sqlite3BtreeHoldsAllMutexes(db) );
  if( alterRefreshSchema(pParse) ){
    sqlite3SrcListDelete(db, pSrc);
    return 0;
  }
  pTab = sqlite3LocateTableItem(pParse, 0, &pSrc->a[0]);
  if( pTab ){
    int iDb = sqlite3SchemaToIndex(db, pTab->pSchema);
    *pzDb = db->aDb[iDb].zDbSName;
    *piDb = iDb;

    if( SQLITE_OK!=isRealTable(pParse, pTab, iOp) 
     || SQLITE_OK!=isAlterableTable(pParse, pTab) 
    ){
      pTab = 0;
    }
  }
#ifndef SQLITE_OMIT_AUTHORIZATION
  if( pTab && bAuth ){
    if( sqlite3AuthCheck(pParse, SQLITE_ALTER_TABLE, *pzDb, pTab->zName, 0) ){
      pTab = 0;
    }
  }
#endif
  sqlite3SrcListDelete(db, pSrc);
  return pTab;
}

/* Emit the UPDATE that puts an edited CREATE TABLE back in the schema. */
static void alterUpdateSchemaSql(
  Parse *pParse,
  Table *pTab,
  const char *zDb,
  const char *zFmt,
  ...
){
  sqlite3 *db = pParse->db;
  char *zEdit;
  va_list ap;

  va_start(ap, zFmt);
  zEdit = sqlite3VMPrintf(db, zFmt, ap);
  va_end(ap);
  if( zEdit==0 ) return;

  sqlite3NestedParse(pParse,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = %s "
      "WHERE type='table' AND tbl_name=%Q COLLATE nocase"
      , zDb, zEdit, pTab->zName
  );
  sqlite3DbFree(db, zEdit);
}

/*
** Generate bytecode for one of:
**
**  (1)   ALTER TABLE pSrc DROP CONSTRAINT pCons
**  (2)   ALTER TABLE pSrc ALTER pCol DROP <kind>
**
** One of pCons and pCol must be NULL and the other non-null.
*/
void sqlite3AlterDropConstraint(
  Parse *pParse,     /* Parsing context */
  SrcList *pSrc,     /* The table being altered */
  Token *pCons,      /* Name of the constraint to drop, or 0 */
  Token *pCol,       /* Name of the column to take constraints off, or 0 */
  int eType          /* Kind to drop for form (2) */
){
  sqlite3 *db = pParse->db;
  Table *pTab = 0;
  int iDb = 0;
  const char *zDb = 0;
  char *zArg = 0;

  assert( (pCol==0)!=(pCons==0) );
  assert( (pCol==0)==(eType==0) );
  assert( pSrc->nSrc==1 );
  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, pCons!=0, 2);
  if( !pTab ) return;

  if( pCons ){
    char *z = sqlite3NameFromToken(db, pCons);
    zArg = sqlite3MPrintf(db, "sqlite_drop_constraint(sql, %Q)", z);
    sqlite3DbFree(db, z);
  }else{
    int iCol;
    if( alterFindCol(pParse, pTab, pCol, &iCol) ) return;
    zArg = sqlite3MPrintf(db, "sqlite_drop_colcons(%d, sql, %d, %d)",
                          iDb, iCol, eType);
  }

  /* Edit the SQL for the named table. */
  alterUpdateSchemaSql(pParse, pTab, zDb, "%s", zArg);
  sqlite3DbFree(db, zArg);

  /* Finally, reload the database schema. */
  renameReloadSchema(pParse, iDb, INITFLAG_AlterDropCons);
}

/*
** The implementation of SQL function sqlite_fail(MSG). This takes a single
** argument, and returns it as an error message with the error code set to
** SQLITE_CONSTRAINT.
*/
static void failConstraintFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  const char *zText = (const char*)sqlite3_value_text(argv[0]);
  int err = sqlite3_value_int(argv[1]);
  (void)NotUsed;
  sqlite3_result_error(ctx, zText, -1);
  sqlite3_result_error_code(ctx, err);
}

/*
** Buffer pCons, which is nCons bytes in size, contains the text of a 
** NOT NULL or CHECK constraint that will be inserted into a CREATE TABLE
** statement. If successful, this function returns the size of the buffer in
** bytes not including any trailing whitespace or "--" style comments. Or,
** if an OOM occurs, it returns 0 and sets db->mallocFailed to true.
**
** C-style comments at the end are preserved.  "--" style comments are
** removed because the comment terminator might be \000, and we are about
** to insert the pCons[] text into the middle of a larger string, and that
** will have the effect of removing the comment terminator and messing up
** the syntax.
*/
static int alterRtrimConstraint(
  sqlite3 *db,                    /* used to record OOM error */
  const char *pCons,              /* Buffer containing constraint */
  int nCons                       /* Size of pCons in bytes */
){
  u8 *zTmp = (u8*)sqlite3MPrintf(db, "%.*s", nCons, pCons);
  int iOff = 0;
  int iEnd = 0;

  if( zTmp==0 ) return 0;

  while( 1 ){
    int t = 0;
    int nToken = sqlite3GetToken(&zTmp[iOff], &t);
    if( t==TK_ILLEGAL ) break;
    if( t!=TK_SPACE && (t!=TK_COMMENT || zTmp[iOff]!='-') ){
      iEnd = iOff+nToken;
    }
    iOff += nToken;
  }

  sqlite3DbFree(db, zTmp);
  return iEnd;
}

/*
** Prepare a statement of the form:
**
**   ALTER TABLE pSrc ALTER pCol SET NOT NULL
*/
void sqlite3AlterSetNotNull(
  Parse *pParse,   /* Parsing context */
  SrcList *pSrc,   /* Name of the table being altered */
  Token *pCol,     /* Name of the column to add a NOT NULL constraint to */
  Token *pFirst    /* The NOT token of the NOT NULL constraint text */
){
  Table *pTab = 0;
  int iCol = 0;
  int iDb = 0;
  const char *zDb = 0;
  const char *pCons = 0;
  int nCons = 0;

  /* Look up the table being altered. */
  assert( pSrc->nSrc==1 );
  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, 0, 2);
  if( !pTab ) return;

  /* Find the column being altered. */
  if( alterFindCol(pParse, pTab, pCol, &iCol) ){
    return;
  }

  /* Find the length in bytes of the constraint definition */
  pCons = pFirst->z;
  nCons = alterRtrimConstraint(pParse->db, pCons, pParse->sLastToken.z - pCons);

  /* Search for a constraint violation. Throw an exception if one is found. */
  sqlite3NestedParse(pParse,
      "SELECT sqlite_fail('constraint failed', %d) "
      "FROM %Q.%Q AS x WHERE x.%.*s IS NULL", 
      SQLITE_CONSTRAINT, zDb, pTab->zName, (int)pCol->n, pCol->z
  );

  /* Edit the SQL for the named table. */
  alterUpdateSchemaSql(pParse, pTab, zDb,
      "sqlite_insert_constraint(%d, sqlite_drop_colcons(%d, sql, %d, %d),"
      " %.*Q, %d)",
      iDb, iDb, iCol, PARSELOC_NotNull, nCons, pCons, iCol
  );

  /* Finally, reload the database schema. */
  renameReloadSchema(pParse, iDb, INITFLAG_AlterAddCons);
}

/*
** Implementation of internal SQL function:
**
**     sqlite_find_constraint(SQL, CONSTRAINT-NAME)
**
** This function returns true if the SQL passed as the first argument is a
** CREATE TABLE that contains a constraint with the name CONSTRAINT-NAME,
** or false otherwise.
*/
static void findConstraintFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  const u8 *zSql = 0;
  const u8 *zCons = 0;
  int iOff = 0;
  int t = 0;

  (void)NotUsed;
  zSql = sqlite3_value_text(argv[0]);
  zCons = sqlite3_value_text(argv[1]);

  if( zSql==0 || zCons==0 ) return;
  while( t!=TK_LP && t!=TK_ILLEGAL ){
    iOff += sqlite3GetToken(&zSql[iOff], &t);
  }

  while( 1 ){
    iOff += getConstraintToken(&zSql[iOff], &t);
    if( t==TK_CONSTRAINT ){
      int nTok = 0;
      int cmp = 0;
      iOff += getWhitespace(&zSql[iOff]);
      nTok = getConstraintToken(&zSql[iOff], &t);
      if( quotedCompare(ctx, t, &zSql[iOff], nTok, zCons, &cmp) ) return;
      if( cmp==0 ){
        sqlite3_result_int(ctx, 1);
        return;
      }
    }else if( t==TK_ILLEGAL ){
      break;
    }
  }

  sqlite3_result_int(ctx, 0);
}

/*
** Generate bytecode to implement:
**
**    ALTER TABLE pSrc ADD [CONSTRAINT pName] CHECK(pExpr)
**
** Any "ON CONFLICT" text that occurs after the "CHECK(...)", up
** until pParse->sLastToken, is included as part of the new constraint.
*/
void sqlite3AlterAddConstraint(
  Parse *pParse,           /* Parse context */
  SrcList *pSrc,           /* Table to add constraint to */
  Token *pFirst,           /* First token of new constraint */
  Token *pName,            /* Name of new constraint. NULL if name omitted. */
  const char *zExpr,       /* Text of CHECK expression */
  int nExpr,               /* Size of pExpr in bytes */
  Expr *pExpr              /* The parsed CHECK expression */
){ 
  Table *pTab = 0;         /* Table identified by pSrc */
  int iDb = 0;             /* Which schema does pTab live in */
  const char *zDb = 0;     /* Name of the schema in which pTab lives */
  const char *pCons = 0;   /* Text of the constraint */
  int nCons;               /* Bytes of text to use from pCons[] */
  int rc;                  /* Result from error checking pExpr */

  /* Look up the table being altered. */
  assert( pSrc->nSrc==1 );
  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, 1, 2);
  if( !pTab ){
    sqlite3ExprDelete(pParse->db, pExpr);
    return;
  }

  /* Verify that the new CHECK constraint does not contain any
  ** internal-use-only function.  Forum post 2026-05-10T01:11:28Z
  */
  rc = sqlite3ResolveSelfReference(pParse, pTab, NC_IsCheck, pExpr, 0);
  sqlite3ExprDelete(pParse->db, pExpr);
  if( rc ) return;

  /* If this new constraint has a name, check that it is not a duplicate of
  ** an existing constraint. It is an error if it is.  */
  if( pName ){
    char *zName = sqlite3NameFromToken(pParse->db, pName);

    sqlite3NestedParse(pParse,
        "SELECT sqlite_fail('constraint %q already exists', %d) "
        "FROM \"%w\"." LEGACY_SCHEMA_TABLE " "
        "WHERE type='table' AND tbl_name=%Q COLLATE nocase "
        "AND sqlite_find_constraint(sql, %Q)",
        zName, SQLITE_ERROR, zDb, pTab->zName, zName
    );
    sqlite3DbFree(pParse->db, zName);
  }

  /* Search for a constraint violation. Throw an exception if one is found. */
  sqlite3NestedParse(pParse,
      "SELECT sqlite_fail('constraint failed', %d) "
      "FROM %Q.%Q WHERE (%.*s) IS NOT TRUE", 
      SQLITE_CONSTRAINT, zDb, pTab->zName, nExpr, zExpr
  );

  /* Edit the SQL for the named table. */
  pCons = pFirst->z;
  nCons = alterRtrimConstraint(pParse->db, pCons, pParse->sLastToken.z - pCons);

  alterUpdateSchemaSql(pParse, pTab, zDb,
      "sqlite_insert_constraint(%d, sql, %.*Q, -1)", iDb, nCons, pCons
  );

  /* Finally, reload the database schema. */
  renameReloadSchema(pParse, iDb, INITFLAG_AlterAddCons);
}

/* Reject a name already taken, splice zCons in, and reload the schema. */
static void alterAddConstraintText(
  Parse *pParse,
  Table *pTab,
  int iDb,
  const char *zDb,
  const char *zName,
  const char *zCons,
  int nCons,
  int iCol
){
  if( zName ){
    sqlite3NestedParse(pParse,
        "SELECT sqlite_fail('constraint %q already exists', %d) "
        "FROM \"%w\"." LEGACY_SCHEMA_TABLE " "
        "WHERE type='table' AND tbl_name=%Q COLLATE nocase "
        "AND sqlite_find_constraint(sql, %Q)",
        zName, SQLITE_ERROR, zDb, pTab->zName, zName
    );
  }

  alterUpdateSchemaSql(pParse, pTab, zDb,
      "sqlite_insert_constraint(%d, sql, %.*Q, %d)", iDb, nCons, zCons, iCol
  );

  renameReloadSchema(pParse, iDb, INITFLAG_AlterAddCons);
}

/* Number of automatic indexes on pTab. */
static int alterCountAutoIndex(Table *pTab){
  Index *pIdx;
  int n = 0;
  for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
    if( pIdx->idxType!=SQLITE_IDXTYPE_APPDEF ) n++;
  }
  return n;
}

/* True if a PRIMARY KEY over pList would make a column alias the rowid. */
static int alterPkIsRowidAlias(Table *pTab, ExprList *pList, const char **pzCol){
  Expr *pExpr;
  int iCol;

  if( pTab->tabFlags & TF_WithoutRowid ) return 0;
  if( pList==0 || pList->nExpr!=1 ) return 0;
  if( pList->a[0].fg.sortFlags & KEYINFO_ORDER_DESC ) return 0;
  pExpr = sqlite3ExprSkipCollate(pList->a[0].pExpr);
  if( pExpr==0 ) return 0;
  if( pExpr->op!=TK_ID && pExpr->op!=TK_STRING ) return 0;
  if( ExprHasProperty(pExpr, EP_IntValue) ) return 0;
  iCol = sqlite3ColumnIndex(pTab, pExpr->u.zToken);
  if( iCol<0 || pTab->aCol[iCol].eCType!=COLTYPE_INTEGER ) return 0;
  *pzCol = pTab->aCol[iCol].zCnName;
  return 1;
}

/* ALTER TABLE ADD CONSTRAINT <name> UNIQUE/PRIMARY KEY/FOREIGN KEY. */
void sqlite3AlterAddNamedConstraint(
  Parse *pParse,
  SrcList *pSrc,
  Token *pFirst,
  Token *pName,
  int eType,
  ExprList *pList,
  const char *zCols,
  int nCols
){
  sqlite3 *db = pParse->db;
  Table *pTab;
  int iDb = 0;
  const char *zDb = 0;
  char *zName = 0;
  const char *zCons;
  int nCons;

  assert( pSrc->nSrc==1 );
  assert( eType==ALTERCONS_Unique || eType==ALTERCONS_PrimaryKey
       || eType==ALTERCONS_ForeignKey );

  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, 1, 2);
  if( pTab==0 ) goto add_named_cons_exit;

  if( eType==ALTERCONS_PrimaryKey ){
    const char *zCol = 0;
    if( pTab->tabFlags & TF_HasPrimaryKey ){
      sqlite3ErrorMsg(pParse, "table \"%s\" has more than one primary key",
                      pTab->zName);
      goto add_named_cons_exit;
    }
    if( alterPkIsRowidAlias(pTab, pList, &zCol) ){
      sqlite3ErrorMsg(pParse,
          "cannot add an INTEGER PRIMARY KEY to table \"%s\": column \"%s\" "
          "would become an alias for the rowid", pTab->zName, zCol);
      goto add_named_cons_exit;
    }
  }

  zName = sqlite3NameFromToken(db, pName);
  if( zName==0 ) goto add_named_cons_exit;

  zCons = pFirst->z;
  nCons = alterRtrimConstraint(db, zCons, pParse->sLastToken.z - zCons);

  if( eType==ALTERCONS_ForeignKey ){
    alterAddConstraintText(pParse, pTab, iDb, zDb, zName, zCons, nCons, -1);

    if( db->flags & SQLITE_ForeignKeys ){
      pParse->colNamesSet = 1;
      sqlite3NestedParse(pParse,
          "SELECT sqlite_fail('foreign key constraint %q on %q failed', %d) "
          "FROM pragma_foreign_key_check(%Q,%Q)",
          zName, pTab->zName, SQLITE_CONSTRAINT, pTab->zName, zDb
      );
    }
  }else{
    char *zIdx = sqlite3MPrintf(db, "sqlite_autoindex_%s_%d",
                                pTab->zName, alterCountAutoIndex(pTab)+1);
    if( zIdx==0 ) goto add_named_cons_exit;

    if( eType==ALTERCONS_PrimaryKey && (pTab->tabFlags & TF_Strict)!=0 ){
      sqlite3NestedParse(pParse,
          "SELECT sqlite_fail('PRIMARY KEY %q on %q would be NULL', %d) "
          "FROM \"%w\".\"%w\" WHERE (%.*s) IS NULL",
          zName, pTab->zName, SQLITE_CONSTRAINT, zDb, pTab->zName, nCols, zCols
      );
    }

    sqlite3NestedParse(pParse,
        "CREATE UNIQUE INDEX \"%w\".\"%w\" ON \"%w\"(%.*s)",
        zDb, zIdx, pTab->zName, nCols, zCols
    );
    sqlite3NestedParse(pParse,
        "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET sql=NULL "
        "WHERE type='index' AND name=%Q COLLATE nocase",
        zDb, zIdx
    );
    sqlite3DbFree(db, zIdx);

    alterAddConstraintText(pParse, pTab, iDb, zDb, zName, zCons, nCons, -1);
  }

add_named_cons_exit:
  sqlite3ExprListDelete(db, pList);
  sqlite3DbFree(db, zName);
}

/* ALTER TABLE COLUMN <c> ADD DEFAULT <value>. */
void sqlite3AlterAddDefault(
  Parse *pParse,
  SrcList *pSrc,
  Token *pCol,
  Expr *pExpr,
  const char *zStart,
  const char *zEnd
){
  sqlite3 *db = pParse->db;
  Table *pTab;
  Column *pTabCol;
  int iDb = 0;
  int iCol = 0;
  const char *zDb = 0;
  char *zCons = 0;

  assert( pSrc->nSrc==1 );
  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, 0, 2);
  if( pTab==0 ) goto add_default_exit;
  if( alterFindCol(pParse, pTab, pCol, &iCol) ) goto add_default_exit;

  if( pExpr==0 || !sqlite3ExprIsConstantOrFunction(pExpr, 0) ){
    sqlite3ErrorMsg(pParse, "default value of column [%s] is not constant",
                    pTab->aCol[iCol].zCnName);
    goto add_default_exit;
  }
  pTabCol = &pTab->aCol[iCol];
  if( pTabCol->colFlags & COLFLAG_GENERATED ){
    sqlite3ErrorMsg(pParse, "cannot use DEFAULT on a generated column");
    goto add_default_exit;
  }
  if( sqlite3ColumnExpr(pTab, pTabCol)!=0 ){
    sqlite3ErrorMsg(pParse, "column \"%s\" already has a default value",
                    pTabCol->zCnName);
    goto add_default_exit;
  }
#ifndef SQLITE_OMIT_AUTHORIZATION
  if( db->xAuth ) sqlite3FuncAuth(pParse, pExpr);
#endif

  zCons = sqlite3MPrintf(db, "DEFAULT %.*s", (int)(zEnd - zStart), zStart);
  if( zCons==0 ) goto add_default_exit;

  alterAddConstraintText(pParse, pTab, iDb, zDb, 0, zCons,
                         sqlite3Strlen30(zCons), iCol);

add_default_exit:
  sqlite3ExprDelete(db, pExpr);
  sqlite3DbFree(db, zCons);
}

/* sqlite_drop_pk(ISCHEMA,SQL): remove the PRIMARY KEY clause. */
static void dropPkFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  int iSchema = sqlite3_value_int(argv[0]);
  const char *zSql = (const char*)sqlite3_value_text(argv[1]);
  AlterEdit x;
  ParseLoc *p;

  UNUSED_PARAMETER(NotUsed);
  if( !alterEditBegin(&x, db, iSchema, zSql) ) goto drop_pk_done;

  for(p=x.sParse.pLoc; p; p=p->pNext){
    if( p->eType==PARSELOC_PrimaryKey ) break;
  }
  if( p==0 ){
    x.rc = SQLITE_CORRUPT_BKPT;
    goto drop_pk_done;
  }
  if( !alterEditCopy(&x) ) goto drop_pk_done;

  x.nOut = alterExciseClause(x.zOut, x.nOut, x.zSql, &p->t);
  sqlite3_result_text(ctx, x.zOut, x.nOut, SQLITE_TRANSIENT);

drop_pk_done:
  alterEditFinish(&x, ctx);
}

/* Trailing number of an automatic index name, or 0. */
static int alterAutoIndexNumber(const char *zName){
  const char *z = zName ? strrchr(zName, '_') : 0;
  int n = 0;
  if( z==0 ) return 0;
  z++;
  while( sqlite3Isdigit(z[0]) ){
    n = n*10 + (z[0] - '0');
    z++;
  }
  return z[0]==0 ? n : 0;
}

/* sqlite_drop_fk(...): remove every FOREIGN KEY matching the given shape. */
static void dropFkFunc(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  int iSchema = sqlite3_value_int(argv[0]);
  const char *zSql = (const char*)sqlite3_value_text(argv[1]);
  const char *zTo = (const char*)sqlite3_value_text(argv[2]);
  int nChild = sqlite3_value_int(argv[3]);
  int nParent;
  Table *pTab;
  FKey *pFKey;
  ParseLoc *pLoc;
  AlterEdit x;
  int nFound = 0;
  int nKey, nRec, i;

  if( zTo==0 || nChild<=0 || argc<4+nChild ){

    return;
  }
  if( !alterEditBegin(&x, db, iSchema, zSql) ) goto drop_fk_done;
  nParent = argc - 4 - nChild;
  pTab = x.pTab;

  nKey = 0;
  for(pFKey=pTab->u.tab.pFKey; pFKey; pFKey=pFKey->pNextFrom) nKey++;
  nRec = 0;
  for(pLoc=x.sParse.pLoc; pLoc; pLoc=pLoc->pNext){
    if( pLoc->eType==PARSELOC_ForeignKey ) nRec++;
  }
  if( nKey!=nRec ){
    x.rc = SQLITE_CORRUPT_BKPT;
    goto drop_fk_done;
  }

  if( !alterEditCopy(&x) ) goto drop_fk_done;

  pLoc = x.sParse.pLoc;
  for(pFKey=pTab->u.tab.pFKey; pFKey; pFKey=pFKey->pNextFrom){
    int bMatch;
    while( pLoc && pLoc->eType!=PARSELOC_ForeignKey ) pLoc = pLoc->pNext;
    assert( pLoc!=0 );

    bMatch = pFKey->nCol==nChild && sqlite3StrICmp(pFKey->zTo, zTo)==0;
    for(i=0; bMatch && i<nChild; i++){
      const char *zWant = (const char*)sqlite3_value_text(argv[4+i]);
      int iFrom = pFKey->aCol[i].iFrom;
      assert( iFrom>=0 && iFrom<pTab->nCol );
      if( zWant==0
       || sqlite3StrICmp(pTab->aCol[iFrom].zCnName, zWant)!=0
      ){
        bMatch = 0;
      }
    }
    if( nParent!=0 && nParent!=nChild ) bMatch = 0;
    for(i=0; bMatch && i<nChild; i++){
      const char *zHave = pFKey->aCol[i].zCol;
      if( nParent==0 ){
        if( zHave!=0 ) bMatch = 0;
      }else{
        const char *zWant = (const char*)sqlite3_value_text(argv[4+nChild+i]);
        if( zHave==0 || zWant==0 || sqlite3StrICmp(zHave, zWant)!=0 ){
          bMatch = 0;
        }
      }
    }

    if( bMatch ){
      x.nOut = alterExciseClause(x.zOut, x.nOut, x.zSql, &pLoc->t);
      nFound++;
    }
    pLoc = pLoc->pNext;
  }

  if( nFound==0 ){
    errorMPrintf(ctx, "table \"%s\" has no such FOREIGN KEY", pTab->zName);
    goto drop_fk_done;
  }
  sqlite3_result_text(ctx, x.zOut, x.nOut, SQLITE_TRANSIENT);

drop_fk_done:
  alterEditFinish(&x, ctx);
}

/* ALTER TABLE DROP CONSTRAINT PRIMARY KEY. */
void sqlite3AlterDropPrimaryKey(Parse *pParse, SrcList *pSrc){
  sqlite3 *db = pParse->db;
  Table *pTab;
  Index *pPk;
  int iDb = 0;
  const char *zDb = 0;

  assert( pSrc->nSrc==1 );
  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, 1, 2);
  if( pTab==0 ) return;

  if( (pTab->tabFlags & TF_HasPrimaryKey)==0 ){
    sqlite3ErrorMsg(pParse, "table \"%s\" has no PRIMARY KEY", pTab->zName);
    return;
  }
  if( pTab->tabFlags & TF_WithoutRowid ){
    sqlite3ErrorMsg(pParse,
        "cannot drop the PRIMARY KEY of WITHOUT ROWID table \"%s\"",
        pTab->zName);
    return;
  }
  if( pTab->iPKey>=0 ){
    sqlite3ErrorMsg(pParse,
        "cannot drop an INTEGER PRIMARY KEY from table \"%s\": column \"%s\" "
        "holds the rowid", pTab->zName, pTab->aCol[pTab->iPKey].zCnName);
    return;
  }
  pPk = sqlite3PrimaryKeyIndex(pTab);
  if( pPk==0 ){
    sqlite3ErrorMsg(pParse, "no index for the PRIMARY KEY of \"%s\"",
                    pTab->zName);
    return;
  }

  alterUpdateSchemaSql(pParse, pTab, zDb, "sqlite_drop_pk(%d, sql)", iDb);

  sqlite3CodeDropIndex(pParse, pPk, iDb);

  {
    int n = alterAutoIndexNumber(pPk->zName);
    while( n>0 ){
      char *zOld = sqlite3MPrintf(db, "sqlite_autoindex_%s_%d", pTab->zName,n+1);
      int bFound = zOld!=0 && sqlite3FindIndex(db, zOld, zDb)!=0;
      if( bFound ){
        sqlite3NestedParse(pParse,
            "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE
            " SET name='sqlite_autoindex_%q_%d' "
            "WHERE type='index' AND name=%Q", zDb, pTab->zName, n, zOld
        );
      }
      sqlite3DbFree(db, zOld);
      if( !bFound ) break;
      n++;
    }
  }

  renameReloadSchema(pParse, iDb, INITFLAG_AlterDropCons);
}

/* sqlite_set_strict(ISCHEMA,SQL,BSEP): append the STRICT table-option. */
static void setStrictFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  int iSchema = sqlite3_value_int(argv[0]);
  const char *zSql = (const char*)sqlite3_value_text(argv[1]);
  int bSep = sqlite3_value_int(argv[2]);
  int nSql;
  char *zNew;

  UNUSED_PARAMETER(NotUsed);
  if( zSql==0 || iSchema<0 || iSchema>=db->nDb ) return;

  nSql = alterRtrimConstraint(db, zSql, sqlite3Strlen30(zSql));
  if( nSql<=0 ){
    sqlite3_result_error_code(ctx, db->mallocFailed ? SQLITE_NOMEM
                                                    : SQLITE_CORRUPT_BKPT);
    return;
  }

  zNew = sqlite3MPrintf(db, "%.*s%s STRICT", nSql, zSql, bSep ? "," : "");
  if( zNew==0 ){
    sqlite3_result_error_nomem(ctx);
    return;
  }
  sqlite3_result_text(ctx, zNew, -1, SQLITE_DYNAMIC);
}

/* sqlite_unset_strict(ISCHEMA,SQL): rebuild the option list without STRICT. */
static void unsetStrictFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  int iSchema = sqlite3_value_int(argv[0]);
  const char *zSql = (const char*)sqlite3_value_text(argv[1]);
  AlterEdit x;
  int nKeep;

  UNUSED_PARAMETER(NotUsed);
  if( !alterEditBegin(&x, db, iSchema, zSql) ) goto unset_strict_done;
  if( x.sParse.sColListEnd.z==0 ){

    x.rc = SQLITE_CORRUPT_BKPT;
    goto unset_strict_done;
  }

  nKeep = (int)(&x.sParse.sColListEnd.z[x.sParse.sColListEnd.n] - zSql);
  assert( nKeep>0 && nKeep<=sqlite3Strlen30(zSql) );
  x.zOut = sqlite3MPrintf(db, "%.*s%s", nKeep, zSql,
      (x.pTab->tabFlags & TF_WithoutRowid)!=0 ? " WITHOUT ROWID" : ""
  );
  if( x.zOut==0 ){
    x.rc = SQLITE_NOMEM_BKPT;
    goto unset_strict_done;
  }
  sqlite3_result_text(ctx, x.zOut, -1, SQLITE_TRANSIENT);

unset_strict_done:
  alterEditFinish(&x, ctx);
}

/* TF_ flag for a table-option name, or 0 if it is not one. */
static u32 alterTableOptionCode(Token *pOpt, int bWithout){
  if( bWithout ){
    if( pOpt->n==5 && sqlite3_strnicmp(pOpt->z, "rowid", 5)==0 ){
      return TF_WithoutRowid;
    }
    return 0;
  }
  if( pOpt->n==6 && sqlite3_strnicmp(pOpt->z, "strict", 6)==0 ){
    return TF_Strict;
  }
  return 0;
}

/* Prepare and run one statement, discarding any rows it returns. */
static int alterExecSql(sqlite3 *db, char **pzErrMsg, const char *zSql){
  sqlite3_stmt *pStmt = 0;
  int rc;

  if( zSql==0 ) return SQLITE_NOMEM_BKPT;
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc==SQLITE_OK ){
    while( sqlite3_step(pStmt)==SQLITE_ROW ){}
    rc = sqlite3_finalize(pStmt);
  }else{
    sqlite3_finalize(pStmt);
  }
  if( rc!=SQLITE_OK && *pzErrMsg==0 ){
    sqlite3SetString(pzErrMsg, db, sqlite3_errmsg(db));
  }
  return rc;
}

/* Run a SELECT returning one text value and return a copy of it. */
static char *alterQueryText(sqlite3 *db, int *pRc, char *zSql){
  sqlite3_stmt *pStmt = 0;
  char *zRet = 0;
  if( zSql==0 ){ *pRc = SQLITE_NOMEM_BKPT; return 0; }
  *pRc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( *pRc==SQLITE_OK ){
    if( sqlite3_step(pStmt)==SQLITE_ROW ){
      zRet = sqlite3DbStrDup(db, (const char*)sqlite3_column_text(pStmt, 0));
    }
    *pRc = sqlite3_finalize(pStmt);
  }else{
    sqlite3_finalize(pStmt);
  }
  sqlite3DbFree(db, zSql);
  return zRet;
}

/* Copy of a CREATE statement with the name it creates qualified by zDb. */
static char *alterQualifyDdl(sqlite3 *db, const char *zDb, const char *zSql){
  static const u8 aPhrase[] = { TK_IF, TK_NOT, TK_EXISTS };
  const unsigned char *z = (const unsigned char*)zSql;
  int i = 0;
  int iName = -1;
  int iIf = -1;
  int nMatch = 0;

  while( z[i] ){
    int t;
    int n = sqlite3GetToken(&z[i], &t);
    if( t!=TK_SPACE ){
      if( nMatch==0 ){
        if( t==TK_TABLE || t==TK_INDEX || t==TK_TRIGGER ) nMatch = 1;
      }else if( nMatch<4 && t==aPhrase[nMatch-1] ){
        if( nMatch==1 ) iIf = i;
        nMatch++;
      }else{
        iName = (nMatch<4 && iIf>=0) ? iIf : i;
        break;
      }
    }
    i += n;
  }
  if( iName<0 ) return sqlite3DbStrDup(db, zSql);
  return sqlite3MPrintf(db, "%.*s\"%w\".%s", iName, zSql, zDb, &zSql[iName]);
}

/* Append each CREATE statement zSql returns to *pazRedo, schema-qualified. */
static int alterCollectDdl(
  sqlite3 *db,
  char ***pazRedo,
  int *pnRedo,
  const char *zSchema,
  char *zSql
){
  sqlite3_stmt *pStmt = 0;
  int rc;

  if( zSql==0 ) return SQLITE_NOMEM_BKPT;
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3DbFree(db, zSql);
  if( rc!=SQLITE_OK ) return rc;
  while( sqlite3_step(pStmt)==SQLITE_ROW ){
    int n = *pnRedo;
    char **azNew = sqlite3DbRealloc(db, *pazRedo, (n+2)*sizeof(char*));
    if( azNew==0 ){ rc = SQLITE_NOMEM_BKPT; break; }
    *pazRedo = azNew;
    azNew[n] = alterQualifyDdl(db, zSchema,
                               (const char*)sqlite3_column_text(pStmt, 0));
    if( azNew[n]==0 ){ rc = SQLITE_NOMEM_BKPT; break; }
    azNew[n+1] = 0;
    *pnRedo = n+1;
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_finalize(pStmt);
  }else{
    sqlite3_finalize(pStmt);
  }
  return rc;
}

/* Copy of zSql with its table-option list and its name rewritten. */
static char *alterRewriteCreate(
  sqlite3 *db,
  int iDb,
  const char *zSql,
  u32 tabFlags,
  const char *zNewName
){
  Parse sParse;
  char *zNew = 0;
  int rc;

  rc = renameParseSql(&sParse, db->aDb[iDb].zDbSName, db, zSql, iDb==1);
  if( rc==SQLITE_OK && sParse.pNewTable!=0 && sParse.sColListEnd.z!=0 ){
    RenameToken *pName = renameTokenFind(&sParse, 0, sParse.pNewTable->zName);
    int nKeep = (int)(&sParse.sColListEnd.z[sParse.sColListEnd.n] - zSql);
    const char *zWr = (tabFlags & TF_WithoutRowid) ? " WITHOUT ROWID" : "";
    const char *zSep = ((tabFlags & TF_WithoutRowid)
                     && (tabFlags & TF_Strict)) ? "," : "";
    const char *zSt = (tabFlags & TF_Strict) ? " STRICT" : "";
    if( zNewName==0 ){
      zNew = sqlite3MPrintf(db, "%.*s%s%s%s", nKeep, zSql, zWr, zSep, zSt);
    }else if( pName ){
      int iName = (int)(pName->t.z - zSql);
      assert( iName>0 && iName + (int)pName->t.n <= nKeep );
      zNew = sqlite3MPrintf(db, "%.*s\"%w\"%.*s%s%s%s",
          iName, zSql,
          zNewName,
          nKeep - iName - (int)pName->t.n,
          &zSql[iName + pName->t.n],
          zWr, zSep, zSt);
    }
  }
  renameParseCleanup(&sParse);
  return zNew;
}

/* Copy of zSql with the declared type at pLoc replaced by zType. */
static char *alterSpliceType(
  sqlite3 *db,
  const char *zSql,
  const Token *pLoc,
  const char *zType
){
  int iStart = (int)(pLoc->z - zSql);
  int iEnd = iStart + (int)pLoc->n;
  assert( iStart>=0 && iEnd<=sqlite3Strlen30(zSql) );
  return sqlite3MPrintf(db, "%.*s%s%s%s", iStart, zSql,
                        pLoc->n==0 ? " " : "", zType, &zSql[iEnd]);
}

/* Copy of zSql with column zCol's declared type replaced by zType. */
static char *alterRetypeText(
  sqlite3 *db,
  int iDb,
  const char *zSql,
  const char *zCol,
  const char *zType,
  char **pzErr
){
  Parse sParse;
  Table *pTab;
  ParseLoc *p;
  char *zNew = 0;
  int iCol;

  if( renameParseSql(&sParse, db->aDb[iDb].zDbSName, db, zSql, iDb==1) ){
    goto retype_out;
  }
  pTab = sParse.pNewTable;
  if( pTab==0 || !IsOrdinaryTable(pTab) ) goto retype_out;
  iCol = alterColumnIndex(pTab, zCol);
  if( iCol<0 ){
    if( pzErr ) *pzErr = sqlite3MPrintf(db, "no such column: %s", zCol);
    goto retype_out;
  }
  for(p=sParse.pLoc; p; p=p->pNext){
    if( p->eType==PARSELOC_ColType && p->iCol==iCol ) break;
  }
  if( p==0 ) goto retype_out;

  zNew = alterSpliceType(db, zSql, &p->t, zType);

retype_out:
  renameParseCleanup(&sParse);
  return zNew;
}

struct AlterRebuild {
  const char *zCreate;
  const char *zCopy;
  const char *zRename;
  const char *zRestore;
  int nRedo;
  const char **azRedo;
};

/* Bytes one string occupies in a plan, terminator included. */
static i64 alterPlanLen(const char *z){
  return z ? (i64)sqlite3Strlen30(z)+1 : 0;
}

/* Copy zVal into the plan's tail at *pz and return where it landed. */
static const char *alterPlanStr(char **pz, const char *zVal){
  i64 n = alterPlanLen(zVal);
  char *zRet;
  if( n==0 ) return 0;
  zRet = *pz;
  memcpy(zRet, zVal, (size_t)n);
  *pz = &zRet[n];
  return zRet;
}

/* Package a rebuild plan for one phase.  Returns 0 on OOM. */
static AlterRebuild *alterRebuildNew(
  sqlite3 *db,
  const char *zCreate,
  const char *zCopy,
  const char *zRename,
  const char *zRestore,
  char **azRedo,
  int nRedo
){
  AlterRebuild *p;
  i64 nByte;
  char *z;
  int i;

  nByte = sizeof(*p) + (i64)nRedo*sizeof(char*)
        + alterPlanLen(zCreate) + alterPlanLen(zCopy)
        + alterPlanLen(zRename) + alterPlanLen(zRestore);
  for(i=0; i<nRedo; i++) nByte += alterPlanLen(azRedo[i]);

  p = sqlite3DbMallocZero(db, nByte);
  if( p==0 ) return 0;
  p->azRedo = (const char**)&p[1];
  p->nRedo = nRedo;
  z = (char*)&p->azRedo[nRedo];
  p->zCreate  = alterPlanStr(&z, zCreate);
  p->zCopy    = alterPlanStr(&z, zCopy);
  p->zRename  = alterPlanStr(&z, zRename);
  p->zRestore = alterPlanStr(&z, zRestore);
  for(i=0; i<nRedo; i++) p->azRedo[i] = alterPlanStr(&z, azRedo[i]);
  return p;
}

/* Implement OP_AlterTabOpt: run one phase of the plan pReb. */
int sqlite3RunAlterTabOpt(
  char **pzErrMsg,
  sqlite3 *db,
  int iDb,
  const AlterRebuild *pReb,
  int iPhase
){
  u64 savedFlags;
  int rc;
  int i;

  assert( iDb>=0 && iDb<db->nDb );
  assert( iPhase==1 || iPhase==2 );
  UNUSED_PARAMETER(iDb);

  sqlite3BtreeEnterAll(db);

  if( iPhase==1 ){

    rc = alterExecSql(db, pzErrMsg, pReb->zCreate);
    if( rc==SQLITE_OK ) rc = alterExecSql(db, pzErrMsg, pReb->zCopy);
  }else{
    savedFlags = db->flags;
    db->flags |= SQLITE_LegacyAlter;
    rc = alterExecSql(db, pzErrMsg, pReb->zRename);
    db->flags = savedFlags;

    if( rc==SQLITE_OK ){
      savedFlags = db->flags;
      db->flags |= SQLITE_WriteSchema;
      db->flags &= ~(u64)SQLITE_Defensive;
      rc = alterExecSql(db, pzErrMsg, pReb->zRestore);
      db->flags = savedFlags;
    }

    for(i=0; rc==SQLITE_OK && i<pReb->nRedo; i++){
      rc = alterExecSql(db, pzErrMsg, pReb->azRedo[i]);
    }
  }

  sqlite3BtreeLeaveAll(db);
  return rc;
}

/* Emit the check that looks for a row the new definition rejects. */
static void alterCheckExistingRows(
  Parse *pParse,
  Table *pTab,
  const char *zDb,
  const char *zOpt,
  int bOn
){
  pParse->colNamesSet = 1;
  sqlite3NestedParse(pParse,
      "SELECT sqlite_fail('cannot %s %s on %q: ' || quick_check, %d) "
      "FROM pragma_quick_check(%Q,%Q) "
      "WHERE quick_check GLOB 'non-* value in*' "
      "OR quick_check GLOB 'NULL value in*' "
      "OR quick_check GLOB 'TEXT value in*' "
      "OR quick_check GLOB 'NUMERIC value in*'",
      bOn ? "set" : "unset", zOpt, pTab->zName, SQLITE_CONSTRAINT,
      pTab->zName, zDb
  );
}

/* ALTER TABLE SET STRICT ON|OFF. */
static void alterSetStrict(
  Parse *pParse,
  Table *pTab,
  int iDb,
  const char *zDb,
  int bOn
){
  int ii;

  assert( IsOrdinaryTable(pTab) );
  if( bOn ){
    for(ii=0; ii<pTab->nCol; ii++){
      Column *pCol = &pTab->aCol[ii];
      if( pCol->eCType==COLTYPE_CUSTOM ){
        if( pCol->colFlags & COLFLAG_HASTYPE ){
          sqlite3ErrorMsg(pParse, "unknown datatype for %s.%s: \"%s\"",
              pTab->zName, pCol->zCnName, sqlite3ColumnType(pCol, "")
          );
        }else{
          sqlite3ErrorMsg(pParse, "missing datatype for %s.%s",
              pTab->zName, pCol->zCnName
          );
        }
        return;
      }
    }
  }

  sqlite3MayAbort(pParse);

  if( bOn ){
    alterUpdateSchemaSql(pParse, pTab, zDb, "sqlite_set_strict(%d, sql, %d)",
                         iDb, (pTab->tabFlags & TF_WithoutRowid)!=0);
  }else{
    alterUpdateSchemaSql(pParse, pTab, zDb, "sqlite_unset_strict(%d, sql)", iDb);
  }

  renameReloadSchema(pParse, iDb, INITFLAG_AlterSetOpt);
  alterCheckExistingRows(pParse, pTab, zDb, "STRICT", bOn);
}

/* Generate the three steps of a table rebuild and the plan they run. */
static void alterCodeRebuild(
  Parse *pParse,
  Table *pTab,
  int iDb,
  const char *zCol,
  const char *zType,
  u8 eWrOp
){
  sqlite3 *db = pParse->db;
  const char *zDb = db->aDb[iDb].zDbSName;
  AlterRebuild *pReb;
  SrcList *pDrop;
  Token tSchema, tName;
  char *zName = 0;
  char *zTmp = 0;
  char *zCols = 0;
  char *zOldSql = 0;
  char *zRetyped = 0;
  char *zFinal = 0;
  char *zCreate = 0;
  char *zCopy = 0;
  char *zRename = 0;
  char *zRestore = 0;
  char **azRedo = 0;
  int nRedo = 0;
  u32 flags;
  int rc = SQLITE_OK;
  int i;
  Vdbe *v;

  if( db->flags & SQLITE_ForeignKeys ){
    HashElem *k;
    Schema *pSchema = db->aDb[iDb].pSchema;
    for(k=sqliteHashFirst(&pSchema->tblHash); k; k=sqliteHashNext(k)){
      Table *pOther = sqliteHashData(k);
      FKey *pFKey;
      if( !IsOrdinaryTable(pOther) ) continue;
      for(pFKey=pOther->u.tab.pFKey; pFKey; pFKey=pFKey->pNextFrom){
        if( sqlite3_stricmp(pFKey->zTo, pTab->zName)==0 ){
          sqlite3ErrorMsg(pParse,
              "cannot rebuild %s while foreign keys are enabled: "
              "table %s refers to it", pTab->zName, pOther->zName);
          return;
        }
      }
    }
  }

  zName = sqlite3DbStrDup(db, pTab->zName);
  zTmp = sqlite3MPrintf(db, "altertab_%s", pTab->zName);
  if( eWrOp ){
    flags = (eWrOp==1 ? TF_WithoutRowid : 0) | (pTab->tabFlags & TF_Strict);
  }else{
    flags = pTab->tabFlags & (TF_WithoutRowid|TF_Strict);
  }
  for(i=0; i<pTab->nCol; i++){
    if( pTab->aCol[i].colFlags & COLFLAG_GENERATED ) continue;
    zCols = sqlite3MPrintf(db, "%z%s\"%w\"", zCols, zCols?",":"",
                           pTab->aCol[i].zCnName);
  }
  if( zName==0 || zTmp==0 || zCols==0 ) goto rebuild_exit;

  if( sqlite3FindTable(db, zTmp, zDb)!=0 ){
    sqlite3ErrorMsg(pParse, "cannot rebuild %s: table %s is in the way",
                    zName, zTmp);
    goto rebuild_exit;
  }

  zOldSql = alterQueryText(db, &rc, sqlite3MPrintf(db,
      "SELECT sql FROM \"%w\"." LEGACY_SCHEMA_TABLE
      " WHERE type='table' AND name=%Q COLLATE nocase", zDb, zName));
  if( rc!=SQLITE_OK ) goto rebuild_dberr;
  if( zOldSql==0 ){
    sqlite3ErrorMsg(pParse, "cannot rebuild %s: it has no CREATE statement",
                    zName);
    goto rebuild_exit;
  }

  rc = alterCollectDdl(db, &azRedo, &nRedo, zDb, sqlite3MPrintf(db,
      "SELECT sql FROM \"%w\"." LEGACY_SCHEMA_TABLE
      " WHERE tbl_name=%Q COLLATE nocase AND sql IS NOT NULL"
      " AND type IN ('index','trigger')", zDb, zName));
  if( rc!=SQLITE_OK ) goto rebuild_dberr;

  if( iDb!=1 && sqlite3FindTable(db, zName, db->aDb[1].zDbSName)==0 ){
    rc = alterCollectDdl(db, &azRedo, &nRedo, db->aDb[1].zDbSName,
      sqlite3MPrintf(db,
        "SELECT sql FROM \"%w\"." LEGACY_SCHEMA_TABLE
        " WHERE tbl_name=%Q COLLATE nocase AND sql IS NOT NULL"
        " AND type='trigger'", db->aDb[1].zDbSName, zName));
    if( rc!=SQLITE_OK ) goto rebuild_dberr;
  }

  if( zCol ){
    char *zErr = 0;
    zRetyped = alterRetypeText(db, iDb, zOldSql, zCol, zType, &zErr);
    if( zRetyped==0 ){
      if( zErr ) sqlite3ErrorMsg(pParse, "%s", zErr);
      sqlite3DbFree(db, zErr);
      goto rebuild_exit;
    }
  }
  {
    const char *zBase = zRetyped ? zRetyped : zOldSql;
    char *zTmpSql = alterRewriteCreate(db, iDb, zBase, flags, zTmp);
    if( zTmpSql ){
      zCreate = alterQualifyDdl(db, zDb, zTmpSql);
      sqlite3DbFree(db, zTmpSql);
    }
    zFinal = alterRewriteCreate(db, iDb, zBase, flags, 0);
  }
  if( zCreate==0 || zFinal==0 ){
    if( db->mallocFailed==0 ){
      sqlite3ErrorMsg(pParse, "cannot rebuild %s: cannot rewrite its "
                      "CREATE statement", zName);
    }
    goto rebuild_exit;
  }
  zCopy = sqlite3MPrintf(db,
      "INSERT INTO \"%w\".\"%w\"(%s) SELECT %s FROM \"%w\".\"%w\"",
      zDb, zTmp, zCols, zCols, zDb, zName);
  zRename = sqlite3MPrintf(db, "ALTER TABLE \"%w\".\"%w\" RENAME TO \"%w\"",
      zDb, zTmp, zName);
  zRestore = sqlite3MPrintf(db,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET sql=%Q"
      " WHERE type='table' AND name=%Q COLLATE nocase", zDb, zFinal, zName);
  if( zCopy==0 || zRename==0 || zRestore==0 ) goto rebuild_exit;

  v = sqlite3GetVdbe(pParse);
  if( v==0 ) goto rebuild_exit;
  sqlite3MayAbort(pParse);

  pReb = alterRebuildNew(db, zCreate, zCopy, 0, 0, 0, 0);
  if( pReb==0 ) goto rebuild_exit;
  sqlite3VdbeAddOp4(v, OP_AlterTabOpt, iDb, 0, 1, (char*)pReb, P4_DYNAMIC);

  sqlite3TokenInit(&tSchema, (char*)zDb);
  sqlite3TokenInit(&tName, zName);
  pDrop = sqlite3SrcListAppend(pParse, 0, &tSchema, &tName);
  if( pDrop==0 ) goto rebuild_exit;
  sqlite3DropTable(pParse, pDrop, 0, 0);

  pReb = alterRebuildNew(db, 0, 0, zRename, zRestore, azRedo, nRedo);
  if( pReb==0 ) goto rebuild_exit;
  sqlite3VdbeAddOp4(v, OP_AlterTabOpt, iDb, 0, 2, (char*)pReb, P4_DYNAMIC);
  goto rebuild_exit;

rebuild_dberr:
  if( db->mallocFailed==0 ){
    sqlite3ErrorMsg(pParse, "%s", sqlite3_errmsg(db));
  }

rebuild_exit:
  for(i=0; i<nRedo; i++) sqlite3DbFree(db, azRedo[i]);
  sqlite3DbFree(db, azRedo);
  sqlite3DbFree(db, zName);
  sqlite3DbFree(db, zTmp);
  sqlite3DbFree(db, zCols);
  sqlite3DbFree(db, zOldSql);
  sqlite3DbFree(db, zRetyped);
  sqlite3DbFree(db, zFinal);
  sqlite3DbFree(db, zCreate);
  sqlite3DbFree(db, zCopy);
  sqlite3DbFree(db, zRename);
  sqlite3DbFree(db, zRestore);
}

/* ALTER TABLE SET WITHOUT ROWID ON|OFF. */
static void alterSetWithoutRowid(
  Parse *pParse,
  Table *pTab,
  int iDb,
  int bOn
){
  assert( IsOrdinaryTable(pTab) );

  if( bOn ){
    if( (pTab->tabFlags & TF_HasPrimaryKey)==0 ){
      sqlite3ErrorMsg(pParse, "PRIMARY KEY missing on table %s", pTab->zName);
      return;
    }
    if( pTab->tabFlags & TF_Autoincrement ){
      sqlite3ErrorMsg(pParse,
          "AUTOINCREMENT not allowed on WITHOUT ROWID tables");
      return;
    }
  }

  alterCodeRebuild(pParse, pTab, iDb, 0, 0, bOn ? 1 : 2);
}

/* ALTER TABLE SET <table-option> ON|OFF. */
void sqlite3AlterSetTableOption(
  Parse *pParse,
  SrcList *pSrc,
  Token *pOpt,
  int bOn,
  int bWithout
){
  Table *pTab = 0;
  int iDb = 0;
  const char *zDb = 0;
  u32 optFlag;

  assert( pSrc->nSrc==1 );

  if( bOn<0 ){
    sqlite3SrcListDelete(pParse->db, pSrc);
    return;
  }

  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, 1, 3);
  if( !pTab ) return;

  optFlag = alterTableOptionCode(pOpt, bWithout);
  if( optFlag==0 ){
    if( bWithout ){
      sqlite3ErrorMsg(pParse, "unknown table option: WITHOUT %.*s",
                      pOpt->n, pOpt->z);
    }else{
      sqlite3ErrorMsg(pParse, "unknown table option: %.*s", pOpt->n, pOpt->z);
    }
    return;
  }

  if( ((pTab->tabFlags & optFlag)!=0)==(bOn!=0) ) return;

  if( optFlag==TF_Strict ){
    alterSetStrict(pParse, pTab, iDb, zDb, bOn);
  }else{
    assert( optFlag==TF_WithoutRowid );
    alterSetWithoutRowid(pParse, pTab, iDb, bOn);
  }
}

/* ALTER TABLE DROP FOREIGN KEY(<cols>) REFERENCES <tab>(<cols>). */
void sqlite3AlterDropForeignKey(
  Parse *pParse,
  SrcList *pSrc,
  ExprList *pFromCol,
  Token *pTo,
  ExprList *pToCol
){
  sqlite3 *db = pParse->db;
  Table *pTab;
  int iDb = 0;
  const char *zDb = 0;
  char *zArg = 0;
  char *zTo = 0;
  int i;

  assert( pSrc->nSrc==1 );
  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, 1, 2);
  if( pTab==0 ) goto drop_fk_exit;
  if( pFromCol==0 || pFromCol->nExpr<=0 ) goto drop_fk_exit;

  zTo = sqlite3NameFromToken(db, pTo);
  if( zTo==0 ) goto drop_fk_exit;

  zArg = sqlite3MPrintf(db, "sqlite_drop_fk(%d, sql, %Q, %d",
                        iDb, zTo, pFromCol->nExpr);
  for(i=0; zArg && i<pFromCol->nExpr; i++){
    zArg = sqlite3MPrintf(db, "%z, %Q", zArg, pFromCol->a[i].zEName);
  }
  for(i=0; zArg && pToCol && i<pToCol->nExpr; i++){
    zArg = sqlite3MPrintf(db, "%z, %Q", zArg, pToCol->a[i].zEName);
  }
  if( zArg==0 ) goto drop_fk_exit;
  zArg = sqlite3MPrintf(db, "%z)", zArg);
  if( zArg==0 ) goto drop_fk_exit;

  alterUpdateSchemaSql(pParse, pTab, zDb, "%s", zArg);

  renameReloadSchema(pParse, iDb, INITFLAG_AlterDropCons);

drop_fk_exit:
  sqlite3DbFree(db, zArg);
  sqlite3DbFree(db, zTo);
  sqlite3ExprListDelete(db, pFromCol);
  sqlite3ExprListDelete(db, pToCol);
}

/* ALTER TABLE DROP CHECK, the form that names no column. */
void sqlite3AlterDropCheck(Parse *pParse, SrcList *pSrc){
  Table *pTab;
  int iDb = 0;
  const char *zDb = 0;

  assert( pSrc->nSrc==1 );
  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, 1, 2);
  if( pTab==0 ) return;

  alterUpdateSchemaSql(pParse, pTab, zDb, "sqlite_drop_colcons(%d, sql, -1, %d)",
                       iDb, PARSELOC_Check);

  renameReloadSchema(pParse, iDb, INITFLAG_AlterDropCons);
}

/* sqlite_set_coltype(ISCHEMA,SQL,ICOL,TYPE): retype a column, affinity kept. */
static void setColTypeFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  int iSchema = sqlite3_value_int(argv[0]);
  const char *zSql = (const char*)sqlite3_value_text(argv[1]);
  int iCol = sqlite3_value_int(argv[2]);
  const char *zType = (const char*)sqlite3_value_text(argv[3]);
  Table *pTab;
  ParseLoc *p;
  AlterEdit x;
  char aOld, aNew;

  UNUSED_PARAMETER(NotUsed);
  if( zType==0 || iCol<0 ) return;
  if( !alterEditBegin(&x, db, iSchema, zSql) ) goto set_coltype_done;
  pTab = x.pTab;

  if( iCol>=pTab->nCol ){
    x.rc = SQLITE_CORRUPT_BKPT;
    goto set_coltype_done;
  }
  for(p=x.sParse.pLoc; p; p=p->pNext){
    if( p->eType==PARSELOC_ColType && p->iCol==iCol ) break;
  }
  if( p==0 ){
    x.rc = SQLITE_CORRUPT_BKPT;
    goto set_coltype_done;
  }

  aOld = pTab->aCol[iCol].affinity;
  aNew = sqlite3AffinityType(zType, 0);
  if( aOld!=aNew ){
    errorMPrintf(ctx, "cannot change the type of column \"%s\" to \"%s\": "
                 "that changes its affinity, and the rows and index entries "
                 "already stored were written under the old one",
                 pTab->aCol[iCol].zCnName, zType);
    goto set_coltype_done;
  }

  if( alterColInPk(pTab, iCol) ){
    Token t;
    int bWasInt, bIsInt;
    t.z = p->t.z;
    t.n = p->t.n;
    while( t.n>0 && sqlite3Isspace(t.z[0]) ){ t.z++; t.n--; }
    while( t.n>0 && sqlite3Isspace(t.z[t.n-1]) ){ t.n--; }
    bWasInt = t.n==7 && sqlite3_strnicmp(t.z, "INTEGER", 7)==0;
    bIsInt = sqlite3StrICmp(zType, "INTEGER")==0;
    if( bWasInt!=bIsInt ){
      errorMPrintf(ctx, "cannot change the type of PRIMARY KEY column "
                   "\"%s\" to \"%s\": only a column declared exactly INTEGER "
                   "holds the rowid, so this moves where its values live",
                   pTab->aCol[iCol].zCnName, zType);
      goto set_coltype_done;
    }
  }

  x.zOut = alterSpliceType(db, zSql, &p->t, zType);
  if( x.zOut==0 ){
    x.rc = SQLITE_NOMEM_BKPT;
    goto set_coltype_done;
  }
  sqlite3_result_text(ctx, x.zOut, -1, SQLITE_TRANSIENT);

set_coltype_done:
  alterEditFinish(&x, ctx);
}

/* ALTER TABLE COLUMN <c> SET TYPE <type>. */
void sqlite3AlterSetColumnType(
  Parse *pParse,
  SrcList *pSrc,
  Token *pCol,
  Token *pType
){
  sqlite3 *db = pParse->db;
  Table *pTab;
  int iDb = 0;
  int iCol;
  const char *zDb = 0;
  char *zCol = 0;
  char *zType = 0;
  int bRebuild;

  assert( pSrc->nSrc==1 );
  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, 0, 2);
  if( pTab==0 ) return;
  if( pType->n==0 ){
    sqlite3ErrorMsg(pParse, "no type given for column \"%T\"", pCol);
    return;
  }
  if( alterFindCol(pParse, pTab, pCol, &iCol) ) return;
  zCol = sqlite3NameFromToken(db, pCol);
  zType = sqlite3DbStrNDup(db, pType->z, pType->n);
  if( zCol==0 || zType==0 ) goto set_type_exit;

  bRebuild = pTab->aCol[iCol].affinity!=sqlite3AffinityType(zType, 0);
  if( !bRebuild && (pTab->tabFlags & TF_WithoutRowid)==0 ){
    if( alterColInPk(pTab, iCol)
     && (pTab->iPKey==iCol)!=(sqlite3StrICmp(zType,"INTEGER")==0)
    ){
      bRebuild = 1;
    }
  }

  if( bRebuild ){
    alterCodeRebuild(pParse, pTab, iDb, zCol, zType, 0);
  }else{
    alterUpdateSchemaSql(pParse, pTab, zDb,
        "sqlite_set_coltype(%d, sql, %d, %Q)", iDb, iCol, zType
    );
    renameReloadSchema(pParse, iDb, INITFLAG_AlterSetType);
  }

set_type_exit:
  sqlite3DbFree(db, zCol);
  sqlite3DbFree(db, zType);
}

/*
** Register built-in functions used to help implement ALTER TABLE
*/
void sqlite3AlterFunctions(void){
  static FuncDef aAlterTableFuncs[] = {
    INTERNAL_FUNCTION(sqlite_rename_column,  9, renameColumnFunc),
    INTERNAL_FUNCTION(sqlite_rename_table,   7, renameTableFunc),
    INTERNAL_FUNCTION(sqlite_rename_test,    7, renameTableTest),
    INTERNAL_FUNCTION(sqlite_drop_column,    3, dropColumnFunc),
    INTERNAL_FUNCTION(sqlite_rename_quotefix,2, renameQuotefixFunc),
    INTERNAL_FUNCTION(sqlite_drop_constraint,2, dropConstraintFunc),
    INTERNAL_FUNCTION(sqlite_drop_colcons,   4, dropColConsFunc),
    INTERNAL_FUNCTION(sqlite_set_coltype,    4, setColTypeFunc),
    INTERNAL_FUNCTION(sqlite_drop_fk,       -1, dropFkFunc),
    INTERNAL_FUNCTION(sqlite_drop_pk,        2, dropPkFunc),
    INTERNAL_FUNCTION(sqlite_fail,           2, failConstraintFunc),
    INTERNAL_FUNCTION(sqlite_insert_constraint,4,insertConstraintFunc),
    INTERNAL_FUNCTION(sqlite_find_constraint,2, findConstraintFunc),
    INTERNAL_FUNCTION(sqlite_set_strict,     3, setStrictFunc),
    INTERNAL_FUNCTION(sqlite_unset_strict,   2, unsetStrictFunc),
  };
  sqlite3InsertBuiltinFuncs(aAlterTableFuncs, ArraySize(aAlterTableFuncs));
}
#endif  /* SQLITE_ALTER_TABLE */
