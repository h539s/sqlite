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

/*
** Each ParseLoc object records where one piece of a CREATE TABLE statement
** lives within the text being parsed.
**
** Where a RenameToken anchors on the identity of a parse-tree element, a
** ParseLoc anchors on the ordinal of the column it belongs to.  It has to:
** the things recorded here leave no addressable object behind for a
** RenameToken to point at - a NOT NULL constraint becomes four bits in
** Column.notNull, and the end of a column definition is not an object at
** all.  A column index always exists.
**
** eType says what was recorded:
**
**   PARSELOC_NotNull   The extent of a NOT NULL constraint, taken in by
**                      ALTER TABLE ... DROP NOT NULL.  A column can carry
**                      more than one, so there can be several entries with
**                      the same iCol.
**
**   PARSELOC_ColDef    An insertion point inside a column definition: the
**                      first byte past the column's type, or past its name
**                      when the type was omitted.  Anything spliced in
**                      there becomes a constraint on that column.  Exactly
**                      one entry per column.
**
**   PARSELOC_PrimaryKey
**                      The extent of a PRIMARY KEY clause, taken in by
**                      ALTER TABLE ... DROP CONSTRAINT PRIMARY KEY.  A
**                      table has at most one, written either on a column
**                      or on the table, so iCol is -1 for both forms and
**                      the clause is found by kind.
**
**   PARSELOC_Default   The extent of a DEFAULT clause, taken in by
**                      ALTER TABLE ... COLUMN <c> DROP DEFAULT.  A column
**                      can carry more than one - SQLite lets the last win
**                      - so there can be several entries with the same
**                      iCol.
**
** Objects are only created while IN_RENAME_OBJECT, which means only during
** the reparse of a stored schema statement performed by renameParseSql().
** The extent t therefore always points into the same string that the
** caller is about to edit.
**
** Created by sqlite3ParseLocAdd() and consumed by dropColConsFunc(),
** dropPkFunc() and insertConstraintFunc(), all further down in this file.
*/
struct ParseLoc {
  u8 eType;              /* One of the PARSELOC_* values */
  int iCol;              /* Index of the column this belongs to */
  Token t;               /* Extent of the recorded text */
  ParseLoc *pNext;       /* Next location from the same parse */
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


/*
** Trim trailing whitespace and comments from the extent that starts at
** zStart and ends immediately before zEnd.  Return the trimmed length.
**
** Tokens tile the input exactly, and zEnd is always a token boundary, so
** this never needs to look at a partial token.
*/
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

/*
** Record the location of a NOT NULL constraint on column iCol.  zStart
** points at the "NOT" keyword and zEnd just past the "NULL" keyword.
**
** The constraint may extend past zEnd with an ON CONFLICT clause.  At the
** point this is called Parse.sLastToken holds the parser's lookahead -
** the first token after the whole constraint - so it bounds the extent
** from above.  The bound is generous (it includes any intervening
** whitespace and comments) and notNullRtrim() pulls it back to the last
** real token.
**
** If the constraint is named, and the "CONSTRAINT <name>" clause is
** separated from the "NOT" keyword by nothing but whitespace and
** comments, then the name is part of this constraint and the extent is
** widened to the left to take it in.
*/
void sqlite3ConsLocAdd(
  Parse *pParse,        /* Parsing context */
  u8 eType,             /* PARSELOC_NotNull, _PrimaryKey or _Default */
  int iCol,             /* Column being constrained, or -1 */
  const char *zStart,   /* First byte of the constraint keyword */
  const char *zEnd      /* First byte past that keyword */
){
  const char *zKw;
  const char *zLimit;

  assert( pParse->isCreate );
  assert( zStart!=0 && zEnd!=0 && zEnd>zStart );

  /* Widen to the left over an immediately preceding "CONSTRAINT <name>". */
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

  /* Extend to the right as far as the parser's lookahead allows, then pull
  ** back to the last real token.  The clause can carry more than the two
  ** keywords it starts with - a sort order, an ON CONFLICT, a column list -
  ** and Parse.sLastToken is the first token after all of it. */
  zLimit = pParse->sLastToken.z;
  if( zLimit==0 || zLimit<zEnd ) zLimit = zEnd;
  zEnd = &zStart[notNullRtrim(zStart, zLimit)];

  sqlite3ParseLocAdd(pParse, eType, iCol, zStart, zEnd);
}

/*
** Widen the recorded extent of the column being parsed so that it reaches
** the end of the constraint just reduced.  Called once per column
** constraint, so the last call for a column leaves the extent ending where
** that column's definition ends - which is where ALTER TABLE splices a new
** column-constraint in, and matches where one written by hand would go.
**
** Parse.sLastToken holds the parser's lookahead, the first token after the
** constraint, so it bounds the extent from above; notNullRtrim() pulls it
** back to the last real token.
*/
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

/*
** Record where a CHECK constraint sits, so that ALTER TABLE ... DROP CHECK
** can cut it out without looking for it.
**
** pKw is the CHECK keyword.  sqlite3ConsLocAdd() takes in everything that
** follows it up to the end of the last real token - the parenthesised
** expression, and the ON CONFLICT clause a table-level CHECK may carry -
** and an immediately preceding "CONSTRAINT <name>".
**
** bCol says which form was written: the column being defined, or -1 for a
** constraint written at the end of the list.  SQLite draws no distinction
** between the two - pTab->pCheck is a flat list, a CHECK written on a
** column may refer to any column of the table, and it is enforced exactly
** as a table-level one is - so where it was written is the only thing that
** tells them apart, and it is what DROP CHECK partitions them by.
*/
void sqlite3CheckLocAdd(Parse *pParse, Token *pKw, int bCol){
  Table *p = pParse->pNewTable;
  int iCol = -1;
  assert( IN_RENAME_OBJECT );
  if( p==0 ) return;
  if( bCol ){
    if( p->nCol<=0 ) return;
    iCol = p->nCol-1;
  }
  sqlite3ConsLocAdd(pParse, PARSELOC_Check, iCol, pKw->z, &pKw->z[pKw->n]);
}

/*
** Extend the most recently recorded FOREIGN KEY extent to zEnd, so that a
** column-level DEFERRABLE clause leaves with the key it belongs to.
**
** The table form carries its DEFERRABLE clause in the same grammar rule, so
** the extent already covers it.  The column form does not: "a REFERENCES
** p(x) DEFERRABLE INITIALLY DEFERRED" is two ccons, and the extent recorded
** when the key was created stops at the end of the first.
**
** Left behind, that clause would not be inert.  SQLite applies a DEFERRABLE
** clause to whichever key is the most recently created one, so after the
** key it was written for is gone it would silently defer a different key.
**
** It is only this key's clause if it sits immediately after the extent, so
** that is what is checked.  A DEFERRABLE written on a column that has no
** REFERENCES of its own is somewhere else entirely, and the extent is left
** as it was.
*/
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

/*
** Record where a column's DEFAULT clause sits, so that ALTER TABLE ...
** COLUMN <c> DROP DEFAULT can cut it out without looking for it.
**
** pKw is the DEFAULT keyword.  sqlite3ConsLocAdd() takes in everything that
** follows it up to the end of the last real token - the value, however it
** was written - and an immediately preceding "CONSTRAINT <name>".
**
** The clause belongs to the column being defined, which is the last one
** added so far.  The five grammar rules for DEFAULT all reduce while that
** is still true.
*/
void sqlite3DefaultLocAdd(Parse *pParse, Token *pKw){
  Table *p = pParse->pNewTable;
  assert( IN_RENAME_OBJECT );
  if( p==0 || p->nCol<=0 ) return;
  sqlite3ConsLocAdd(pParse, PARSELOC_Default, p->nCol-1,
                    pKw->z, &pKw->z[pKw->n]);
}

/*
** Record one position within the text being parsed.  See the comment on
** struct ParseLoc for what the eType values mean.  zEnd may equal zStart,
** which records a bare position rather than an extent.
**
** Only ever called while IN_RENAME_OBJECT, so the recorded pointers are
** into the stored statement that the caller is about to edit.
*/
void sqlite3ParseLocAdd(
  Parse *pParse,        /* Parsing context */
  u8 eType,             /* PARSELOC_NotNull or PARSELOC_ColDef */
  int iCol,             /* Index of the column this belongs to */
  const char *zStart,   /* First byte of the extent */
  const char *zEnd      /* First byte past the extent */
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

/*
** Free a list of ParseLoc objects.
*/
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


/*
** Find the column named zCol in pTab, which is a table as just reparsed out
** of the text held in sqlite_schema.  Returns the column index, or -1.
**
** The editors are handed column names rather than column indexes on purpose.
** An index would have to be resolved from the in-memory Table at prepare
** time, and the in-memory Table is a cache of the stored text kept in step
** by the schema cookie - a cookie that an edit to sqlite_schema under
** writable_schema does not touch.  Resolving the name here, against the text
** about to be edited, means a stale cache cannot send the edit to the wrong
** column.
*/
static int alterColumnIndex(Table *pTab, const char *zCol){
  int i;
  if( pTab==0 || zCol==0 ) return -1;
  for(i=0; i<pTab->nCol; i++){
    if( sqlite3_stricmp(pTab->aCol[i].zCnName, zCol)==0 ) return i;
  }
  return -1;
}

/*
** Cut the clause whose extent is pLoc out of the statement held in zOut,
** which is nOut bytes long and started life as a copy of zSql.  Return the
** new length.
**
** The whitespace and comments on either side of the clause go with it.  If
** what comes next closes the list or separates it, the neighbours can abut
** - "a INT NOT NULL, b" becomes "a INT, b" and not "a INT , b" - and a
** comma sitting in front of the clause belongs to it, so that removing a
** table-constraint does not leave the list with a hole.  Otherwise exactly
** one space is left behind to keep the neighbours apart.
**
** A comment in front of the clause is deliberately left alone.  It belongs
** to whatever precedes it, not to the clause being removed.
**
** Offsets are taken relative to zSql rather than zOut so that a caller
** removing several clauses can work right to left: every extent not yet
** used still addresses the same byte of zOut that it addressed in zSql.
*/
static int alterExciseClause(
  char *zOut,             /* Statement being edited, in place */
  int nOut,               /* Current length of zOut */
  const char *zSql,       /* The original text the extents point into */
  const Token *pLoc       /* Extent of the clause to remove */
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

/*
** Shared implementation of the internal SQL functions
**
**     sqlite_drop_notnull(ISCHEMA, SQL, COLNAME)
**     sqlite_drop_default(ISCHEMA, SQL, COLNAME)
**
** SQL is a CREATE TABLE statement belonging to schema ISCHEMA.  Return a
** copy of that statement with every constraint of kind eType on the column
** named COLNAME removed.  The name is resolved against SQL itself - see
** alterColumnIndex().  If the column carries no such constraint the
** statement is returned unchanged, which follows postgres and matches what
** the INTEGER form of sqlite_drop_constraint() does.
**
** This is the "technique C" counterpart of sqlite_drop_constraint().
** Instead of scanning the text for the constraint, it reparses the
** statement and reads the extents that sqlite3ConsLocAdd() recorded during
** that parse.  Because the parser - not a heuristic scan - decides which
** column each constraint belongs to, a column carrying more than one such
** clause has all of them removed.  SQLite accepts both "a NOT NULL NOT
** NULL" and "a DEFAULT 1 DEFAULT 2"; the scanning implementation removes
** only the first, silently leaving the rest in force.
*/
static void dropColConsFunc(
  sqlite3_context *ctx,
  sqlite3_value **argv,
  u8 eType,                       /* Kind of clause to remove */
  int bTabCons                    /* True if a NULL COLNAME is allowed, and
                                  ** means the table-level constraints */
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  int iSchema = sqlite3_value_int(argv[0]);
  const char *zSql = (const char*)sqlite3_value_text(argv[1]);
  const char *zCol = (const char*)sqlite3_value_text(argv[2]);
  const char *zDb;
  Table *pTab;
  Parse sParse;
  char *zOut = 0;
  int nOut = 0;
  int iCol;
  int rc;
#ifndef SQLITE_OMIT_AUTHORIZATION
  sqlite3_xauth xAuth = db->xAuth;
  db->xAuth = 0;
#endif

  if( zSql==0 || iSchema<0 || iSchema>=db->nDb
   || (zCol==0 && !bTabCons)
  ){
    rc = SQLITE_OK;
    goto drop_notnull_done;
  }
  zDb = db->aDb[iSchema].zDbSName;

  rc = renameParseSql(&sParse, zDb, db, zSql, iSchema==1);
  if( rc!=SQLITE_OK ){
    /* The stored statement does not parse.  It is the definition of the very
    ** table being altered, so there is nothing sensible to do with it. */
    rc = SQLITE_CORRUPT_BKPT;
    goto drop_notnull_cleanup;
  }
  pTab = sParse.pNewTable;
  if( pTab==0 || !IsOrdinaryTable(pTab) ){
    rc = SQLITE_CORRUPT_BKPT;
    goto drop_notnull_cleanup;
  }
  if( zCol==0 ){
    /* No column named: the clauses written at table level, which are the
    ** ones recorded against no column. */
    iCol = -1;
    goto drop_notnull_edit;
  }
  iCol = alterColumnIndex(pTab, zCol);
  if( iCol<0 ){
    /* The stored definition has no such column.  That definition is what the
    ** table will be once the schema is next loaded, so this is a real "no
    ** such column", not a corrupt statement. */
    errorMPrintf(ctx, "no such column: %s", zCol);
    rc = SQLITE_OK;
    goto drop_notnull_cleanup;
  }

drop_notnull_edit:
  nOut = sqlite3Strlen30(zSql);
  zOut = sqlite3DbMallocRaw(db, (i64)nOut+1);
  if( zOut==0 ){
    rc = SQLITE_NOMEM_BKPT;
    goto drop_notnull_cleanup;
  }
  memcpy(zOut, zSql, (size_t)nOut+1);

  /* Excise the right-most constraint still to be removed, then repeat.
  ** Working right to left means every offset not yet used still addresses
  ** the same byte of zOut that it addressed in zSql.  Entries that have
  ** been dealt with are marked by setting iCol to -1 rather than being
  ** unlinked, so that the list stays owned by sParse. */
  while( 1 ){
    ParseLoc *p;
    ParseLoc *pBest = 0;

    for(p=sParse.pLoc; p; p=p->pNext){
      if( p->eType!=eType || p->iCol!=iCol ) continue;
      if( pBest==0 || p->t.z>pBest->t.z ) pBest = p;
    }
    if( pBest==0 ) break;
    pBest->eType = 0;      /* Mark it done.  iCol cannot be used for this:
                           ** -1 is a real value, meaning a table-level
                           ** constraint, and is what the form that names
                           ** no column matches on. */
    nOut = alterExciseClause(zOut, nOut, zSql, &pBest->t);
  }

  sqlite3_result_text(ctx, zOut, nOut, SQLITE_TRANSIENT);

drop_notnull_cleanup:
  renameParseCleanup(&sParse);
  sqlite3DbFree(db, zOut);

drop_notnull_done:
#ifndef SQLITE_OMIT_AUTHORIZATION
  db->xAuth = xAuth;
#endif
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
  }
}

/*
** Internal SQL function sqlite_drop_notnull(ISCHEMA, SQL, COLNAME).
*/
static void dropNotNullFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  UNUSED_PARAMETER(NotUsed);
  dropColConsFunc(ctx, argv, PARSELOC_NotNull, 0);
}

/*
** Internal SQL function sqlite_drop_default(ISCHEMA, SQL, COLNAME).
*/
static void dropDefaultFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  UNUSED_PARAMETER(NotUsed);
  dropColConsFunc(ctx, argv, PARSELOC_Default, 0);
}

