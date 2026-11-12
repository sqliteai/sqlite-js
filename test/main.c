//
//  main.c
//  sqlitejs
//
//  Created by Marco Bambini on 31/03/25.
//

#include <stdio.h>
#include "sqlite3.h"
#include "sqlitejs.h"
#include <pthread.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH                 "js_functions.sqlite"
#define VERBOSE                 1
#define DEBUGF(...)             do {if (VERBOSE) printf(__VA_ARGS__);} while (0)


#define TEST_THREAD_NTHREADS    1000
#define TEST_THREAD_FIRST_INIT  0
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

static int print_results_callback(void *data, int argc, char **argv, char **names) {
    for (int i = 0; i < argc; i++) {
        printf("%s: %s ", names[i], argv[i] ? argv[i] : "NULL");
    }
    printf("\n");
    return SQLITE_OK;
}

int db_exec_v2 (sqlite3 *db, const char *sql, bool expected_ok) {
    int rc = sqlite3_exec(db, sql, VERBOSE ? print_results_callback : NULL, NULL, NULL);
    if (!expected_ok && rc == SQLITE_OK) printf("FAIL: %s\n      Expected: Error, Got: OK\n", sql);
    if (expected_ok && rc != SQLITE_OK) printf("FAIL: %s\n      Expected Ok, Got Error: %s\n", sql, sqlite3_errmsg(db));
    return rc;
}

int db_exec(sqlite3 *db, const char *sql) {
    return db_exec_v2(db, sql, true);
}

// A helper function to execute a query and check its string result.
// Returns SQLITE_OK on success, SQLITE_ERROR on failure.
int check_query_v2(sqlite3 *db, const char *sql, const char **expected_array, int nexpected) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        printf("FAIL: Cannot prepare statement: %s\n", sql);
        printf("      %s\n", sqlite3_errmsg(db));
        return SQLITE_ERROR;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        printf("FAIL: Query did not return a row: %s\n", sql);
        if (rc != SQLITE_DONE) {
            printf("      %s\n", sqlite3_errmsg(db));
        }
        sqlite3_finalize(stmt);
        return SQLITE_ERROR;
    }

    for (int c=0; c<nexpected; c++) {
        const char *expected = expected_array[c];
        const unsigned char *result = sqlite3_column_text(stmt, c);
        if (result == NULL && expected != NULL) {
            printf("FAIL: %s\n      Expected: \"%s\", Got: NULL\n", sql, expected);
            sqlite3_finalize(stmt);
            return SQLITE_ERROR;
        }
        
        if (result != NULL && expected == NULL) {
            printf("FAIL: %s\n      Expected: NULL, Got: \"%s\"\n", sql, result);
            sqlite3_finalize(stmt);
            return SQLITE_ERROR;
        }
        
        if (result && expected && strcmp((const char*)result, expected) != 0) {
            printf("FAIL: %s\n      Expected: \"%s\", Got: \"%s\"\n", sql, expected, result);
            sqlite3_finalize(stmt);
            return SQLITE_ERROR;
        }
    }
    
    // Check if there are more rows, which would be unexpected
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc != SQLITE_ROW && rc != SQLITE_OK) {
        printf("FAIL: %s\n      Expected: OK, Got: rc %d\n", sql, rc);
        sqlite3_finalize(stmt);
        return rc;
    }

    sqlite3_finalize(stmt);
    DEBUGF("PASS: %s\n", sql);
    return rc;
}

int check_query(sqlite3 *db, const char *sql, const char *expected) {
    return check_query_v2(db, sql, (const char*[]){expected}, 1);
}

