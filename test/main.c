//
//  main.c
//  sqlitejs
//
//  Created by Marco Bambini on 31/03/25.
//

#include <stdio.h>
#include "sqlite3.h"
#include "sqlitejs.h"

#define DB_PATH         "js_functions.sqlite"

static int print_results_callback(void *data, int argc, char **argv, char **names) {
    for (int i = 0; i < argc; i++) {
        printf("%s: %s ", names[i], argv[i] ? argv[i] : "NULL");
    }
    printf("\n");
    return SQLITE_OK;
}

int db_exec (sqlite3 *db, const char *sql) {
    int rc = sqlite3_exec(db, sql, print_results_callback, NULL, NULL);
    if (rc != SQLITE_OK) printf("Error while executing %s: %s\n", sql, sqlite3_errmsg(db));
    return rc;
}

// MARK: -

int test_serialization (const char *db_path, bool load_functions, int nstep) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) goto abort_test;
    
    #if JS_LOAD_EMBEDDED
    rc = sqlite3_js_init(db, NULL, NULL);
    #else
    // enable load extension
    rc = sqlite3_enable_load_extension(db, 1);
    if (rc != SQLITE_OK) goto abort_test;

    rc = db_exec(db, "SELECT load_extension('./dist/js');");
    if (rc != SQLITE_OK) goto abort_test;
    #endif
    
    rc = db_exec(db, (load_functions) ? "SELECT js_init_table(1);" : "SELECT js_init_table();");
    if (rc != SQLITE_OK) goto abort_test;
    
    printf("Step %d...\n", nstep);
    
    if (nstep == 1) {
        rc = db_exec(db, "SELECT js_create_scalar('SuperFunction', '(function(args){return args[0];})')");
        if (rc == SQLITE_OK) rc = db_exec(db, "SELECT SuperFunction(123), SuperFunction(12.3);");
    }
    if (nstep == 2) {
        rc = db_exec(db, "SELECT js_create_scalar('SuperFunction', '(function(args){return args[0] * 2;})')");
        if (rc == SQLITE_OK) rc = db_exec(db, "SELECT SuperFunction(123), SuperFunction(12.3);");
    }
    if (nstep == 3) {
        rc = db_exec(db, "SELECT SuperFunction(123), SuperFunction(12.3);");
    }
    if (rc != SQLITE_OK) goto abort_test;
    printf("\n");
    
abort_test:
    if (rc != SQLITE_OK) printf("Error: %s\n", sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return rc;
}