/*
** Internal SQL function sqlite_drop_check(ISCHEMA, SQL, COLNAME).
**
** The two forms partition the table's CHECK constraints by where each one
** was written.  A NULL COLNAME removes those written at table level, after
** the column list; otherwise those written inside the named column's
** definition go.  Neither reaches the other's, whatever the constraints
** happen to mention.
*/
static void dropCheckFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  UNUSED_PARAMETER(NotUsed);
  dropColConsFunc(ctx, argv, PARSELOC_Check, 1);
}

/*
** Internal SQL function:
**
**     sqlite_insert_constraint(ISCHEMA, SQL, CONSTRAINT-TEXT, COLNAME)
**
** SQL is a CREATE TABLE statement belonging to schema ISCHEMA.  Return a
** copy of it with CONSTRAINT-TEXT spliced in.
**
** A NULL COLNAME adds a table-constraint: the text goes in just before the
** ")" that closes the column and constraint list, introduced by a comma, so
** it becomes the last constraint of the table.  Otherwise the text goes into
** the definition of the named column, where it becomes a constraint on that
** column.  The name is resolved against SQL itself - see
** alterColumnIndex().
**
** Neither position is found by searching.  The statement is reparsed and
** both come from what the parser recorded during that parse:
** Parse.sColListEnd for the closing ")", and the PARSELOC_ColDef entry for
** the column, whose
** extent ends where that column's definition does.  No text work is left.
*/
static void insertConstraintFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  int iSchema = sqlite3_value_int(argv[0]);
  const char *zSql = (const char*)sqlite3_value_text(argv[1]);
  const char *zCons = (const char*)sqlite3_value_text(argv[2]);
  const char *zCol = (const char*)sqlite3_value_text(argv[3]);
  const char *zDb;
  Table *pTab;
  Parse sParse;
  char *zNew;
  int iOff;
  int iCol;
  int rc;
#ifndef SQLITE_OMIT_AUTHORIZATION
  sqlite3_xauth xAuth = db->xAuth;
  db->xAuth = 0;
#endif

  UNUSED_PARAMETER(NotUsed);
  if( zSql==0 || zCons==0 || iSchema<0 || iSchema>=db->nDb ){
    rc = SQLITE_OK;
    goto insert_cons_done;
  }
  zDb = db->aDb[iSchema].zDbSName;

  rc = renameParseSql(&sParse, zDb, db, zSql, iSchema==1);
  if( rc!=SQLITE_OK ){
    /* The stored statement does not parse.  It is the definition of the very
    ** table being altered, so there is nothing sensible to do with it. */
    rc = SQLITE_CORRUPT_BKPT;
    goto insert_cons_cleanup;
  }
  pTab = sParse.pNewTable;
  if( pTab==0 || !IsOrdinaryTable(pTab) ){
    rc = SQLITE_CORRUPT_BKPT;
    goto insert_cons_cleanup;
  }
  /* A NULL column name asks for a table-constraint.  Otherwise the name is
  ** resolved against the stored text, not against the in-memory Table. */
  if( zCol==0 ){
    iCol = -1;
  }else{
    iCol = alterColumnIndex(pTab, zCol);
    if( iCol<0 ){
      errorMPrintf(ctx, "no such column: %s", zCol);
      rc = SQLITE_OK;
      goto insert_cons_cleanup;
    }
  }

  if( iCol<0 ){
    if( sParse.sColListEnd.z==0 ){
      rc = SQLITE_CORRUPT_BKPT;
      goto insert_cons_cleanup;
    }
    iOff = (int)(sParse.sColListEnd.z - zSql);
    zNew = sqlite3MPrintf(db, "%.*s, %s%s", iOff, zSql, zCons, &zSql[iOff]);
  }else{
    ParseLoc *p;
    for(p=sParse.pLoc; p; p=p->pNext){
      if( p->eType==PARSELOC_ColDef && p->iCol==iCol ) break;
    }
    if( p==0 ){
      rc = SQLITE_CORRUPT_BKPT;
      goto insert_cons_cleanup;
    }
    iOff = (int)(p->t.z - zSql) + (int)p->t.n;
    zNew = sqlite3MPrintf(db, "%.*s %s%s", iOff, zSql, zCons, &zSql[iOff]);
  }
  if( zNew==0 ){
    rc = SQLITE_NOMEM_BKPT;
    goto insert_cons_cleanup;
  }
  sqlite3_result_text(ctx, zNew, -1, SQLITE_TRANSIENT);
  sqlite3DbFree(db, zNew);

insert_cons_cleanup:
  renameParseCleanup(&sParse);

insert_cons_done:
#ifndef SQLITE_OMIT_AUTHORIZATION
  db->xAuth = xAuth;
#endif
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
  }
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