// A helper function to execute a query and check its single double result.
// Returns SQLITE_OK on success, SQLITE_ERROR on failure.
int check_query_double_v2(sqlite3 *db, const char *sql, double *expected_array, double *epsilon_array, int count) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        printf("FAIL: Cannot prepare statement: %s\n", sql);
        printf("      %s\n", sqlite3_errmsg(db));
        return SQLITE_ERROR;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        printf("FAIL: Query did not return a row: %s\n", sql);
        if (rc != SQLITE_DONE) {
            printf("      %s\n", sqlite3_errmsg(db));
        }
        sqlite3_finalize(stmt);
        return SQLITE_ERROR;
    }

    for (int c=0; c<count; c++) {
        double result = sqlite3_column_double(stmt, c);
        double expected = expected_array[c];
        double epsilon = epsilon_array[c];
        if (fabs(result - expected) > epsilon) {
            printf("FAIL: %s\n      Expected: %f (approx), Got: %f\n", sql, expected, result);
            sqlite3_finalize(stmt);
            return SQLITE_ERROR;
        }
    }

    // Check if there are more rows, which would be unexpected
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc != SQLITE_ROW && rc != SQLITE_OK) {
        printf("FAIL: %s\n      Expected: OK, Got: rc %d\n", sql, rc);
        sqlite3_finalize(stmt);
        return rc;
    }
    
    sqlite3_finalize(stmt);
    DEBUGF("PASS: %s\n", sql);
    return SQLITE_OK;
}

int check_query_double(sqlite3 *db, const char *sql, double expected, double epsilon) {
    return check_query_double_v2(db, sql, (double[]){expected}, (double[]){epsilon}, 1);
}

// MARK: -

// Opens an in-memory database with the extension loaded.
int test_open (sqlite3 **db) {
    int rc = sqlite3_open(":memory:", db);
    if (rc != SQLITE_OK) return rc;
    
    #if JS_LOAD_EMBEDDED
    return sqlite3_js_init(*db, NULL, NULL);
    #else
    rc = sqlite3_enable_load_extension(*db, 1);
    if (rc != SQLITE_OK) return rc;
    return db_exec(*db, "SELECT load_extension('./dist/js');");
    #endif
}