int test_execution (void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) goto abort_test;
    
    #if JS_LOAD_EMBEDDED
    rc = sqlite3_js_init(db, NULL, NULL);
    #else
    // enable load extension
    rc = sqlite3_enable_load_extension(db, 1);
    if (rc != SQLITE_OK) goto abort_test;
    
    rc = db_exec(db, "SELECT load_extension('./dist/js');");
    if (rc != SQLITE_OK) goto abort_test;
    #endif
    
    // context
    printf("Testing context\n");
    
    rc = db_exec(db, "SELECT js_eval('x = 100;');");
    rc = db_exec(db, "SELECT js_eval('x = x*2;');");
    rc = db_exec(db, "SELECT js_eval('function test1(n) {return n*x;}');");
    rc = db_exec(db, "SELECT js_eval('test1(50);');");
    
    // eval
    printf("\nTesting js_eval\n");
    rc = db_exec(db, "SELECT js_eval('136*10');");
    rc = db_exec(db, "SELECT js_eval('Math.cos(13);');");
    rc = db_exec(db, "SELECT js_eval('Math.random();');");
    
    // scalar
    printf("\nTesting js_create_scalar\n");
    rc = db_exec(db, "SELECT js_create_scalar('Cos', '(function(args){return Math.cos(args[0]);})')");
    rc = db_exec(db, "SELECT Cos(123), cos(12.3);");
    rc = db_exec(db, "SELECT js_create_scalar('Sin', '(function(args){return Math.sin(args[0]);})')");
    rc = db_exec(db, "SELECT Sin(123), sin(12.3);");
    
    // aggregate
    printf("\nTesting js_create_aggregate\n");
    rc = db_exec(db, "SELECT js_create_aggregate('Median', 'prod = 1; n = 0;', '(function(args){n++; prod = prod * args[0];})', '(function(){return Math.pow(prod, 1/n);})');");
    rc = db_exec(db, "CREATE TABLE data(val INTEGER);");
    rc = db_exec(db, "INSERT INTO data(val) VALUES (2), (4), (8);");
    rc = db_exec(db, "SELECT Median(val) FROM data;");
    rc = db_exec(db, "INSERT INTO data(val) VALUES (10), (12), (14), (16), (18), (20);");
    rc = db_exec(db, "SELECT Median(val) FROM data;");
    
    // db object
    printf("\nTesting db.exec\n");
    rc = db_exec(db, "SELECT js_eval('let rs = db.exec(''SELECT * FROM data;''); console.log(`rowset = ${rs.toArray()}`);');");
    
    // collation
    printf("\nTesting js_create_collation\n");
    const char *collation_js_function = "(function(str1,str2){"
    // Check if either string starts with 'A' or 'a'"
    "const str1StartsWithA = str1.length > 0 && (str1[0].toLowerCase() === ''a'');"
    "const str2StartsWithA = str2.length > 0 && (str2[0].toLowerCase() === ''a'');"
    // If one starts with A and the other does not, prioritize the one with A
    "if (str1StartsWithA && !str2StartsWithA) return -1;"
    "if (!str1StartsWithA && str2StartsWithA) return 1;"
    // Otherwise, perform a case-insensitive string comparison
    "return str1.toLowerCase().localeCompare(str2.toLowerCase());"
    "})";
    
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT js_create_collation('A_FIRST', '%s')", collation_js_function);
    
    rc = db_exec(db, sql);
    
    // create test table and insert data
    rc = db_exec(db,
                 "CREATE TABLE test(name TEXT);"
                 "INSERT INTO test VALUES('Zebra');"
                 "INSERT INTO test VALUES('Apple');"
                 "INSERT INTO test VALUES('banana');"
                 "INSERT INTO test VALUES('Carrot');"
                 "INSERT INTO test VALUES('acorn');");
    
    printf("Standard collation (lexicographical):\n");
    rc = db_exec(db, "SELECT name FROM test ORDER BY name;");
    
    printf("\nCustom collation (A_FIRST):\n");
    rc = db_exec(db, "SELECT name FROM test ORDER BY name COLLATE A_FIRST;");
    
    // window
    printf("\nTesting js_create_window\n");
    rc = db_exec(db, "SELECT js_create_window('sumint', 'sum = 0;', '(function(args){sum += args[0];})', '(function(){return sum;})', '(function(){return sum;})', '(function(args){sum -= args[0];})');");
    
    // example from https://www.sqlite.org/windowfunctions.html#udfwinfunc
    rc = db_exec(db,
                 "CREATE TABLE t3(x, y);"
                 "INSERT INTO t3 VALUES('a', 4), ('b', 5), ('c', 3), ('d', 8), ('e', 1);");
    
    rc = db_exec(db, "SELECT x, sumint(y) OVER (ORDER BY x ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS sum_y FROM t3 ORDER BY x;");
    
    rc = db_exec(db, "SELECT x, sumint(y) OVER (ORDER BY x ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS sum_y FROM t3 ORDER BY x;");
    
abort_test:
    if (rc != SQLITE_OK) printf("Error: %s\n", sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return rc;
}

// MARK: -

int main (void) {
    printf("SQLite-JS version: %s (engine: %s)\n\n", sqlitejs_version(), quickjs_version());

    int rc = test_execution();
    rc = test_serialization(DB_PATH, false, 1); // create and execute original implementations
    rc = test_serialization(DB_PATH, false, 2); // update functions previously registered in the js_functions table
    rc = test_serialization(DB_PATH, true,  3); // load the new implementations
    
    sqlite3_int64 current = 0;
    sqlite3_int64 highwater = 0;
    bool reset = false;
    rc = sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &current, &highwater, reset);
    if (current > 0) {
        printf("memory leak: %lld\n", current);
        return 1;
    }
    
    return rc;
}