/*
** Generate bytecode for one of:
**
**  (1)   ALTER TABLE pSrc DROP CONSTRAINT pCons
**  (2)   ALTER TABLE pSrc ALTER pCol DROP <kind>
**
** One of pCons and pCol must be NULL and the other non-null.
**
** Form (1) drops a constraint the user named, whatever kind it is, so the
** editor is fixed.  Form (2) drops constraints of one kind off a named
** column, and zFunc is the editor that knows which kind: every one of them
** takes (ISCHEMA, SQL, COLNAME) and returns the edited statement.
*/
void sqlite3AlterDropConstraint(
  Parse *pParse,     /* Parsing context */
  SrcList *pSrc,     /* The table being altered */
  Token *pCons,      /* Name of the constraint to drop, or 0 */
  Token *pCol,       /* Name of the column to take constraints off, or 0 */
  const char *zFunc  /* Editor for form (2), naming which kind goes */
){
  sqlite3 *db = pParse->db;
  Table *pTab = 0;
  int iDb = 0;
  const char *zDb = 0;
  char *zArg = 0;

  assert( (pCol==0)!=(pCons==0) );
  assert( (pCol==0)==(zFunc==0) );
  assert( pSrc->nSrc==1 );
  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, pCons!=0, 2);
  if( !pTab ) return;

  if( pCons ){
    char *z = sqlite3NameFromToken(db, pCons);
    zArg = sqlite3MPrintf(db, "sqlite_drop_constraint(sql, %Q)", z);
    sqlite3DbFree(db, z);
  }else{
    int iCol;
    char *zCol;
    /* alterFindCol() is still what authorizes the change and reports an
    ** unknown column, but the index it returns is not used: the editor is
    ** given the name and resolves it against the text it is about to edit. */
    if( alterFindCol(pParse, pTab, pCol, &iCol) ) return;
    zCol = sqlite3NameFromToken(db, pCol);
    if( zCol==0 ) return;
    zArg = sqlite3MPrintf(db, "%s(%d, sql, %Q)", zFunc, iDb, zCol);
    sqlite3DbFree(db, zCol);
  }

  /* Edit the SQL for the named table. */
  sqlite3NestedParse(pParse,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = %s "
      "WHERE type='table' AND tbl_name=%Q COLLATE nocase"
      , zDb, zArg, pTab->zName
  );
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
  char *zCol = 0;
  int nCons = 0;

  /* Look up the table being altered. */
  assert( pSrc->nSrc==1 );
  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, 0, 2);
  if( !pTab ) return;

  /* Find the column being altered.  alterFindCol() authorizes the change and
  ** reports an unknown column; the index it returns is not used, because the
  ** editors resolve the name against the text they are about to edit. */
  if( alterFindCol(pParse, pTab, pCol, &iCol) ){
    return;
  }
  zCol = sqlite3NameFromToken(pParse->db, pCol);
  if( zCol==0 ) return;

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
  sqlite3NestedParse(pParse,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = sqlite_insert_constraint(%d, "
              "sqlite_drop_notnull(%d, sql, %Q), %.*Q, %Q) "
      "WHERE type='table' AND tbl_name=%Q COLLATE nocase"
      , zDb, iDb, iDb, zCol, nCons, pCons, zCol, pTab->zName
  );
  sqlite3DbFree(pParse->db, zCol);

  /* Finally, reload the database schema. */
  renameReloadSchema(pParse, iDb, INITFLAG_AlterDropCons);
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

  sqlite3NestedParse(pParse,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = sqlite_insert_constraint(%d, sql, %.*Q, NULL) "
      "WHERE type='table' AND tbl_name=%Q COLLATE nocase"
      , zDb, iDb, nCons, pCons, pTab->zName
  );

  /* Finally, reload the database schema. */
  renameReloadSchema(pParse, iDb, INITFLAG_AlterDropCons);
}

/*
** Common tail shared by every ALTER TABLE ... ADD CONSTRAINT form.
**
** Reject a name another constraint on this table already carries, splice
** zCons into the stored CREATE TABLE statement, and reload the schema.
** A zCol of 0 stores it as a table-constraint; otherwise it becomes a
** constraint on that column, named rather than numbered so that the editor
** resolves it against the stored text.
**
** The text stored is the text the user wrote.  Nothing is regenerated from
** the parse tree, so a constraint reads back the way it was typed.
*/
static void alterAddConstraintText(
  Parse *pParse,        /* Parse context */
  Table *pTab,          /* Table being altered */
  int iDb,              /* Schema holding pTab */
  const char *zDb,      /* Name of that schema */
  const char *zName,    /* Name of the new constraint, or 0 if unnamed */
  const char *zCons,    /* Text of the constraint to store */
  int nCons,            /* Bytes of zCons to use */
  const char *zCol      /* Column to attach it to, or 0 for the table */
){
  /* An unnamed constraint has no name to collide with, so there is nothing
  ** to look for.  That is the DEFAULT case: it is reached by kind rather
  ** than by name, so no name is asked for and none is stored. */
  if( zName ){
    sqlite3NestedParse(pParse,
        "SELECT sqlite_fail('constraint %q already exists', %d) "
        "FROM \"%w\"." LEGACY_SCHEMA_TABLE " "
        "WHERE type='table' AND tbl_name=%Q COLLATE nocase "
        "AND sqlite_find_constraint(sql, %Q)",
        zName, SQLITE_ERROR, zDb, pTab->zName, zName
    );
  }

  sqlite3NestedParse(pParse,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = sqlite_insert_constraint(%d, sql, %.*Q, %Q) "
      "WHERE type='table' AND tbl_name=%Q COLLATE nocase"
      , zDb, iDb, nCons, zCons, zCol, pTab->zName
  );

  renameReloadSchema(pParse, iDb, INITFLAG_AlterAddCons);
}

/*
** Count the automatic indexes on pTab - the ones a PRIMARY KEY or UNIQUE
** constraint in the table's own definition brought into being, as opposed
** to the ones CREATE INDEX made.
**
** This is how the name of the index for a new UNIQUE or PRIMARY KEY
** constraint is worked out ahead of time.  sqlite3CreateIndex() names an
** automatic index "sqlite_autoindex_<table>_<n>" where n counts the indexes
** already on the table.  When the edited statement is reparsed only the
** automatic ones exist at that point, and the new constraint is last in the
** list, so its n is this count plus one.
*/
static int alterCountAutoIndex(Table *pTab){
  Index *pIdx;
  int n = 0;
  for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
    if( pIdx->idxType!=SQLITE_IDXTYPE_APPDEF ) n++;
  }
  return n;
}

/*
** True if a PRIMARY KEY over pList would turn a column of pTab into an
** alias for the rowid, which is the one form of PRIMARY KEY that changes
** how rows are stored rather than adding an index beside them.  The test
** mirrors the one in sqlite3AddPrimaryKey().
**
** If it would, *pzCol is set to the name of that column.
*/
static int alterPkIsRowidAlias(Table *pTab, ExprList *pList, const char **pzCol){
  Expr *pExpr;
  int iCol;

  if( pTab->tabFlags & TF_WithoutRowid ) return 0;
  if( pList==0 || pList->nExpr!=1 ) return 0;
  if( pList->a[0].fg.sortFlags & KEYINFO_ORDER_DESC ) return 0;
  pExpr = sqlite3ExprSkipCollate(pList->a[0].pExpr);
  if( pExpr==0 ) return 0;
  /* A quoted key column arrives as TK_STRING; sqlite3AddPrimaryKey() has
  ** sqlite3StringToId() turn it into TK_ID before looking at it. */
  if( pExpr->op!=TK_ID && pExpr->op!=TK_STRING ) return 0;
  if( ExprHasProperty(pExpr, EP_IntValue) ) return 0;
  iCol = sqlite3ColumnIndex(pTab, pExpr->u.zToken);
  if( iCol<0 || pTab->aCol[iCol].eCType!=COLTYPE_INTEGER ) return 0;
  *pzCol = pTab->aCol[iCol].zCnName;
  return 1;
}

/*
** Implement:
**
**     ALTER TABLE <table> ADD CONSTRAINT <name> UNIQUE(...)
**     ALTER TABLE <table> ADD CONSTRAINT <name> PRIMARY KEY(...)
**     ALTER TABLE <table> ADD CONSTRAINT <name> FOREIGN KEY(...) REFERENCES ...
**
** pFirst is the CONSTRAINT keyword; the constraint runs from there to the
** end of the statement.  pList and zCols/nCols are the indexed column list,
** as a parse tree and as text; both are 0 for a FOREIGN KEY.  Ownership of
** pList passes to this routine.
**
** A UNIQUE or PRIMARY KEY constraint is not text alone: it needs the b-tree
** of an automatic index to go with it.  That is built here by a nested
** CREATE UNIQUE INDEX under the name the reparse will look for.  Building
** it is also what vets the rows already in the table - if two of them
** collide the index build fails and takes the whole statement with it.
** Afterwards the index's own sql is set to NULL, which is how the schema
** records an index that belongs to a constraint rather than to a CREATE
** INDEX of its own.
**
** A FOREIGN KEY is text alone.  Its rows are checked only when foreign keys
** are being enforced, which is the same rule that decides whether an INSERT
** would check them.
*/
void sqlite3AlterAddNamedConstraint(
  Parse *pParse,        /* Parse context */
  SrcList *pSrc,        /* Table to add the constraint to */
  Token *pFirst,        /* The CONSTRAINT keyword */
  Token *pName,         /* Name of the new constraint */
  int eType,            /* One of the ALTERCONS_* values */
  ExprList *pList,      /* Indexed columns, or 0 for a FOREIGN KEY */
  const char *zCols,    /* The same list as written, or 0 */
  int nCols             /* Bytes of zCols */
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
    /* An INTEGER PRIMARY KEY is the rowid rather than an index over it, so
    ** adding one would have to rewrite every row.  Refuse rather than
    ** quietly produce a table whose rowids do not match the column. */
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
    alterAddConstraintText(pParse, pTab, iDb, zDb, zName, zCons, nCons, 0);

    /* Emitted after the reload above, so that foreign_key_check sees the
    ** key that was just added. */
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

    /* A PRIMARY KEY on a STRICT table implies NOT NULL on every column of
    ** the key, which sqlite3EndTable() adds when the edited statement is
    ** reparsed.  Nothing rechecks the rows at that point, so check them
    ** here.  A rowid table that is not STRICT allows NULLs in a PRIMARY
    ** KEY, so there is nothing to check for it. */
    if( eType==ALTERCONS_PrimaryKey && (pTab->tabFlags & TF_Strict)!=0 ){
      sqlite3NestedParse(pParse,
          "SELECT sqlite_fail('PRIMARY KEY %q on %q would be NULL', %d) "
          "FROM \"%w\".\"%w\" WHERE (%.*s) IS NULL",
          zName, pTab->zName, SQLITE_CONSTRAINT, zDb, pTab->zName, nCols, zCols
      );
    }

    /* Build the index.  This runs before the statement text is edited, so
    ** it sees the table as it is now; the reparse afterwards finds the
    ** b-tree already in place under the name it derives for the new
    ** constraint. */
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

    alterAddConstraintText(pParse, pTab, iDb, zDb, zName, zCons, nCons, 0);
  }

add_named_cons_exit:
  sqlite3ExprListDelete(db, pList);
  sqlite3DbFree(db, zName);
}