// sql must fail with an error message that contains expected
int check_error (sqlite3 *db, const char *sql, const char *expected) {
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc == SQLITE_OK || strstr(sqlite3_errmsg(db), expected) == NULL) {
        printf("FAIL: %s\n      Expected an error containing: %s, Got: %d %s\n", sql, expected, rc, (rc == SQLITE_OK) ? "OK" : sqlite3_errmsg(db));
        return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

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
    
    DEBUGF("\n");
    
    rc = check_query(db, (load_functions) ? "SELECT js_init_table(1);" : "SELECT js_init_table();", "0");
    if (rc != SQLITE_OK) goto abort_test;
    
    DEBUGF("Step %d...\n", nstep);
    
    if (nstep == 1) {
        rc = check_query(db, "SELECT js_create_scalar('SuperFunction', '(function(args){return args[0];})')", "0");
        if (rc != SQLITE_OK) goto abort_test;
        if (rc == SQLITE_OK) rc = check_query_double_v2(db, "SELECT SuperFunction(123), SuperFunction(12.3);", (double[]){123, 12.3}, (double[]){0.00000001, 0.00000001}, 2);
        if (rc != SQLITE_OK) goto abort_test;
    }
    if (nstep == 2) {
        rc = check_query(db, "SELECT js_create_scalar('SuperFunction', '(function(args){return args[0] * 2;})')", "0");
        if (rc == SQLITE_OK) rc = check_query_double_v2(db, "SELECT SuperFunction(123), SuperFunction(12.3);", (double[]){246, 24.6}, (double[]){0.00000001, 0.00000001}, 2);
        if (rc != SQLITE_OK) goto abort_test;
    }
    if (nstep == 3) {
        rc = check_query_double_v2(db, "SELECT SuperFunction(123), SuperFunction(12.3);", (double[]){246, 24.6}, (double[]){0.00000001, 0.00000001}, 2);
        if (rc != SQLITE_OK) goto abort_test;
    }

    if (rc != SQLITE_OK) goto abort_test;
    
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
    DEBUGF("Testing context\n");
    
    rc = check_query(db, "SELECT js_eval('x = 100;');", "100");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_eval('x = x*2;');", "200");
    if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT js_eval('function test1(n) {return n*x;}');");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_eval('test1(50);');", "10000");
    if (rc != SQLITE_OK) goto abort_test;
    rc = (db_exec_v2(db, "SELECT js_eval('undefined_obj;');", false) == SQLITE_ERROR ? SQLITE_OK : SQLITE_ERROR);
    if (rc != SQLITE_OK) goto abort_test;
    rc = (db_exec_v2(db, "SELECT js_create_window('code_with_error', 'sum = 0; undefined_obj', '(function(args){sum += args[0];})', '(function(){return sum;})', '(function(){return sum;})', '(function(args){sum -= args[0];})');", false) == SQLITE_ERROR ? SQLITE_OK : SQLITE_ERROR);
    if (rc != SQLITE_OK) goto abort_test;
    rc = (db_exec_v2(db, "SELECT js_create_window('code_with_error_2', 'sum = 0;', '(function(args){sum += args[0;})', '(function(){return sum;})', '(function(){return sum;})', '(function(args){sum -= args[0];})');", false) == SQLITE_ERROR ? SQLITE_OK : SQLITE_ERROR);
    if (rc != SQLITE_OK) goto abort_test;
    
    // eval
    DEBUGF("\nTesting js_eval\n");
    rc = check_query(db, "SELECT js_eval('136*10');", "1360");
    if (rc != SQLITE_OK) goto abort_test;

    rc = check_query_double(db, "SELECT js_eval('Math.cos(13);');", 0.90744678, 0.00000001);
    if (rc != SQLITE_OK) goto abort_test;

    // For queries with unpredictable results, we can still use db_exec.
    rc = db_exec(db, "SELECT js_eval('Math.random();');");
    if (rc != SQLITE_OK) goto abort_test;
    
    // scalar
    DEBUGF("\nTesting js_create_scalar\n");
    rc = check_query(db, "SELECT js_create_scalar('Cos', '(function(args){return Math.cos(args[0]);})')", "0");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query_double_v2(db, "SELECT Cos(123), cos(12.3);", (double[]){-0.887968906691855, 0.96473261788661}, (double[]){0.00000001, 0.00000001}, 2);
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_create_scalar('Sin', '(function(args){return Math.sin(args[0]);})')", "0");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query_double_v2(db, "SELECT Sin(123), sin(12.3);", (double[]){-0.459903490689591, -0.263231791365801 }, (double[]){0.00000001, 0.00000001}, 2);
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_create_scalar('f-2', '(function(args){return args[0]*10;})')", "0");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT [f-2](2);", "20");
    if (rc != SQLITE_OK) goto abort_test;
    
    // aggregate
    DEBUGF("\nTesting js_create_aggregate\n");
    rc = check_query(db, "SELECT js_create_aggregate('Median', 'prod = 1; n = 0;', '(function(args){n++; prod = prod * args[0];})', '(function(){return Math.pow(prod, 1/n);})');", "0");
    if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "CREATE TABLE data(val INTEGER);");
    if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "INSERT INTO data(val) VALUES (2), (4), (8);");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query_double(db, "SELECT Median(val) FROM data;", 4.0, 0.00000001);
    if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "INSERT INTO data(val) VALUES (10), (12), (14), (16), (18), (20);");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query_double(db, "SELECT Median(val) FROM data;", 9.48154437885665, 0.00000001);
    if (rc != SQLITE_OK) goto abort_test;

    // db object
    DEBUGF("\nTesting db.exec\n");
    rc = check_query(db, "SELECT js_eval('let rs = db.exec(''SELECT * FROM data;''); console.log(`(debug) test console log: rowset = ${rs.toArray()}`);');", NULL);
    if (rc != SQLITE_OK) goto abort_test;

    // collation
    DEBUGF("\nTesting js_create_collation\n");
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
    
    rc = check_query(db, sql, "0");
    if (rc != SQLITE_OK) goto abort_test;

    // create test table and insert data
    rc = db_exec(db,
                 "CREATE TABLE test(name TEXT);"
                 "INSERT INTO test VALUES('Zebra');"
                 "INSERT INTO test VALUES('Apple');"
                 "INSERT INTO test VALUES('banana');"
                 "INSERT INTO test VALUES('Carrot');"
                 "INSERT INTO test VALUES('acorn');");
    if (rc != SQLITE_OK) goto abort_test;

    DEBUGF("Standard collation (lexicographical):\n");
    rc = check_query(db, "SELECT name FROM test ORDER BY name;", "Apple");
    if (rc != SQLITE_OK && rc != SQLITE_ROW) goto abort_test;

    DEBUGF("\nCustom collation (A_FIRST):\n");
    rc = check_query(db, "SELECT name FROM test ORDER BY name COLLATE A_FIRST;", "acorn");
    
    // window
    DEBUGF("\nTesting js_create_window\n");
    rc = check_query(db, "SELECT js_create_window('sumint', 'sum = 0;', '(function(args){sum += args[0];})', '(function(){return sum;})', '(function(){return sum;})', '(function(args){sum -= args[0];})');", "0");
    if (rc != SQLITE_OK ) goto abort_test;

    // example from https://www.sqlite.org/windowfunctions.html#udfwinfunc
    rc = db_exec(db,
                 "CREATE TABLE t3(x, y);"
                 "INSERT INTO t3 VALUES('a', 4), ('b', 5), ('c', 3), ('d', 8), ('e', 1);");
    if (rc != SQLITE_OK ) goto abort_test;

    rc = check_query_v2(db, "SELECT x, sumint(y) OVER (ORDER BY x ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS sum_y FROM t3 ORDER BY x;", (const char*[]){"a", "9"}, 2);
    if (rc != SQLITE_OK && rc != SQLITE_ROW) goto abort_test;

    rc = check_query_v2(db, "SELECT x, sumint(y) OVER (ORDER BY x ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS sum_y FROM t3 ORDER BY x;", (const char*[]){"a", "9"}, 2);
    if (rc != SQLITE_OK && rc != SQLITE_ROW) goto abort_test;
    
    rc = SQLITE_OK;
    