/*
** Implement:
**
**     ALTER TABLE <table> COLUMN <column> ADD DEFAULT <value>
**
** DEFAULT is the one constraint this command understands that cannot be a
** table-constraint, so this form names the column it belongs to.  pExpr
** with zStart/zEnd is the default value as parsed and as written, the same
** pair sqlite3AddDefaultValue() is handed in a CREATE TABLE.
**
** No row is touched.  A DEFAULT says what to store when an INSERT does not
** mention the column, so rows already in the table are unaffected.
*/
void sqlite3AlterAddDefault(
  Parse *pParse,        /* Parse context */
  SrcList *pSrc,        /* Table to add the default to */
  Token *pCol,          /* Name of the column it applies to */
  Expr *pExpr,          /* The default value, as parsed */
  const char *zStart,   /* First byte of the default value text */
  const char *zEnd      /* First byte past the default value text */
){
  sqlite3 *db = pParse->db;
  Table *pTab;
  Column *pTabCol;
  int iDb = 0;
  int iCol = 0;
  const char *zDb = 0;
  char *zCons = 0;

  /* bAuth is 0 here, as in every other command that names a column:
  ** alterFindCol() below raises the SQLITE_ALTER_TABLE callback with the
  ** column name, and raising it twice for one statement would report the
  ** same change to the authorizer as two. */
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
  /* A column carries at most one DEFAULT.  The expression list is where a
  ** GENERATED expression lives too, but a generated column was rejected
  ** just above, so anything found here is a default value. */
  if( sqlite3ColumnExpr(pTab, pTabCol)!=0 ){
    sqlite3ErrorMsg(pParse, "column \"%s\" already has a default value",
                    pTabCol->zCnName);
    goto add_default_exit;
  }
#ifndef SQLITE_OMIT_AUTHORIZATION
  if( db->xAuth ) sqlite3FuncAuth(pParse, pExpr);
#endif

  /* The column is addressed to this command and is not part of what gets
  ** stored, so the text is put together from the value rather than copied
  ** whole.  There is no name: a DEFAULT is reached by kind, through
  ** ALTER TABLE ... COLUMN <c> DROP DEFAULT. */
  zCons = sqlite3MPrintf(db, "DEFAULT %.*s", (int)(zEnd - zStart), zStart);
  if( zCons==0 ) goto add_default_exit;

  alterAddConstraintText(pParse, pTab, iDb, zDb, 0, zCons,
                         sqlite3Strlen30(zCons), pTabCol->zCnName);

add_default_exit:
  sqlite3ExprDelete(db, pExpr);
  sqlite3DbFree(db, zCons);
}

/*
** Internal SQL function:
**
**     sqlite_drop_pk(ISCHEMA, SQL)
**
** SQL is a CREATE TABLE statement belonging to schema ISCHEMA.  Return a
** copy of it with the PRIMARY KEY clause removed.
**
** The clause is not searched for.  The statement is reparsed and the extent
** recorded by sqlite3ConsLocAdd() during that parse says where it is, so
** the column form ("a INT PRIMARY KEY DESC ON CONFLICT FAIL") and the table
** form ("CONSTRAINT k PRIMARY KEY(a,b)") are handled by the same code, and
** an ON CONFLICT clause or a sort order is taken in without being looked
** for.
**
** A table-constraint has a comma in front of it that has to go with it, or
** the list would be left with a hole.  Which comma - the one before or the
** one after - is decided by what follows the clause, the same way
** sqlite_drop_constraint() decides it.
*/
static void dropPkFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  int iSchema = sqlite3_value_int(argv[0]);
  const char *zSql = (const char*)sqlite3_value_text(argv[1]);
  const char *zDb;
  ParseLoc *p;
  Parse sParse;
  char *zOut = 0;
  int nOut = 0;
  int rc;
#ifndef SQLITE_OMIT_AUTHORIZATION
  sqlite3_xauth xAuth = db->xAuth;
  db->xAuth = 0;
#endif

  UNUSED_PARAMETER(NotUsed);
  if( zSql==0 || iSchema<0 || iSchema>=db->nDb ){
    rc = SQLITE_OK;
    goto drop_pk_done;
  }
  zDb = db->aDb[iSchema].zDbSName;

  rc = renameParseSql(&sParse, zDb, db, zSql, iSchema==1);
  if( rc!=SQLITE_OK ){
    rc = SQLITE_CORRUPT_BKPT;
    goto drop_pk_cleanup;
  }
  if( sParse.pNewTable==0 || !IsOrdinaryTable(sParse.pNewTable) ){
    rc = SQLITE_CORRUPT_BKPT;
    goto drop_pk_cleanup;
  }
  for(p=sParse.pLoc; p; p=p->pNext){
    if( p->eType==PARSELOC_PrimaryKey ) break;
  }
  if( p==0 ){
    /* The table has a PRIMARY KEY - the caller checked - but the stored
    ** text has no clause to remove it from. */
    rc = SQLITE_CORRUPT_BKPT;
    goto drop_pk_cleanup;
  }

  nOut = sqlite3Strlen30(zSql);
  zOut = sqlite3DbMallocRaw(db, (i64)nOut+1);
  if( zOut==0 ){
    rc = SQLITE_NOMEM_BKPT;
    goto drop_pk_cleanup;
  }
  memcpy(zOut, zSql, (size_t)nOut+1);

  nOut = alterExciseClause(zOut, nOut, zSql, &p->t);
  sqlite3_result_text(ctx, zOut, nOut, SQLITE_TRANSIENT);

drop_pk_cleanup:
  renameParseCleanup(&sParse);
  sqlite3DbFree(db, zOut);

drop_pk_done:
#ifndef SQLITE_OMIT_AUTHORIZATION
  db->xAuth = xAuth;
#endif
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
  }
}

/*
** Return the trailing number of an automatic index name, which is always
** "sqlite_autoindex_<table>_<n>".  Returns 0 if the name is not of that
** shape, which should not happen for an index the schema built itself.
*/
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

/*
** Internal SQL function:
**
**     sqlite_drop_fk(ISCHEMA, SQL, PARENT, NCHILD, <child...>, <parent...>)
**
** SQL is a CREATE TABLE statement belonging to schema ISCHEMA.  Return a
** copy of it with every FOREIGN KEY matching the given shape removed: the
** child columns are the NCHILD arguments after NCHILD, the parent table is
** PARENT, and the parent columns are whatever arguments follow the child
** ones - none of them if the key was written without a parent column list.
**
** A FOREIGN KEY need not have a name, so it is identified by what it says
** rather than by what it is called.  Two keys may say the same thing; all
** of them go.
**
** Pairing a key with its text:
**
**   Both lists are built by prepending - Table.u.tab.pFKey in
**   sqlite3CreateForeignKey() and Parse.pLoc in sqlite3ParseLocAdd() - and
**   the extent is recorded in the same statement that links the key, so the
**   two run in the same order and pair off one for one.  If they somehow do
**   not, the statement is refused rather than guessed at.
*/
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
  const char *zDb;
  Table *pTab;
  FKey *pFKey;
  ParseLoc *pLoc;
  Parse sParse;
  char *zOut = 0;
  int nOut = 0;
  int nFound = 0;
  int nKey, nRec, i;
  int rc;
#ifndef SQLITE_OMIT_AUTHORIZATION
  sqlite3_xauth xAuth = db->xAuth;
  db->xAuth = 0;
#endif

  if( zSql==0 || zTo==0 || iSchema<0 || iSchema>=db->nDb
   || nChild<=0 || argc<4+nChild
  ){
    rc = SQLITE_OK;
    goto drop_fk_done;
  }
  nParent = argc - 4 - nChild;
  zDb = db->aDb[iSchema].zDbSName;

  rc = renameParseSql(&sParse, zDb, db, zSql, iSchema==1);
  if( rc!=SQLITE_OK ){
    rc = SQLITE_CORRUPT_BKPT;
    goto drop_fk_cleanup;
  }
  pTab = sParse.pNewTable;
  if( pTab==0 || !IsOrdinaryTable(pTab) ){
    rc = SQLITE_CORRUPT_BKPT;
    goto drop_fk_cleanup;
  }

  /* The two lists must be the same length, or the pairing below is
  ** meaningless. */
  nKey = 0;
  for(pFKey=pTab->u.tab.pFKey; pFKey; pFKey=pFKey->pNextFrom) nKey++;
  nRec = 0;
  for(pLoc=sParse.pLoc; pLoc; pLoc=pLoc->pNext){
    if( pLoc->eType==PARSELOC_ForeignKey ) nRec++;
  }
  if( nKey!=nRec ){
    rc = SQLITE_CORRUPT_BKPT;
    goto drop_fk_cleanup;
  }

  nOut = sqlite3Strlen30(zSql);
  zOut = sqlite3DbMallocRaw(db, (i64)nOut+1);
  if( zOut==0 ){
    rc = SQLITE_NOMEM_BKPT;
    goto drop_fk_cleanup;
  }
  memcpy(zOut, zSql, (size_t)nOut+1);

  /* Walk the two lists together.  Both are in reverse order of appearance,
  ** so this also removes matches right to left, which keeps every extent
  ** not yet used addressing the same byte of zOut that it did in zSql. */
  pLoc = sParse.pLoc;
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
    if( bMatch ){
      if( nParent==0 ){
        /* The request named no parent columns, so the key must not either. */
        for(i=0; i<nChild; i++){
          if( pFKey->aCol[i].zCol!=0 ) bMatch = 0;
        }
      }else if( nParent!=nChild ){
        bMatch = 0;
      }else{
        for(i=0; bMatch && i<nChild; i++){
          const char *zWant = (const char*)sqlite3_value_text(argv[4+nChild+i]);
          if( pFKey->aCol[i].zCol==0 || zWant==0
           || sqlite3StrICmp(pFKey->aCol[i].zCol, zWant)!=0
          ){
            bMatch = 0;
          }
        }
      }
    }

    if( bMatch ){
      nOut = alterExciseClause(zOut, nOut, zSql, &pLoc->t);
      nFound++;
    }
    pLoc = pLoc->pNext;
  }

  if( nFound==0 ){
    errorMPrintf(ctx, "table \"%s\" has no such FOREIGN KEY", pTab->zName);
    rc = SQLITE_OK;
    goto drop_fk_cleanup;
  }
  sqlite3_result_text(ctx, zOut, nOut, SQLITE_TRANSIENT);

drop_fk_cleanup:
  renameParseCleanup(&sParse);
  sqlite3DbFree(db, zOut);

drop_fk_done:
#ifndef SQLITE_OMIT_AUTHORIZATION
  db->xAuth = xAuth;
#endif
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
  }
}

/*
** Implement "ALTER TABLE <table> DROP CONSTRAINT PRIMARY KEY".
**
** A PRIMARY KEY need not be named, so it is dropped by kind.  On a rowid
** table an ordinary PRIMARY KEY is a constraint in the text plus an
** automatic index beside the rows, and both have to go: leaving the index
** behind would make the next schema load report an orphan index.  No row is
** rewritten - the record layout of a rowid table does not depend on which
** of its columns the PRIMARY KEY names.
**
** Two shapes are refused rather than half-done:
**
**   *  A WITHOUT ROWID table keeps its rows in PRIMARY KEY order, so the
**      key is the table.  Dropping it would mean rebuilding.
**
**   *  An INTEGER PRIMARY KEY is the rowid.  Its values are not in the
**      record at all - the record holds a NULL in that slot - so a table
**      that lost the clause would read that column back as NULL for every
**      row.  Rebuilding is the only way to keep the values.
*/
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

  /* Take the clause out of the stored statement. */
  sqlite3NestedParse(pParse,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = sqlite_drop_pk(%d, sql) "
      "WHERE type='table' AND tbl_name=%Q COLLATE nocase"
      , zDb, iDb, pTab->zName
  );

  /* And take away the index that went with it. */
  sqlite3CodeDropIndex(pParse, pPk, iDb);

  /* Automatic indexes are numbered by the order their constraints appear in
  ** the CREATE TABLE, and the reparse will number them afresh.  Any that sat
  ** after the PRIMARY KEY therefore move down one, and their rows have to be
  ** renamed to match or the next schema load reports an orphan index.
  ** Ascending order, so that each name is free by the time it is taken. */
  {
    int n = alterAutoIndexNumber(pPk->zName);
    while( n>0 ){
      char *zOld = sqlite3MPrintf(db, "sqlite_autoindex_%s_%d", pTab->zName,n+1);
      char *zNew = sqlite3MPrintf(db, "sqlite_autoindex_%s_%d", pTab->zName, n);
      if( zOld==0 || zNew==0 ){
        sqlite3DbFree(db, zOld);
        sqlite3DbFree(db, zNew);
        break;
      }
      if( sqlite3FindIndex(db, zOld, zDb)==0 ){
        sqlite3DbFree(db, zOld);
        sqlite3DbFree(db, zNew);
        break;
      }
      sqlite3NestedParse(pParse,
          "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET name=%Q "
          "WHERE type='index' AND name=%Q", zDb, zNew, zOld
      );
      sqlite3DbFree(db, zOld);
      sqlite3DbFree(db, zNew);
      n++;
    }
  }

  renameReloadSchema(pParse, iDb, INITFLAG_AlterDropCons);
}

/*
** Internal SQL function:
**
**     sqlite_set_strict(SQL, BSEP)
**
** SQL is a CREATE TABLE statement.  Return a copy of it with the STRICT
** table-option appended.  BSEP is true if the statement already carries a
** table-option list (in practice, WITHOUT ROWID) and the new option has to
** be introduced with a comma rather than with a space.
**
** The caller knows whether a separator is needed because it read
** TF_WithoutRowid off the Table object.  Nothing here has to go looking
** for it in the text.
**
** The only text work is deciding where the statement really ends.  The
** stored SQL can carry trailing whitespace, and when the CREATE TABLE was
** followed by a semicolon it can carry a trailing comment as well.
** Appending after a "--" comment would bury the new option inside it, so
** alterRtrimConstraint() is used to step back to the end of the last real
** token.  It keeps block comments, which are terminated and so are safe to
** append after.
*/
static void setStrictFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const char *zSql = (const char*)sqlite3_value_text(argv[0]);
  int bSep = sqlite3_value_int(argv[1]);
  int nSql;
  char *zNew;

  UNUSED_PARAMETER(NotUsed);
  if( zSql==0 ) return;

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

/*
** Internal SQL function:
**
**     sqlite_unset_strict(ISCHEMA, SQL)
**
** SQL is a CREATE TABLE statement belonging to schema ISCHEMA that carries
** the STRICT table-option.  Return a copy of it with STRICT removed.
**
** The option list is not searched for STRICT and edited in place.  It is
** regenerated: everything from the ")" that closes the column list to the
** end of the statement is replaced by the list the table should have once
** STRICT is gone, which is " WITHOUT ROWID" or nothing at all.
**
** That keeps this indifferent to how the list was written - "STRICT",
** "WITHOUT ROWID, STRICT", "STRICT , without rowid", a comment sitting
** between the two - none of which a scan for the STRICT keyword would
** handle without also working out which comma belongs to it.  The parser
** already knows both facts this needs: where the list starts (just past
** Parse.sColListEnd, recorded during the reparse) and which options the
** table ends up with (TF_WithoutRowid).
**
** A comment written inside the option list is dropped along with the rest
** of the list.  Comments anywhere else in the statement are untouched.
*/
static void unsetStrictFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  int iSchema = sqlite3_value_int(argv[0]);
  const char *zSql = (const char*)sqlite3_value_text(argv[1]);
  const char *zDb;
  Table *pTab;
  Parse sParse;
  char *zNew;
  int nKeep;
  int rc;
#ifndef SQLITE_OMIT_AUTHORIZATION
  sqlite3_xauth xAuth = db->xAuth;
  db->xAuth = 0;
#endif

  UNUSED_PARAMETER(NotUsed);
  if( zSql==0 || iSchema<0 || iSchema>=db->nDb ){
    rc = SQLITE_OK;
    goto unset_strict_done;
  }
  zDb = db->aDb[iSchema].zDbSName;

  rc = renameParseSql(&sParse, zDb, db, zSql, iSchema==1);
  if( rc!=SQLITE_OK ) goto unset_strict_cleanup;
  pTab = sParse.pNewTable;
  if( pTab==0 || !IsOrdinaryTable(pTab) || sParse.sColListEnd.z==0 ){
    /* This can happen if the sqlite_schema table is corrupt */
    rc = SQLITE_CORRUPT_BKPT;
    goto unset_strict_cleanup;
  }

  nKeep = (int)(&sParse.sColListEnd.z[sParse.sColListEnd.n] - zSql);
  assert( nKeep>0 && nKeep<=sqlite3Strlen30(zSql) );
  zNew = sqlite3MPrintf(db, "%.*s%s", nKeep, zSql,
      (pTab->tabFlags & TF_WithoutRowid)!=0 ? " WITHOUT ROWID" : ""
  );
  if( zNew==0 ){
    rc = SQLITE_NOMEM_BKPT;
    goto unset_strict_cleanup;
  }
  sqlite3_result_text(ctx, zNew, -1, SQLITE_TRANSIENT);
  sqlite3DbFree(db, zNew);

unset_strict_cleanup:
  renameParseCleanup(&sParse);

unset_strict_done:
#ifndef SQLITE_OMIT_AUTHORIZATION
  db->xAuth = xAuth;
#endif
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
  }
}

/*
** Map the name of a table-option to the TF_ flag that represents it, or
** return 0 if the name is not one this command understands.
**
** bWithout says the name was preceded by the WITHOUT keyword, which is how
** a CREATE TABLE spells that option and therefore how this one does too.
*/
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

/*
** The name the replacement table is built under while a rebuild is in
** progress.  Derived from the table name so that both phases of the
** rebuild agree on it without having to pass it between them.  Caller
** frees the result.
*/
static char *alterRebuildName(sqlite3 *db, const char *zTab){
  return sqlite3MPrintf(db, "altertab_%s", zTab);
}

/*
** Prepare and run one statement of dynamically built SQL.  Rows returned
** are discarded.  On error, leave a message in *pzErrMsg.
*/
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
static int alterExecSqlF(sqlite3 *db, char **pzErrMsg, const char *zFmt, ...){
  char *z;
  va_list ap;
  int rc;
  va_start(ap, zFmt);
  z = sqlite3VMPrintf(db, zFmt, ap);
  va_end(ap);
  rc = alterExecSql(db, pzErrMsg, z);
  sqlite3DbFree(db, z);
  return rc;
}

/*
** Run one SELECT that is expected to return a single text value, and
** return a copy of it.  Returns NULL if there is no row.
**
** Takes ownership of zSql, which is freed before returning.
*/
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

/*
** Return a copy of the CREATE statement zSql with the name of the object
** it creates qualified with schema zDb.  The caller frees the result.
**
** Nothing else can steer a nested CREATE into a schema other than "main".
** sqlite3RunVacuum() gets away with setting db->init.iDb because VACUUM
** also sets DBFLAG_Vacuum, which is what lets sqlite3TwoPartName() accept
** that state; a rebuild has no such licence, so it names the schema in the
** statement text instead.
**
** The text held in sqlite_schema is always a plain unqualified CREATE:
** sqlite3EndTable(), sqlite3CreateIndex() and sqlite3BeginTrigger() each
** normalise away both the TEMP keyword and any schema prefix the user
** wrote.  So the name is simply the first token after TABLE, INDEX or
** TRIGGER, skipping an IF NOT EXISTS if a complete one is present.  A
** partial match is a name that happens to be spelled "if".
*/
static char *alterQualifyDdl(sqlite3 *db, const char *zDb, const char *zSql){
  static const u8 aPhrase[] = { TK_IF, TK_NOT, TK_EXISTS };
  const unsigned char *z = (const unsigned char*)zSql;
  int i = 0;            /* Offset of the token being looked at */
  int iName = -1;       /* Offset the schema prefix is inserted in front of */
  int iIf = -1;         /* Offset of a partially matched IF NOT EXISTS */
  int nMatch = 0;       /* 0 before TABLE/INDEX/TRIGGER, then 1 + phrase */

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

/*
** Run zSql, which is expected to return one column of CREATE statements,
** and append each one to the NULL-terminated array at *pazRedo, qualified
** with schema zSchema.  *pnRedo is the number of entries already there and
** is advanced past the ones added.
**
** Takes ownership of zSql, which is freed before returning.
*/
static int alterCollectDdl(
  sqlite3 *db,          /* Database connection */
  char ***pazRedo,      /* IN/OUT: the array being built */
  int *pnRedo,          /* IN/OUT: number of entries in it */
  const char *zSchema,  /* Schema the statements are to be run against */
  char *zSql            /* The query to run */
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

/*
** Return a copy of CREATE TABLE statement zSql with its table-option list
** replaced by the one implied by tabFlags, and its table name replaced by
** zNewName.  The caller frees the result.
**
** Neither edit is a search.  The option list is rebuilt from what follows
** Parse.sColListEnd, on the same reasoning as sqlite_unset_strict().  The name
** is located through its RenameToken, which is what sqlite_rename_table()
** uses, so a quoted or awkwardly spelled name needs no special handling.
*/
static char *alterRewriteCreate(
  sqlite3 *db,          /* Database connection */
  int iDb,              /* Schema that zSql belongs to */
  const char *zSql,     /* The CREATE TABLE statement to rewrite */
  u32 tabFlags,         /* TF_WithoutRowid and/or TF_Strict, or 0 */
  const char *zNewName  /* Rename the table to this, or NULL to keep it */
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
          iName, zSql,                                   /* up to the name */
          zNewName,                                      /* the new name */
          nKeep - iName - (int)pName->t.n,               /* rest of the body */
          &zSql[iName + pName->t.n],
          zWr, zSep, zSt);
    }
  }
  renameParseCleanup(&sParse);
  return zNew;
}

/*
** Return a copy of CREATE TABLE statement zSql with the declared type of
** column zCol replaced by zType.  The caller frees it.  Returns 0 on
** failure, having set *pzErr when the reason is worth reporting.
**
** This is only the edit.  Whether the edit is a safe thing to do on its
** own is setColTypeFunc()'s question; when it is not, the caller is
** rebuilding the table and the answer does not arise.
**
** The column is resolved against zSql, and the extent of the type it
** currently declares comes from what the parser recorded during the
** reparse, so a column written without a type needs no special case: its
** extent is empty and sits where a type would go.
*/
static char *alterRetypeText(
  sqlite3 *db,          /* Database connection */
  int iDb,              /* Schema that zSql belongs to */
  const char *zSql,     /* The CREATE TABLE statement to rewrite */
  const char *zCol,     /* Column to retype */
  const char *zType,    /* Its new declared type */
  char **pzErr          /* OUT: error message, if any */
){
  Parse sParse;
  Table *pTab;
  ParseLoc *p;
  char *zNew = 0;
  int iCol, iStart, iEnd;

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

  iStart = (int)(p->t.z - zSql);
  iEnd = iStart + (int)p->t.n;
  assert( iStart>=0 && iEnd<=sqlite3Strlen30(zSql) );
  /* A column that had no type needs a space in front of the new one. */
  zNew = sqlite3MPrintf(db, "%.*s%s%s%s", iStart, zSql,
                        p->t.n==0 ? " " : "", zType, &zSql[iEnd]);

retype_out:
  renameParseCleanup(&sParse);
  return zNew;
}