abort_test:
    if (rc != SQLITE_OK) printf("Error: %s\n", sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return rc;
}

void *test_thread_init(void *ptr) {
    sqlite3 **ppdb = (sqlite3 **)ptr;
    int rc;
    
    // printf("test_thread_init\n");

    pthread_mutex_lock(&mutex);
    rc = sqlite3_open(":memory:", ppdb);
    if (rc != SQLITE_OK) goto finalize;
    
    sqlite3 *db= *ppdb;
    
    #if JS_LOAD_EMBEDDED
    rc = sqlite3_js_init(db, NULL, NULL);
    #else
    // enable load extension
    rc = sqlite3_enable_load_extension(db, 1);
    if (rc != SQLITE_OK) goto finalize;
    
    rc = db_exec(db, "SELECT load_extension('./dist/js');");
    if (rc != SQLITE_OK) goto finalize;
    #endif
        
    rc = db_exec(db, "SELECT js_set_max_stack_size(0)");
    if (rc != SQLITE_OK) goto finalize;

    rc = db_exec(db, "SELECT js_create_scalar('x10', '(function(args){return args[0]*10;})')");
    if (rc != SQLITE_OK) goto finalize;

    rc = db_exec(db, "SELECT x10(2);");
    if (rc != SQLITE_OK) goto finalize;

    rc = db_exec(db, "SELECT js_eval('136*10');");
    if (rc != SQLITE_OK) goto finalize;
    
finalize:
    pthread_cond_signal(&cond); // Signal consumer
    pthread_mutex_unlock(&mutex);
    
    // printf("test_thread_init return %d\n", rc);
    return (void*)(intptr_t)rc;
}

void *test_thread_sleep(void *ptr) {
    sqlite3_sleep(5000);
    return (void*)(intptr_t)0;
}

void *test_thread_worker(void *ptr) {
    sqlite3 **ppdb = (sqlite3 **)ptr;
    sqlite3 *db;
    
    pthread_mutex_lock(&mutex);
    while (!(db = *ppdb)) {
        pthread_cond_wait(&cond, &mutex); // Wait for data
    }
    
    // printf("test_thread_worker\n");

    int rc = check_query(db, "SELECT x10(2);", "20");
    if (rc != SQLITE_OK) goto finalize;
    
    rc = check_query(db, "SELECT js_eval('136*10');", "1360");
    if (rc != SQLITE_OK) goto finalize;

    rc = check_query(db, "SELECT js_create_scalar('x20', '(function(args){return args[0]*20;})')", "0");
    if (rc != SQLITE_OK) goto finalize;

    rc = check_query(db, "SELECT x20(2);", "40");
    if (rc != SQLITE_OK) goto finalize;
    
finalize:
    pthread_mutex_unlock(&mutex);

    // printf("test_thread_1 return %d\n", rc);
    return (void*)(intptr_t)rc;
}