/*
** What a table is to be rebuilt as.  A rebuild is the same work whatever
** provoked it - build the replacement beside the original, copy the rows,
** drop the original, put the name and the dependent objects back - and the
** only thing that varies is how the replacement's CREATE TABLE is derived
** from the original's.  This says that, and nothing else.
**
** Handed to OP_AlterTabOpt as P4 in a single allocation, the strings living
** in the tail, so that P4_DYNAMIC frees the whole thing.
*/
struct AlterRebuild {
  const char *zTab;     /* The table being rebuilt */
  const char *zCol;     /* SET TYPE: the column to retype, else 0 */
  const char *zType;    /* SET TYPE: its new declared type, else 0 */
  u8 eWrOp;             /* WITHOUT ROWID: 0 leave alone, 1 add, 2 remove */
};

/*
** Build one.  Returns 0 on OOM.
*/
static AlterRebuild *alterRebuildNew(
  sqlite3 *db,
  const char *zTab,
  const char *zCol,
  const char *zType,
  u8 eWrOp
){
  AlterRebuild *p;
  i64 nTab = zTab ? sqlite3Strlen30(zTab)+1 : 0;
  i64 nCol = zCol ? sqlite3Strlen30(zCol)+1 : 0;
  i64 nType = zType ? sqlite3Strlen30(zType)+1 : 0;
  char *z;

  p = sqlite3DbMallocZero(db, sizeof(*p) + nTab + nCol + nType);
  if( p==0 ) return 0;
  z = (char*)&p[1];
  p->eWrOp = eWrOp;
  if( zTab ){ memcpy(z, zTab, nTab); p->zTab = z; z += nTab; }
  if( zCol ){ memcpy(z, zCol, nCol); p->zCol = z; z += nCol; }
  if( zType ){ memcpy(z, zType, nType); p->zType = z; }
  return p;
}

/*
** Implement the OP_AlterTabOpt opcode.  See the comment on that opcode for
** how the work is split, and alterSetWithoutRowid() for what the two
** phases are separated by.
*/
int sqlite3RunAlterTabOpt(
  char **pzErrMsg,          /* OUT: error message */
  sqlite3 *db,              /* Database connection */
  int iDb,                  /* Schema holding the table */
  const AlterRebuild *pReb, /* What to rebuild the table as */
  int iPhase                /* 1 before the DROP, 2 after it */
){
  const char *zTab = pReb->zTab;
  const char *zDb;
  Table *pTab;
  char *zTmp = 0;
  char *zCols = 0;
  char *zOldSql = 0;
  char *zNewSql = 0;
  char **azRedo = 0;    /* DDL of each index and trigger on the table */
  int nRedo = 0;
  u64 savedFlags;
  int rc = SQLITE_OK;
  int i;

  assert( iDb>=0 && iDb<db->nDb );
  assert( iPhase==1 || iPhase==2 );
  zDb = db->aDb[iDb].zDbSName;
  zTmp = alterRebuildName(db, zTab);
  if( zTmp==0 ) return SQLITE_NOMEM_BKPT;

  if( iPhase==2 ){
    /* The original is gone.  Give the replacement its name and put the
    ** indexes and triggers back.  SQLITE_LegacyAlter keeps the rename from
    ** rewriting references in other objects: those already name the table
    ** correctly, since the name is being restored rather than changed. */
    savedFlags = db->flags;
    db->flags |= SQLITE_LegacyAlter;
    rc = alterExecSqlF(db, pzErrMsg, "ALTER TABLE \"%w\".\"%w\" RENAME TO \"%w\"",
                       zDb, zTmp, zTab);
    db->flags = savedFlags;

    if( db->pAlterRedo ){
      char **az = db->pAlterRedo;

      /* az[0] is the statement the table should be stored under.  The
      ** rename above wrote a correct but requoted version of it; restore
      ** the intended spelling so that turning the option back off gives
      ** the text the table started with. */
      if( rc==SQLITE_OK && az[0] ){
        /* Writing sqlite_schema directly needs writable_schema, which in
        ** turn is ignored while defensive mode is on. */
        u64 f = db->flags;
        db->flags |= SQLITE_WriteSchema;
        db->flags &= ~(u64)SQLITE_Defensive;
        rc = alterExecSqlF(db, pzErrMsg,
            "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET sql=%Q"
            " WHERE type='table' AND name=%Q COLLATE nocase",
            zDb, az[0], zTab);
        db->flags = f;
      }
      /* Already schema-qualified by alterCollectDdl(). */
      for(i=1; rc==SQLITE_OK && az[i]; i++){
        rc = alterExecSql(db, pzErrMsg, az[i]);
      }
      for(i=0; az[i]; i++) sqlite3DbFree(db, az[i]);
      sqlite3DbFree(db, az);
      db->pAlterRedo = 0;
    }
    sqlite3DbFree(db, zTmp);
    return rc;
  }

  /* Phase 1.  Discard any hand-off left behind by a run that failed
  ** between the two phases. */
  if( db->pAlterRedo ){
    char **az = db->pAlterRedo;
    for(i=0; az[i]; i++) sqlite3DbFree(db, az[i]);
    sqlite3DbFree(db, az);
    db->pAlterRedo = 0;
  }
  pTab = sqlite3FindTable(db, zTab, zDb);
  if( pTab==0 || !IsOrdinaryTable(pTab) ){
    rc = SQLITE_CORRUPT_BKPT;
    goto alter_tabopt_out;
  }

  /* The columns to carry across.  Generated columns are computed by the
  ** new table and must not be copied. */
  for(i=0; i<pTab->nCol; i++){
    if( pTab->aCol[i].colFlags & COLFLAG_GENERATED ) continue;
    zCols = sqlite3MPrintf(db, "%z%s\"%w\"", zCols, zCols?",":"",
                           pTab->aCol[i].zCnName);
    if( zCols==0 ){ rc = SQLITE_NOMEM_BKPT; goto alter_tabopt_out; }
  }
  if( zCols==0 ){ rc = SQLITE_CORRUPT_BKPT; goto alter_tabopt_out; }

  /* The stored CREATE TABLE text, and the DDL of every index and trigger
  ** on this table.  The DROP between the two phases takes those objects
  ** with it, so their text has to be captured now.  Automatic indexes have
  ** a NULL sql and are skipped: the new table makes its own. */
  zOldSql = alterQueryText(db, &rc, sqlite3MPrintf(db,
      "SELECT sql FROM \"%w\"." LEGACY_SCHEMA_TABLE
      " WHERE type='table' AND name=%Q COLLATE nocase", zDb, zTab));
  if( rc!=SQLITE_OK ) goto alter_tabopt_out;
  if( zOldSql==0 ){ rc = SQLITE_CORRUPT_BKPT; goto alter_tabopt_out; }

  /* Slot 0 of the hand-off carries the statement the table should end up
  ** stored under; the rest carry the DDL to replay. */
  azRedo = sqlite3DbMallocZero(db, 2*sizeof(char*));
  if( azRedo==0 ){ rc = SQLITE_NOMEM_BKPT; goto alter_tabopt_out; }
  nRedo = 1;

  rc = alterCollectDdl(db, &azRedo, &nRedo, zDb, sqlite3MPrintf(db,
      "SELECT sql FROM \"%w\"." LEGACY_SCHEMA_TABLE
      " WHERE tbl_name=%Q COLLATE nocase AND sql IS NOT NULL"
      " AND type IN ('index','trigger')", zDb, zTab));
  if( rc!=SQLITE_OK ) goto alter_tabopt_err;

  /* A TEMP trigger on a table in another schema is recorded in
  ** temp.sqlite_schema, so the query above does not see it, but the DROP
  ** does take it down.  Collect those too - unless TEMP has a table of its
  ** own by this name, in which case they belong to that one and the DROP
  ** will leave them alone.  An index cannot be in a different schema from
  ** its table, so only triggers can turn up here. */
  if( iDb!=1 && sqlite3FindTable(db, zTab, db->aDb[1].zDbSName)==0 ){
    rc = alterCollectDdl(db, &azRedo, &nRedo, db->aDb[1].zDbSName,
      sqlite3MPrintf(db,
        "SELECT sql FROM \"%w\"." LEGACY_SCHEMA_TABLE
        " WHERE tbl_name=%Q COLLATE nocase AND sql IS NOT NULL"
        " AND type='trigger'", db->aDb[1].zDbSName, zTab));
    if( rc!=SQLITE_OK ) goto alter_tabopt_err;
  }

  /* Derive the replacement's definition.  The table options are rebuilt
  ** from flags either way: the one being changed, if one is, and otherwise
  ** the ones the table already carries. */
  {
    u32 flags;
    char *zBase = zOldSql;
    char *zRetyped = 0;
    char *zTmpSql;

    if( pReb->eWrOp ){
      flags = (pReb->eWrOp==1 ? TF_WithoutRowid : 0)
            | (pTab->tabFlags & TF_Strict);
    }else{
      flags = pTab->tabFlags & (TF_WithoutRowid|TF_Strict);
    }
    if( pReb->zCol ){
      zRetyped = alterRetypeText(db, iDb, zOldSql, pReb->zCol, pReb->zType,
                                 pzErrMsg);
      if( zRetyped==0 ){
        rc = *pzErrMsg ? SQLITE_ERROR : SQLITE_NOMEM_BKPT;
        goto alter_tabopt_out;
      }
      zBase = zRetyped;
    }

    zTmpSql = alterRewriteCreate(db, iDb, zBase, flags, zTmp);
    if( zTmpSql ){
      zNewSql = alterQualifyDdl(db, zDb, zTmpSql);
      sqlite3DbFree(db, zTmpSql);
    }
    if( zNewSql ) azRedo[0] = alterRewriteCreate(db, iDb, zBase, flags, 0);
    sqlite3DbFree(db, zRetyped);
    if( azRedo[0]==0 ){ rc = SQLITE_NOMEM_BKPT; goto alter_tabopt_out; }
  }

  rc = alterExecSql(db, pzErrMsg, zNewSql);

  if( rc==SQLITE_OK ){
    rc = alterExecSqlF(db, pzErrMsg,
        "INSERT INTO \"%w\".\"%w\"(%s) SELECT %s FROM \"%w\".\"%w\"",
        zDb, zTmp, zCols, zCols, zDb, zTab);
  }

  /* Hand the saved DDL to phase 2, which runs after the DROP. */
  if( rc==SQLITE_OK ){
    db->pAlterRedo = azRedo;
    azRedo = 0;
    nRedo = 0;
  }
  goto alter_tabopt_out;

alter_tabopt_err:
  if( *pzErrMsg==0 ) sqlite3SetString(pzErrMsg, db, sqlite3_errmsg(db));

alter_tabopt_out:
  for(i=0; i<nRedo; i++) sqlite3DbFree(db, azRedo[i]);
  sqlite3DbFree(db, azRedo);
  sqlite3DbFree(db, zOldSql);
  sqlite3DbFree(db, zNewSql);
  sqlite3DbFree(db, zCols);
  sqlite3DbFree(db, zTmp);
  return rc;
}

/*
** Emit the nested SQL that looks for a row the table's new definition
** rejects.  Run after the schema has been reloaded, so quick_check sees
** the table as it now is.  Anything it reports aborts the statement, which
** rolls the schema edit back with it.
**
** zOpt names the option in the error message and bOn says which way it was
** moved, so the message reads "cannot set STRICT on t1: ...".
*/
static void alterCheckExistingRows(
  Parse *pParse,        /* Parsing context */
  Table *pTab,          /* The table that was altered */
  const char *zDb,      /* Schema holding pTab */
  const char *zOpt,     /* Name of the option, for the error message */
  int bOn               /* True if the option was turned on */
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

/*
** Implement "ALTER TABLE pTab SET STRICT ON|OFF".
**
** Turning STRICT on has to hold up against three things:
**
**   (1) Every column must be declared with one of the standard datatypes.
**       Checked here, before anything is written, so that the statement
**       fails with the same message CREATE TABLE would have given.
**
**   (2) Every value already stored must match its column's declared type.
**
**   (3) Every column of a non-INTEGER PRIMARY KEY acquires an implied NOT
**       NULL, so no such column may already hold a NULL.
**
** Turning it off is not simply the reverse.  It relaxes (1) and (3), but
** it can still leave rows the new definition rejects, because an ANY
** column has BLOB affinity while the table is strict and NUMERIC affinity
** once it is not.  Text written into such a column while strict - '123',
** say - is stored as text, and a NUMERIC column holding text that
** converts losslessly to a number is precisely what quick_check reports
** as "TEXT value in ...".  So the same check runs in both directions,
** through alterCheckExistingRows().
**
** No row data is rewritten in either direction.  STRICT constrains what
** may be written from here on; it does not change how existing rows are
** stored.
*/
static void alterSetStrict(
  Parse *pParse,        /* Parsing context */
  Table *pTab,          /* The table being altered */
  int iDb,              /* Index of the schema holding pTab */
  const char *zDb,      /* Name of that schema */
  int bOn               /* True to turn STRICT on */
){
  int ii;

  assert( IsOrdinaryTable(pTab) );
  if( bOn ){
    /* Reject custom datatypes up front, before anything is written, so
    ** that the message matches the one CREATE TABLE would have given. */
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

  /* Edit the SQL for the named table. */
  if( bOn ){
    sqlite3NestedParse(pParse,
        "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
        "sql = sqlite_set_strict(sql, %d) "
        "WHERE type='table' AND name=%Q COLLATE nocase"
        , zDb, (pTab->tabFlags & TF_WithoutRowid)!=0, pTab->zName
    );
  }else{
    sqlite3NestedParse(pParse,
        "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
        "sql = sqlite_unset_strict(%d, sql) "
        "WHERE type='table' AND name=%Q COLLATE nocase"
        , zDb, iDb, pTab->zName
    );
  }

  renameReloadSchema(pParse, iDb, INITFLAG_AlterSetOpt);
  alterCheckExistingRows(pParse, pTab, zDb, "STRICT", bOn);
}

/*
** Generate the three steps of a table rebuild.
**
** The work is the same whatever provoked it, so it lives here rather than
** in each caller: build the replacement beside the original and copy the
** rows in, drop the original, then give the replacement the original's
** name and put its indexes and triggers back.  What the replacement is to
** be is described by the zCol/zType/eWrOp arguments, which are handed
** through to the opcode and interpreted there against the stored text.
**
** Only the middle step may destroy a b-tree, and it has to be generated
** here rather than run from inside the opcode: OP_Destroy refuses while
** another statement is reading, and the statement that invokes an opcode
** is itself one.  Generated into this statement, the reader count is one
** and the drop is allowed, exactly as for a plain DROP TABLE.
**
** The DROP fires foreign key actions on any child row pointing at the
** table, and PRAGMA foreign_keys cannot be turned off inside a
** transaction, so that case is refused up front.  This is the same reason
** the published twelve-step rebuild starts by disabling them.
*/
static void alterCodeRebuild(
  Parse *pParse,        /* Parsing context */
  Table *pTab,          /* The table to rebuild */
  int iDb,              /* Schema holding it */
  const char *zCol,     /* Column to retype, or 0 */
  const char *zType,    /* Its new declared type, or 0 */
  u8 eWrOp              /* 0 leave WITHOUT ROWID alone, 1 add, 2 remove */
){
  sqlite3 *db = pParse->db;
  const char *zDb = db->aDb[iDb].zDbSName;
  AlterRebuild *pReb;
  SrcList *pDrop;
  Token tSchema, tName;
  char *zTmp;
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

  /* Both phases derive the same name for the replacement, so it is not
  ** passed between them.  It must therefore be free. */
  zTmp = alterRebuildName(db, pTab->zName);
  if( zTmp==0 ) return;
  if( sqlite3FindTable(db, zTmp, zDb)!=0 ){
    sqlite3ErrorMsg(pParse, "cannot rebuild %s: table %s is in the way",
                    pTab->zName, zTmp);
    sqlite3DbFree(db, zTmp);
    return;
  }
  sqlite3DbFree(db, zTmp);

  v = sqlite3GetVdbe(pParse);
  if( v==0 ) return;
  sqlite3MayAbort(pParse);

  pReb = alterRebuildNew(db, pTab->zName, zCol, zType, eWrOp);
  if( pReb==0 ) return;
  sqlite3VdbeAddOp4(v, OP_AlterTabOpt, iDb, 0, 1, (char*)pReb, P4_DYNAMIC);

  sqlite3TokenInit(&tSchema, (char*)zDb);
  sqlite3TokenInit(&tName, pTab->zName);
  pDrop = sqlite3SrcListAppend(pParse, 0, &tSchema, &tName);
  if( pDrop==0 ) return;
  sqlite3DropTable(pParse, pDrop, 0, 0);

  pReb = alterRebuildNew(db, pTab->zName, zCol, zType, eWrOp);
  if( pReb==0 ) return;
  sqlite3VdbeAddOp4(v, OP_AlterTabOpt, iDb, 0, 2, (char*)pReb, P4_DYNAMIC);
}

/*
** Implement "ALTER TABLE pTab SET WITHOUT ROWID ON|OFF".
**
** A rowid table and a WITHOUT ROWID table are different on disk, so unlike
** STRICT this cannot be a schema-text edit.  The table has to be built
** anew, and the work is split into three pieces because only one of them
** is allowed to destroy a b-tree.
**
**   1. OP_AlterTabOpt phase 1 creates the replacement beside the original,
**      under a derived name, from the stored CREATE TABLE text with the
**      option list and the name rewritten.  Then it copies the rows in.
**
**   2. Ordinary generated DROP TABLE code removes the original.  This has
**      to be generated here rather than run from inside the opcode:
**      OP_Destroy refuses while another statement is reading, and the
**      statement that invokes an opcode is itself one.  Generated into
**      this statement, the reader count is one and the drop is allowed -
**      exactly as for a plain DROP TABLE.
**
**   3. OP_AlterTabOpt phase 2 renames the replacement into place and
**      rebuilds the indexes and triggers that went with the original.
**
** The DROP fires foreign key actions on any child row pointing at the
** table, and PRAGMA foreign_keys cannot be turned off inside a
** transaction, so that case is refused up front instead.  This is the same
** reason the published twelve-step rebuild starts by disabling them.
*/
static void alterSetWithoutRowid(
  Parse *pParse,        /* Parsing context */
  Table *pTab,          /* The table being altered */
  int iDb,              /* Index of the schema holding pTab */
  int bOn               /* True to turn WITHOUT ROWID on */
){
  sqlite3 *db = pParse->db;

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

/*
** Generate bytecode to implement:
**
**    ALTER TABLE pSrc SET <table-option> ON|OFF
**
** Validate the table and the option name, short-circuit when the option
** already has the requested value, and hand off to the routine that knows
** how to move that particular option.
*/
void sqlite3AlterSetTableOption(
  Parse *pParse,    /* Parsing context */
  SrcList *pSrc,    /* The table being altered */
  Token *pOpt,      /* Name of the table-option being set */
  int bOn,          /* True to turn it on, false to turn it off */
  int bWithout      /* True if WITHOUT preceded the name */
){
  Table *pTab = 0;
  int iDb = 0;
  const char *zDb = 0;
  u32 optFlag;

  assert( pSrc->nSrc==1 );

  /* The grammar has already reported a right-hand side that is neither ON
  ** nor OFF.  Return before looking the table up, so that a second and
  ** less specific message does not replace that one. */
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

  /* Setting the option to what it already is changes nothing. */
  if( ((pTab->tabFlags & optFlag)!=0)==(bOn!=0) ) return;

  if( optFlag==TF_Strict ){
    alterSetStrict(pParse, pTab, iDb, zDb, bOn);
  }else{
    assert( optFlag==TF_WithoutRowid );
    alterSetWithoutRowid(pParse, pTab, iDb, bOn);
  }
}

/*
** Implement "ALTER TABLE <table> DROP FOREIGN KEY(<cols>)
**            REFERENCES <table>(<cols>)".
**
** A FOREIGN KEY need not have a name, so it is dropped by what it says.
** The shape is handed to the editor as it was written and matched against
** the stored statement there; nothing is resolved against the in-memory
** schema, which is only asked whether the table can be altered at all.
**
** Dropping a foreign key can only relax the table, never break it, so no
** row is examined and nothing is rebuilt.  The parent's own key and any
** index behind it belong to the parent table and are left alone.
*/
void sqlite3AlterDropForeignKey(
  Parse *pParse,        /* Parsing context */
  SrcList *pSrc,        /* The table being altered */
  ExprList *pFromCol,   /* Columns of this table named by the key */
  Token *pTo,           /* The parent table */
  ExprList *pToCol      /* Columns of the parent, or 0 if none were given */
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

  /* Build the call one name at a time rather than packing the two lists
  ** into strings: a column name can contain anything a quoted identifier
  ** can, so there is no separator that would not need escaping. */
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

  sqlite3NestedParse(pParse,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = %s "
      "WHERE type='table' AND tbl_name=%Q COLLATE nocase"
      , zDb, zArg, pTab->zName
  );

  renameReloadSchema(pParse, iDb, INITFLAG_AlterDropCons);

drop_fk_exit:
  sqlite3DbFree(db, zArg);
  sqlite3DbFree(db, zTo);
  sqlite3ExprListDelete(db, pFromCol);
  sqlite3ExprListDelete(db, pToCol);
}

/*
** Implement "ALTER TABLE <table> DROP CHECK", the form that names no
** column.  The CHECKs written at table level go; those written inside a
** column definition are the other form's business and are left alone.
**
** That form goes through sqlite3AlterDropConstraint() like the other
** per-column editors; only the target differs.
*/
void sqlite3AlterDropCheck(Parse *pParse, SrcList *pSrc){
  Table *pTab;
  int iDb = 0;
  const char *zDb = 0;

  assert( pSrc->nSrc==1 );
  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, 1, 2);
  if( pTab==0 ) return;

  sqlite3NestedParse(pParse,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = sqlite_drop_check(%d, sql, NULL) "
      "WHERE type='table' AND tbl_name=%Q COLLATE nocase"
      , zDb, iDb, pTab->zName
  );

  renameReloadSchema(pParse, iDb, INITFLAG_AlterDropCons);
}

/*
** Internal SQL function:
**
**     sqlite_set_coltype(ISCHEMA, SQL, COLNAME, TYPENAME)
**
** SQL is a CREATE TABLE statement belonging to schema ISCHEMA.  Return a
** copy of it with the declared type of column COLNAME replaced by
** TYPENAME.  The column is resolved against SQL itself, and the extent of
** the type it currently declares comes from what the parser recorded
** during the reparse, so a column written without a type is handled by the
** same code: its extent is empty and sits where a type would go.
**
** The change is refused unless it leaves the column's affinity alone.
**
** That is not caution, it is the difference between an edit and a rebuild.
** A declared type is not just documentation: it fixes the column's
** affinity, and affinity is applied when a value is written.  The rows
** already in the table, and the keys already in every index over the
** column, were written under the old one.  Changing it makes the file
** disagree with its own schema:
**
**   *  PRAGMA integrity_check reports "TEXT value in t.a" once a TEXT
**      column with an index is redeclared INTEGER;
**   *  a lookup through that index stops finding the rows, because the
**      key the query computes is no longer the key that was stored;
**   *  a WITHOUT ROWID table fails integrity_check the same way, its rows
**      being held in the index that its PRIMARY KEY defines.
**
** Putting that right means rewriting every row and rebuilding every index
** - a table rebuild, which is out of reach of a text edit.  So the cases
** that need one are refused rather than half-done, and what is left is
** the change that only ever affected the declaration: one that keeps the
** affinity, such as VARCHAR(20) to TEXT or INT to INTEGER.
**
** One such change is still refused.  Exactly the word INTEGER, on the
** PRIMARY KEY of a rowid table, makes the column an alias for the rowid;
** INT does not, though the two have the same affinity.  Crossing that
** line either way changes where the values live: away from INTEGER they
** would be read back as NULL, having never been in the record at all,
** and towards it the table's automatic index becomes an orphan and the
** schema will not load.
*/
static void setColTypeFunc(
  sqlite3_context *ctx,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  int iSchema = sqlite3_value_int(argv[0]);
  const char *zSql = (const char*)sqlite3_value_text(argv[1]);
  const char *zCol = (const char*)sqlite3_value_text(argv[2]);
  const char *zType = (const char*)sqlite3_value_text(argv[3]);
  const char *zDb;
  Table *pTab;
  ParseLoc *p;
  Parse sParse;
  char *zNew = 0;
  char aOld, aNew;
  int iCol;
  int bInPk = 0;
  int rc;
#ifndef SQLITE_OMIT_AUTHORIZATION
  sqlite3_xauth xAuth = db->xAuth;
  db->xAuth = 0;
#endif

  UNUSED_PARAMETER(NotUsed);
  if( zSql==0 || zCol==0 || zType==0 || iSchema<0 || iSchema>=db->nDb ){
    rc = SQLITE_OK;
    goto set_coltype_done;
  }
  zDb = db->aDb[iSchema].zDbSName;

  rc = renameParseSql(&sParse, zDb, db, zSql, iSchema==1);
  if( rc!=SQLITE_OK ){
    rc = SQLITE_CORRUPT_BKPT;
    goto set_coltype_cleanup;
  }
  pTab = sParse.pNewTable;
  if( pTab==0 || !IsOrdinaryTable(pTab) ){
    rc = SQLITE_CORRUPT_BKPT;
    goto set_coltype_cleanup;
  }
  iCol = alterColumnIndex(pTab, zCol);
  if( iCol<0 ){
    errorMPrintf(ctx, "no such column: %s", zCol);
    rc = SQLITE_OK;
    goto set_coltype_cleanup;
  }
  for(p=sParse.pLoc; p; p=p->pNext){
    if( p->eType==PARSELOC_ColType && p->iCol==iCol ) break;
  }
  if( p==0 ){
    rc = SQLITE_CORRUPT_BKPT;
    goto set_coltype_cleanup;
  }

  aOld = pTab->aCol[iCol].affinity;
  aNew = sqlite3AffinityType(zType, 0);
  if( aOld!=aNew ){
    errorMPrintf(ctx, "cannot change the type of column \"%s\" to \"%s\": "
                 "that changes its affinity, and the rows and index entries "
                 "already stored were written under the old one", zCol, zType);
    rc = SQLITE_OK;
    goto set_coltype_cleanup;
  }

  /* Is this column part of the PRIMARY KEY?  If so, whether its type is
  ** exactly INTEGER decides where its values live. */
  if( pTab->iPKey==iCol ){
    bInPk = 1;
  }else{
    Index *pPk = sqlite3PrimaryKeyIndex(pTab);
    if( pPk ){
      int i;
      for(i=0; i<pPk->nKeyCol; i++){
        if( pPk->aiColumn[i]==iCol ) bInPk = 1;
      }
    }
  }
  if( bInPk ){
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
                   zCol, zType);
      rc = SQLITE_OK;
      goto set_coltype_cleanup;
    }
  }

  zNew = alterRetypeText(db, iSchema, zSql, zCol, zType, 0);
  if( zNew==0 ){
    rc = SQLITE_NOMEM_BKPT;
    goto set_coltype_cleanup;
  }
  sqlite3_result_text(ctx, zNew, -1, SQLITE_TRANSIENT);

set_coltype_cleanup:
  renameParseCleanup(&sParse);
  sqlite3DbFree(db, zNew);

set_coltype_done:
#ifndef SQLITE_OMIT_AUTHORIZATION
  db->xAuth = xAuth;
#endif
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
  }
}