int test_thread (void) {
    sqlite3 *db = NULL;
    int rc;
    int iret;
    
    // Create the a separated thread
    pthread_t thread1, thread_init, thread_sleep[TEST_THREAD_NTHREADS];
    DEBUGF("\n");

#if TEST_THREAD_FIRST_INIT
    iret = pthread_create(&thread_init, NULL, test_thread_init, (void*) &db);
    if (iret) {
        fprintf(stderr, "Error - pthread_create() init return code: %d\n", iret);
        return 1;
    }
#endif
    
    iret = pthread_create(&thread1, NULL, test_thread_worker, (void*) &db);
    if (iret) {
        fprintf(stderr, "Error - pthread_create() 1 return code: %d\n", iret);
        return 1;
    }
    
    
    for (int i=0; i<TEST_THREAD_NTHREADS; i++) {
        iret = pthread_create(&thread_sleep[i], NULL, test_thread_sleep, (void*) &db);
        if (iret) {
            fprintf(stderr, "Error - pthread_create() %d return code: %d\n", i, iret);
            return 1;
        }
    }
   
#if TEST_THREAD_FIRST_INIT == 0
    iret = pthread_create(&thread_init, NULL, test_thread_init, (void*) &db);
    if (iret) {
        fprintf(stderr, "Error - pthread_create() init return code: %d\n", iret);
        return 1;
    }
    
#endif
    
    // Wait for the threads to complete before the main thread continues
    void *thread_1_rc;
    void *thread_init_rc;
    pthread_join(thread1, &thread_1_rc);
    pthread_join(thread_init, &thread_init_rc);
    rc = (int)(intptr_t)thread_1_rc;
    // printf("Thread 1 returns: %d\n", rc);
    if (rc != SQLITE_OK) goto abort_test;
    rc = (int)(intptr_t)thread_init_rc;
    // printf("Thread init returns: %d\n", rc);
    if (rc != SQLITE_OK) goto abort_test;
    
    for (int i=0; i<TEST_THREAD_NTHREADS; i++) {
        void *thread_sleep_rc;
        pthread_join(thread_sleep[i], &thread_sleep_rc);
        rc = (int)(intptr_t)thread_sleep_rc;
        // printf("Thread %d returns: %d\n", i, rc);
        if (rc != SQLITE_OK) goto abort_test;
    }
        
abort_test:
    if (rc != SQLITE_OK) printf("Error: %s\n", sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return rc;
}

// MARK: - Interrupt -

void *test_interrupt_worker (void *ptr) {
    // give the statement time to start running JavaScript
    usleep(200 * 1000);
    sqlite3_interrupt((sqlite3 *)ptr);
    return NULL;
}

// Runs sql, which never ends unless it is interrupted, and interrupts it from another thread.
// The statement must fail as interrupted, and the connection must stay usable.
int test_interrupt_sql (sqlite3 *db, const char *sql) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, test_interrupt_worker, (void *)db) != 0) return SQLITE_ERROR;
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    pthread_join(thread, NULL);
    
    if (rc == SQLITE_OK || strstr(sqlite3_errmsg(db), "interrupted") == NULL) {
        printf("FAIL: %s\n      Expected: interrupted, Got: %d %s\n", sql, rc, sqlite3_errmsg(db));
        return SQLITE_ERROR;
    }
    return check_query(db, "SELECT js_eval('1+1');", "2");
}