/*
** Implement "ALTER TABLE <table> COLUMN <column> SET TYPE <type>".
**
** Any type may be asked for.  Which of two routes gets there depends on
** whether the column's affinity moves.
**
** A declared type fixes the column's affinity, and affinity is applied
** when a value is written.  The rows already in the table, and the keys
** already in every index over the column, were written under the old one.
** So when the affinity stays put - VARCHAR(20) to TEXT, INT to INTEGER -
** nothing on disk is affected and the stored statement is simply edited.
** When it moves, every row and every index entry has to be written again,
** which is a rebuild.
**
** One affinity-preserving change is a rebuild too.  Exactly the word
** INTEGER, on the PRIMARY KEY of a rowid table, makes the column an alias
** for the rowid; INT does not, though the two share an affinity.  Crossing
** that line moves the values between the record and the rowid, so they
** have to be carried across rather than left where they are.
**
** The choice is made here, from the in-memory schema, and that is safe
** because both routes are correct: the rebuild is right whatever the
** affinity does, and the edit re-asks the question against the stored
** statement and refuses if the answer differs there.  A stale cache can
** therefore cost a needless rebuild or produce a refusal, never a wrong
** result.
*/
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

  /* bAuth is 0 for the same reason as in sqlite3AlterAddDefault(): the
  ** callback is raised once, by alterFindCol(), with the column name. */
  assert( pSrc->nSrc==1 );
  pTab = alterFindTable(pParse, pSrc, &iDb, &zDb, 0, 2);
  if( pTab==0 ) return;
  if( pType->n==0 ){
    sqlite3ErrorMsg(pParse, "no type given for column \"%T\"", pCol);
    return;
  }
  /* alterFindCol() is what authorizes the change and reports an unknown
  ** column.  The index it returns picks the route below; the editor still
  ** resolves the name against the text it is about to edit. */
  if( alterFindCol(pParse, pTab, pCol, &iCol) ) return;
  zCol = sqlite3NameFromToken(db, pCol);
  zType = sqlite3DbStrNDup(db, pType->z, pType->n);
  if( zCol==0 || zType==0 ) goto set_type_exit;

  bRebuild = pTab->aCol[iCol].affinity!=sqlite3AffinityType(zType, 0);
  if( !bRebuild && (pTab->tabFlags & TF_WithoutRowid)==0 ){
    /* Would the column start or stop being the rowid?  Only a column that
    ** the PRIMARY KEY names can, so the question is asked of those only. */
    int bInPk = pTab->iPKey==iCol;
    if( !bInPk ){
      Index *pPk = sqlite3PrimaryKeyIndex(pTab);
      int i;
      if( pPk ) for(i=0; i<pPk->nKeyCol; i++){
        if( pPk->aiColumn[i]==iCol ) bInPk = 1;
      }
    }
    if( bInPk && (pTab->iPKey==iCol)!=(sqlite3StrICmp(zType,"INTEGER")==0) ){
      bRebuild = 1;
    }
  }

  if( bRebuild ){
    alterCodeRebuild(pParse, pTab, iDb, zCol, zType, 0);
  }else{
    sqlite3NestedParse(pParse,
        "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
        "sql = sqlite_set_coltype(%d, sql, %Q, %Q) "
        "WHERE type='table' AND tbl_name=%Q COLLATE nocase"
        , zDb, iDb, zCol, zType, pTab->zName
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
    INTERNAL_FUNCTION(sqlite_drop_notnull,   3, dropNotNullFunc),
    INTERNAL_FUNCTION(sqlite_drop_default,   3, dropDefaultFunc),
    INTERNAL_FUNCTION(sqlite_drop_check,     3, dropCheckFunc),
    INTERNAL_FUNCTION(sqlite_set_coltype,    4, setColTypeFunc),
    INTERNAL_FUNCTION(sqlite_drop_fk,       -1, dropFkFunc),
    INTERNAL_FUNCTION(sqlite_drop_pk,        2, dropPkFunc),
    INTERNAL_FUNCTION(sqlite_fail,           2, failConstraintFunc),
    INTERNAL_FUNCTION(sqlite_insert_constraint,4,insertConstraintFunc),
    INTERNAL_FUNCTION(sqlite_find_constraint,2, findConstraintFunc),
    INTERNAL_FUNCTION(sqlite_set_strict,     2, setStrictFunc),
    INTERNAL_FUNCTION(sqlite_unset_strict,   2, unsetStrictFunc),
  };
  sqlite3InsertBuiltinFuncs(aAlterTableFuncs, ArraySize(aAlterTableFuncs));
}
#endif  /* SQLITE_ALTER_TABLE */