int test_interrupt (void) {
    sqlite3 *db = NULL;
    
    // if the code cannot be interrupted, end the test program instead of hanging forever
    alarm(30);
    
    int rc = test_open(&db);
    if (rc != SQLITE_OK) goto abort_test;
    
    rc = test_interrupt_sql(db, "SELECT js_eval('while (true) {}');");
    if (rc != SQLITE_OK) goto abort_test;
    
    rc = db_exec(db, "SELECT js_create_scalar('forever', '(function(args){ for (;;) {} })');");
    if (rc != SQLITE_OK) goto abort_test;
    rc = test_interrupt_sql(db, "SELECT forever();");
    if (rc != SQLITE_OK) goto abort_test;
    
    // JavaScript catch blocks cannot keep running code alive
    rc = test_interrupt_sql(db, "SELECT js_eval('for (;;) { try { while (true) {} } catch (e) {} }');");
    if (rc != SQLITE_OK) goto abort_test;
    
abort_test:
    alarm(0);
    if (rc != SQLITE_OK && db) printf("Error: %s\n", sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return rc;
}

// MARK: - Direct only -

int test_direct_only (void) {
    sqlite3 *db = NULL;
    int rc = test_open(&db);
    if (rc != SQLITE_OK) goto abort_test;
    
    // every function but js_version is SQLITE_DIRECTONLY (0x80000 in the flags of function_list)
    rc = check_query_v2(db, "SELECT count(*) > 10, sum(((flags & 0x80000) != 0) != (name <> 'js_version')) FROM pragma_function_list WHERE name LIKE 'js\\_%' ESCAPE '\\';", (const char*[]){"1", "0"}, 2);
    if (rc != SQLITE_OK) goto abort_test;
    
    // trusted_schema ON lets the schema use any function that is not SQLITE_DIRECTONLY
    rc = db_exec(db, "PRAGMA trusted_schema = ON; CREATE TABLE t (a); CREATE TABLE log (msg);");
    if (rc != SQLITE_OK) goto abort_test;
    
    // a view, a trigger and a generated column cannot run JavaScript or read files
    // (SQLite reports these either when the schema is created or when it is used)
    sqlite3_exec(db, "CREATE VIEW v_eval AS SELECT js_eval('6 * 7') AS x;", NULL, NULL, NULL);
    rc = check_error(db, "SELECT x FROM v_eval;", "unsafe use of js_eval()");
    if (rc != SQLITE_OK) goto abort_test;
    sqlite3_exec(db, "CREATE VIEW v_load AS SELECT js_load_text('/etc/hosts') AS x;", NULL, NULL, NULL);
    rc = check_error(db, "SELECT x FROM v_load;", "unsafe use of js_load_text()");
    if (rc != SQLITE_OK) goto abort_test;
    sqlite3_exec(db, "CREATE TRIGGER t_insert AFTER INSERT ON t BEGIN INSERT INTO log VALUES (js_eval('\"trigger ran\"')); END;", NULL, NULL, NULL);
    rc = check_error(db, "INSERT INTO t VALUES (1);", "unsafe use of js_eval()");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT count(*) FROM log;", "0");
    if (rc != SQLITE_OK) goto abort_test;
    sqlite3_exec(db, "DROP TRIGGER IF EXISTS t_insert;", NULL, NULL, NULL);
    if (sqlite3_exec(db, "CREATE TABLE g (a, b AS (js_eval('1')));", NULL, NULL, NULL) == SQLITE_OK) {
        rc = check_error(db, "INSERT INTO g (a) VALUES (1);", "unsafe use of js_eval()");
        if (rc != SQLITE_OK) goto abort_test;
    }
    
    // a top-level statement can still use them
    rc = check_query(db, "SELECT js_eval('6 * 7');", "42");
    if (rc != SQLITE_OK) goto abort_test;
    
    // and the schema can still use js_version() and the functions created with js_create_*
    rc = db_exec(db, "SELECT js_create_scalar('twice', '(function(args){ return args[0] * 2; })');");
    if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "CREATE VIEW v_ok AS SELECT js_version() IS NOT NULL AS has_version, twice(21) AS answer;");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query_v2(db, "SELECT has_version, answer FROM v_ok;", (const char*[]){"1", "42"}, 2);
    if (rc != SQLITE_OK) goto abort_test;
    
abort_test:
    if (rc != SQLITE_OK && db) printf("Error: %s\n", sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return rc;
}

// MARK: - Aggregate errors -

int test_aggregate_errors (void) {
    sqlite3 *db = NULL;
    int rc = test_open(&db);
    if (rc != SQLITE_OK) goto abort_test;
    
    rc = db_exec(db, "CREATE TABLE t (x); INSERT INTO t VALUES (1), (2), (3);");
    if (rc != SQLITE_OK) goto abort_test;
    
    // an exception thrown by an aggregate's step function fails the statement
    rc = db_exec(db, "SELECT js_create_aggregate('step_fails', 'n = 0;', '(function(args){ if (args[0] == 2) throw new Error(\"step boom\"); n++; })', '(function(){ return n; })');");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_error(db, "SELECT step_fails(x) FROM t;", "step boom");
    if (rc != SQLITE_OK) goto abort_test;
    
    // so does one thrown by a window function's inverse function
    rc = db_exec(db, "SELECT js_create_window('inverse_fails', 'total = 0;', '(function(args){ total += args[0]; })', '(function(){ return total; })', '(function(){ return total; })', '(function(args){ throw new Error(\"inverse boom\"); })');");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_error(db, "SELECT inverse_fails(x) OVER (ORDER BY x ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM t;", "inverse boom");
    if (rc != SQLITE_OK) goto abort_test;
    
    // and the connection stays usable
    rc = check_query(db, "SELECT js_eval('[1, 2, 3].join()');", "1,2,3");
    if (rc != SQLITE_OK) goto abort_test;
    
abort_test:
    if (rc != SQLITE_OK && db) printf("Error: %s\n", sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return rc;
}

// MARK: - Exec errors -

int test_exec_errors (void) {
    sqlite3 *db = NULL;
    int rc = test_open(&db);
    if (rc != SQLITE_OK) goto abort_test;

    rc = db_exec(db, "CREATE TABLE u(a UNIQUE); INSERT INTO u VALUES (1);");
    if (rc != SQLITE_OK) goto abort_test;

    // an error while the statement runs is an exception, from next() and from toArray()
    rc = check_error(db, "SELECT js_eval('db.exec(\"INSERT INTO u VALUES (1)\").next()');", "UNIQUE constraint failed");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_error(db, "SELECT js_eval('db.exec(\"SELECT abs(-9223372036854775808)\").next()');", "integer overflow");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_error(db, "SELECT js_eval('db.exec(\"SELECT 1 UNION ALL SELECT abs(-9223372036854775808) UNION ALL SELECT 3\").toArray()');", "integer overflow");
    if (rc != SQLITE_OK) goto abort_test;

    // JavaScript can catch it
    rc = check_query(db, "SELECT js_eval('(function () { try { db.exec(\"INSERT INTO u VALUES (1)\").next(); return \"no error\"; } catch (e) { return e.message; } })()');", "UNIQUE constraint failed: u.a");
    if (rc != SQLITE_OK) goto abort_test;

    // the end of the rows is still false, and a statement that succeeds still runs
    rc = check_query(db, "SELECT js_eval('[db.exec(\"SELECT 1 WHERE 0\").next(), db.exec(\"INSERT INTO u VALUES (2)\").next(), JSON.stringify(db.exec(\"SELECT a FROM u ORDER BY a\").toArray())].join(\" \")');", "false false [[1],[2]]");
    if (rc != SQLITE_OK) goto abort_test;

abort_test:
    if (rc != SQLITE_OK && db) printf("Error: %s\n", sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return rc;
}

// MARK: -

int test_report(const char *description, int rc){
    printf("%s%-30s %s\n", VERBOSE ? "********** " : "", description, (rc == SQLITE_OK) ? "OK" : "FAILED");
    return (rc == SQLITE_OK) ? 0 : 1;
}

int main (void) {
    printf("SQLite-JS version: %s (engine: %s)\n\n", sqlitejs_version(), quickjs_version());
    int result = 0;
    
    result += test_report("Execution", test_execution());
    result += test_report("Serialization 1", test_serialization(DB_PATH, false, 1)); // create and execute original implementations
    result += test_report("Serialization 2", test_serialization(DB_PATH, false, 2)); // update functions previously registered in the js_functions table
    result += test_report("Serialization 3", test_serialization(DB_PATH, true,  3)); // load the new implementations
    result += test_report("Thread", test_thread());
    result += test_report("Interrupt", test_interrupt());
    result += test_report("Direct only", test_direct_only());
    result += test_report("Aggregate errors", test_aggregate_errors());
    result += test_report("Exec errors", test_exec_errors());

    sqlite3_int64 current = 0;
    sqlite3_int64 highwater = 0;
    bool reset = false;
    sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &current, &highwater, reset);
    if (current > 0) {
        printf("memory leak: %lld\n", current);
    }
    test_report("Memory", (int)current);

    return result;
}
