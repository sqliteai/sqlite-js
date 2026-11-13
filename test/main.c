//
//  main.c
//  sqlitejs
//
//  Created by Marco Bambini on 31/03/25.
//

#include <stdio.h>
#include <stdlib.h>   // strtoll, in the stack size test
#include <errno.h>    // EINVAL, in the stack-on-other-threads test
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

// Ends the program if the test does not finish in time, instead of hanging forever when the
// code cannot be interrupted. alarm() would do, but it is POSIX only and Windows has no such
// thing, so this is a thread; test/main.c already needs pthreads.
static volatile int test_watchdog_stop = 0;

static void *test_watchdog (void *ptr) {
    int tenths = *(int *)ptr * 10;
    for (int i = 0; (i < tenths) && !test_watchdog_stop; ++i) usleep(100 * 1000);
    if (!test_watchdog_stop) {
        printf("FAIL: the test did not finish in %d seconds: JavaScript could not be interrupted\n", *(int *)ptr);
        fflush(stdout);
        abort();
    }
    return NULL;
}

int test_interrupt (void) {
    sqlite3 *db = NULL;
    
    int seconds = 30;
    pthread_t watchdog;
    test_watchdog_stop = 0;
    bool watching = (pthread_create(&watchdog, NULL, test_watchdog, &seconds) == 0);
    
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
    test_watchdog_stop = 1;
    if (watching) pthread_join(watchdog, NULL);
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

// MARK: - Blobs -

int test_blobs (void) {
    sqlite3 *db = NULL;
    int rc = test_open(&db);
    if (rc != SQLITE_OK) goto abort_test;
    
    // a BLOB arrives as an ArrayBuffer and goes back as the same BLOB
    rc = db_exec(db, "SELECT js_create_scalar('identity', '(function(args){ return args[0]; })');");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query_v2(db, "SELECT quote(identity(x'00ff10')), quote(identity(x'')), typeof(identity(zeroblob(3)));", (const char*[]){"X'00FF10'", "X''", "blob"}, 3);
    if (rc != SQLITE_OK) goto abort_test;
    
    // ArrayBuffers and typed arrays made in JavaScript, including a view of part of a buffer
    rc = check_query_v2(db, "SELECT quote(js_eval('new Uint8Array([1, 2, 3])')), quote(js_eval('new Uint8Array([1, 2, 3, 4, 5]).subarray(1, 3)')), quote(js_eval('new Uint8Array([7, 8]).buffer')), quote(js_eval('new ArrayBuffer(0)'));", (const char*[]){"X'010203'", "X'0203'", "X'0708'", "X''"}, 4);
    if (rc != SQLITE_OK) goto abort_test;
    
    // a function that makes a BLOB from a BLOB
    rc = db_exec(db, "SELECT js_create_scalar('reverse_bytes', '(function(args){ return new Uint8Array(args[0]).reverse(); })');");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT quote(reverse_bytes(x'010203'));", "X'030201'");
    if (rc != SQLITE_OK) goto abort_test;
    
    // a detached buffer has no bytes, so returning it is an error
    rc = check_error(db, "SELECT js_eval('(function () { const b = new ArrayBuffer(4); b.transfer(); return b; })()');", "detached");
    if (rc != SQLITE_OK) goto abort_test;
    
    // db.exec() binds them as BLOBs too
    rc = check_query(db, "SELECT js_eval('(function () { const rs = db.exec(\"SELECT quote(?), quote(?)\", new Uint8Array([1, 2]), new ArrayBuffer(0)); rs.next(); return rs.get(0) + \" \" + rs.get(1); })()');", "X'0102' X''");
    if (rc != SQLITE_OK) goto abort_test;
    
    // a typed array that tracks the length of a resizable buffer has the buffer's current length,
    // after the buffer shrinks (by 1 MB, so that reading the old length would read far past it)
    // or grows
    rc = check_query_v2(db, "SELECT quote(js_eval('(function () { const ab = new ArrayBuffer(16, { maxByteLength: 16 }); const u = new Uint8Array(ab); u.fill(7); ab.resize(2); return u; })()')), length(js_eval('(function () { const ab = new ArrayBuffer(1024 * 1024, { maxByteLength: 1024 * 1024 }); const u = new Uint8Array(ab); ab.resize(1); return u; })()')), length(js_eval('(function () { const ab = new ArrayBuffer(4, { maxByteLength: 16 }); const u = new Uint8Array(ab, 2); ab.resize(16); return u; })()'));", (const char*[]){"X'0707'", "1", "14"}, 3);
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_eval('(function () { const ab = new ArrayBuffer(1024 * 1024, { maxByteLength: 1024 * 1024 }); const u = new Uint8Array(ab); ab.resize(3); const rs = db.exec(\"SELECT length(?)\", u); rs.next(); return rs.get(0); })()');", "3");
    if (rc != SQLITE_OK) goto abort_test;
    
    // a typed array of fixed length that a shrunken buffer no longer holds is an error
    rc = check_error(db, "SELECT js_eval('(function () { const ab = new ArrayBuffer(16, { maxByteLength: 16 }); const u = new Uint8Array(ab, 0, 8); ab.resize(4); return u; })()');", "detached or resized");
    if (rc != SQLITE_OK) goto abort_test;

    // db.exec() throws for a buffer it cannot read, as returning it fails, instead of binding NULL
    rc = check_query_v2(db, "SELECT js_eval('(function () { const ab = new ArrayBuffer(16, { maxByteLength: 16 }); const u = new Uint8Array(ab, 0, 8); ab.resize(4); try { db.exec(\"SELECT ?\", u); return \"bound\"; } catch (e) { return e.message; } })()'), js_eval('(function () { const b = new ArrayBuffer(4); b.transfer(); try { db.exec(\"SELECT ?\", b); return \"bound\"; } catch (e) { return \"thrown\"; } })()');", (const char*[]){"ArrayBuffer is detached or resized", "thrown"}, 2);
    if (rc != SQLITE_OK) goto abort_test;

    // a view's own offset and length count, not properties a script defines over them
    rc = check_query_v2(db, "SELECT quote(js_eval('(function () { const u = new Uint8Array([1, 2, 3]).subarray(1, 2); Object.defineProperty(u, \"byteOffset\", { value: 0 }); Object.defineProperty(u, \"byteLength\", { value: 3 }); return u; })()')), js_eval('(function () { const u = new Uint8Array([1, 2, 3]).subarray(1, 2); Object.defineProperty(u, \"byteOffset\", { value: 0 }); const rs = db.exec(\"SELECT quote(?)\", u); rs.next(); return rs.get(0); })()');", (const char*[]){"X'02'", "X'02'"}, 2);
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT quote(js_eval('(function () { Object.defineProperty(Object.getPrototypeOf(Uint8Array.prototype), \"byteOffset\", { get() { return 0; } }); return new Uint8Array([1, 2, 3]).subarray(2); })()'));", "X'03'");
    if (rc != SQLITE_OK) goto abort_test;
    
abort_test:
    if (rc != SQLITE_OK && db) printf("Error: %s\n", sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return rc;
}

// MARK: - Numbers -

int test_numbers (void) {
    sqlite3 *db = NULL;
    int rc = test_open(&db);
    if (rc != SQLITE_OK) goto abort_test;
    
    rc = db_exec(db, "SELECT js_create_scalar('identity', '(function(args){ return args[0]; })');");
    if (rc != SQLITE_OK) goto abort_test;
    
    // integers keep their type and value through JavaScript, beyond the 32-bit range too
    rc = check_query(db, "WITH v(x) AS (VALUES (0), (42), (-7), (2147483647), (2147483648), (-2147483649), (9007199254740991), (-9007199254740991)) SELECT group_concat(typeof(identity(x)) || ' ' || (identity(x) = x), ',') FROM v;", "integer 1,integer 1,integer 1,integer 1,integer 1,integer 1,integer 1,integer 1");
    if (rc != SQLITE_OK) goto abort_test;
    
    // REALs that are not whole numbers stay REAL; a whole number is an INTEGER, as JavaScript cannot tell 3.0 from 3
    rc = check_query_v2(db, "SELECT quote(identity(3.5)), quote(identity(-0.25)), quote(identity(1e300)), quote(identity(3.0));", (const char*[]){"3.5", "-0.25", "1.0e+300", "3"}, 4);
    if (rc != SQLITE_OK) goto abort_test;
    
    // beyond 2^53 - 1 a double cannot hold every integer, so the result is a REAL
    rc = check_query(db, "SELECT typeof(identity(9007199254740993));", "real");
    if (rc != SQLITE_OK) goto abort_test;
    
    // results of arithmetic
    rc = check_query_v2(db, "SELECT quote(js_eval('2.5 * 2')), quote(js_eval('1e10')), typeof(js_eval('0.1 + 0.2')), typeof(js_eval('NaN'));", (const char*[]){"5", "10000000000", "real", "null"}, 4);
    if (rc != SQLITE_OK) goto abort_test;
    
    // an aggregate whose result is past the 32-bit range
    rc = db_exec(db, "SELECT js_create_aggregate('js_sum', 'total = 0;', '(function(args){ total += args[0]; })', '(function(){ return total; })');");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "WITH v(x) AS (VALUES (3000000000), (1)) SELECT typeof(js_sum(x)) || ' ' || js_sum(x) FROM v;", "integer 3000000001");
    if (rc != SQLITE_OK) goto abort_test;
    
    // db.exec() binds numbers the same way
    rc = check_query(db, "SELECT js_eval('(function () { const rs = db.exec(\"SELECT typeof(?), typeof(?), typeof(?)\", 2147483648, 2.5 * 2, 2.5); rs.next(); return [rs.get(0), rs.get(1), rs.get(2)].join(); })()');", "integer,integer,real");
    if (rc != SQLITE_OK) goto abort_test;
    
abort_test:
    if (rc != SQLITE_OK && db) printf("Error: %s\n", sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return rc;
}

// MARK: - Memory limit -

// Creates and runs an aggregate on a new connection with a memory limit of limit_kb, if not 0,
// or with the default limit after filling all but free_kb of it. The statements may fail for
// lack of memory, but must not crash: QuickJS can crash if it runs out of memory while it
// creates a context, which every aggregate does.
int test_aggregate_near_limit (int limit_kb, int free_kb) {
    sqlite3 *db = NULL;
    char sql[256];
    int rc = test_open(&db);
    if (rc != SQLITE_OK) goto abort_test;
    
    if (limit_kb) snprintf(sql, sizeof(sql), "SELECT js_set_memory_limit(%d * 1024);", limit_kb);
    else snprintf(sql, sizeof(sql), "SELECT js_eval('globalThis.keep = []; try { for (;;) keep.push(new ArrayBuffer(1024)); } catch (e) {} keep.length -= %d; 0');", free_kb);
    rc = db_exec(db, sql);
    if (rc != SQLITE_OK) goto abort_test;
    
    sqlite3_exec(db, "SELECT js_create_aggregate('n_rows', 'n = 0;', '(function(args){ n++; })', '(function(){ return n; })');", NULL, NULL, NULL);
    sqlite3_exec(db, "WITH v(x) AS (VALUES (1), (2)) SELECT n_rows(x) FROM v;", NULL, NULL, NULL);
    
abort_test:
    if (db) sqlite3_close(db);
    return rc;
}

int test_memory_limit (void) {
    sqlite3 *db = NULL;
    int rc = test_open(&db);
    if (rc != SQLITE_OK) goto abort_test;
    
    // the default limit, 64 MB, allows 16 MB but not 128 MB
    rc = check_query(db, "SELECT js_eval('new ArrayBuffer(16 * 1024 * 1024).byteLength');", "16777216");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_error(db, "SELECT js_eval('new ArrayBuffer(128 * 1024 * 1024).byteLength');", "out of memory");
    if (rc != SQLITE_OK) goto abort_test;
    
    // code that keeps allocating stops at the limit, and the connection stays usable
    rc = check_error(db, "SELECT js_eval('(function () { const a = []; for (;;) a.push(new Array(100000).fill(1)); })()');", "out of memory");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_eval('[1, 2, 3].map(x => x * 2).join()');", "2,4,6");
    if (rc != SQLITE_OK) goto abort_test;
    
    // a higher limit, then no limit
    rc = db_exec(db, "SELECT js_set_memory_limit(256 * 1024 * 1024);");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_eval('new ArrayBuffer(128 * 1024 * 1024).byteLength');", "134217728");
    if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT js_set_memory_limit(0);");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_eval('new ArrayBuffer(128 * 1024 * 1024).byteLength');", "134217728");
    if (rc != SQLITE_OK) goto abort_test;
    
    // a lower limit
    rc = db_exec(db, "SELECT js_set_memory_limit(8 * 1024 * 1024);");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_error(db, "SELECT js_eval('new ArrayBuffer(16 * 1024 * 1024).byteLength');", "out of memory");
    if (rc != SQLITE_OK) goto abort_test;

    // garbage is still collected with more than two thirds of the limit in use: with 48 of 64 MB
    // held, code that makes many times the free memory in cyclic garbage, which only the
    // collector frees, runs.
    // On a connection of its own, and with megabytes to spare: running out of memory leaves some
    // memory allocated and reachable, and how much an object costs is up to the allocator of the
    // platform, which rounds allocations up by more once the heap of the process is warm.
    if (db) sqlite3_close(db);
    db = NULL;
    rc = test_open(&db);
    if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT js_set_memory_limit(64 * 1024 * 1024);");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_eval('globalThis.keep = new ArrayBuffer(48 * 1024 * 1024); for (let i = 0; i < 300000; i++) { const o = {}; o.o = o; } keep.byteLength');", "50331648");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_create_aggregate('n_rows', 'n = 0;', '(function(args){ n++; })', '(function(){ return n; })');", "0");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "WITH RECURSIVE g(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM g WHERE x < 300) SELECT sum(c) FROM (SELECT n_rows(x) AS c FROM g GROUP BY x);", "300");
    if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT js_eval('delete globalThis.keep; 0');");
    if (rc != SQLITE_OK) goto abort_test;

    // a limit set under the memory already allocated, when that memory is garbage, still leaves
    // the connection usable: QuickJS refuses every allocation while the memory in use is over
    // the limit, and it collects only when it creates an object, which it cannot do without
    // allocating, so the collector has to run as the limit changes
    if (db) sqlite3_close(db);
    db = NULL;
    rc = test_open(&db);
    if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT js_set_memory_limit(0);");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_eval('globalThis.g = []; for (let i = 0; i < 200000; i++) { const o = {}; o.o = o; g.push(o); } g.length');", "200000");
    if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT js_eval('delete globalThis.g; 0');");
    if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT js_set_memory_limit(4 * 1024 * 1024);");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_eval('[1, 2, 3].map(x => x * 2).join()');", "2,4,6");
    if (rc != SQLITE_OK) goto abort_test;

    // running out of memory through many small allocations, which leaves no room for the error
    // object, is reported as "out of memory" too
    rc = check_error(db, "SELECT js_eval('(function () { let h = null; for (;;) h = {n: h}; })()');", "out of memory");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_eval('1 + 1');", "2");
    if (rc != SQLITE_OK) goto abort_test;
    
    // only a non-negative integer is a limit
    const char *invalid[] = {"-1", "1.5", "'1000000'", "NULL", "x'00'"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char sql[128];
        snprintf(sql, sizeof(sql), "SELECT js_set_memory_limit(%s);", invalid[i]);
        rc = check_error(db, sql, "The memory limit must be an integer number of bytes");
        if (rc != SQLITE_OK) goto abort_test;
    }
    
    // aggregates fail cleanly near the limit: under small limits, and with little memory left
    for (int kb = 100; kb <= 420; kb += 8) {
        rc = test_aggregate_near_limit(kb, 0);
        if (rc != SQLITE_OK) goto abort_test;
    }
    for (int kb = 0; kb <= 16; ++kb) {
        rc = test_aggregate_near_limit(0, kb);
        if (rc != SQLITE_OK) goto abort_test;
    }
    
abort_test:
    if (rc != SQLITE_OK && db) printf("Error: %s\n", sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return rc;
}

// MARK: - Stack size -

int test_stack_size (void) {
    sqlite3 *db = NULL;
    char sql[256];
    int rc = test_open(&db);
    if (rc != SQLITE_OK) goto abort_test;

    // code that calls itself without end fails, and the connection stays usable
    const char *recurse = "SELECT js_eval('(function f(n) { return f(n + 1); })(0)');";
    rc = check_error(db, recurse, "Maximum call stack size exceeded");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_query(db, "SELECT js_eval('1 + 1');", "2");
    if (rc != SQLITE_OK) goto abort_test;

    // a size is an integer no larger than the thread's stack, or 0 for no check: text would read
    // as 0, and a size larger than the stack never trips the check, so the code above would
    // crash the process
    const char *invalid[] = {"'abc'", "-1", "1.5", "'1048576'", "NULL", "x'00'", "1073741824"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        snprintf(sql, sizeof(sql), "SELECT js_set_max_stack_size(%s);", invalid[i]);
        rc = check_error(db, sql, "The stack size must be an integer number of bytes from 1 to ");
        if (rc != SQLITE_OK) goto abort_test;
    }

    // close to the largest size allowed, the code still stops, also when it calls itself
    // through SQL, which puts SQLite's frames between QuickJS's stack checks
    const char *most_text = strstr(sqlite3_errmsg(db), "from 1 to ");
    long long most = most_text ? strtoll(most_text + strlen("from 1 to "), NULL, 10) : 0;
    if (most <= 64 * 1024) { printf("FAIL: no stack size range in: %s\n", sqlite3_errmsg(db)); rc = SQLITE_ERROR; goto abort_test; }
    snprintf(sql, sizeof(sql), "SELECT js_set_max_stack_size(%lld);", most - 4096);
    rc = db_exec(db, sql);
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_error(db, recurse, "Maximum call stack size exceeded");
    if (rc != SQLITE_OK) goto abort_test;
    rc = db_exec(db, "SELECT js_create_scalar('recurse_sql', '(function (args) { try { const rs = db.exec(\"SELECT recurse_sql(?)\", args[0] + 1); if (rs.next()) return rs.get(0); } catch (e) {} throw new Error(\"stopped\"); })');");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_error(db, "SELECT recurse_sql(0);", "stopped");
    if (rc != SQLITE_OK) goto abort_test;

    // a smaller size
    rc = db_exec(db, "SELECT js_set_max_stack_size(256 * 1024);");
    if (rc != SQLITE_OK) goto abort_test;
    rc = check_error(db, recurse, "Maximum call stack size exceeded");
    if (rc != SQLITE_OK) goto abort_test;

    // 0 is still allowed, for connections used from several threads (see test_thread)
    rc = check_query(db, "SELECT quote(js_set_max_stack_size(0));", "NULL");
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

// MARK: - Stack on other threads -

typedef struct {
    sqlite3     *db;
    int         depth;      // how deep deep() is asked to go
    int         rc;
} stack_thread_args;

// Creates deep(n), which calls itself n levels deep, and forever(), which calls itself without end.
static int stack_thread_functions (sqlite3 *db) {
    int rc = db_exec(db, "SELECT js_create_scalar('deep', '(function (args) { return (function f(n) { return (n === 0) ? 0 : 1 + f(n - 1); })(args[0]); })');");
    if (rc != SQLITE_OK) return rc;
    return db_exec(db, "SELECT js_create_scalar('forever', '(function (args) { return (function f(n) { return f(n + 1); })(0); })');");
}

// Runs deep() and forever() on the calling thread: deep() returns its depth, and forever() stops
// with an error instead of crashing.
static void *stack_thread_run (void *ptr) {
    stack_thread_args *args = (stack_thread_args *)ptr;
    char sql[64], expected[16];
    snprintf(sql, sizeof(sql), "SELECT deep(%d);", args->depth);
    snprintf(expected, sizeof(expected), "%d", args->depth);
    args->rc = check_query(args->db, sql, expected);
    if (args->rc == SQLITE_OK) args->rc = check_error(args->db, "SELECT forever();", "Maximum call stack size exceeded");
    return NULL;
}

// Runs forever() on the calling thread, which must stop with an error instead of crashing.
static void *stack_thread_forever (void *ptr) {
    stack_thread_args *args = (stack_thread_args *)ptr;
    args->rc = check_error(args->db, "SELECT forever();", "Maximum call stack size exceeded");
    return NULL;
}

static void *stack_thread_open (void *ptr) {
    stack_thread_args *args = (stack_thread_args *)ptr;
    args->rc = test_open(&args->db);
    if (args->rc == SQLITE_OK) args->rc = stack_thread_functions(args->db);
    return NULL;
}

// Opens a connection and runs JavaScript on a thread whose whole stack is smaller than twice
// SQLITE_JS_STACK_MARGIN, so that keeping the margin free would leave JavaScript no room at all.
// The extension must still load and work: a stack size of 0 would turn QuickJS's check off and
// one of a few bytes stops every program, including the code that creates a context, which once
// made the extension fail to load at all.
static void *stack_thread_tiny (void *ptr) {
    stack_thread_args *args = (stack_thread_args *)ptr;
    sqlite3 *db = NULL;
    args->rc = test_open(&db);
    if (args->rc == SQLITE_OK) args->rc = check_query(db, "SELECT js_eval('6 * 7');", "42");
    // and runaway recursion must still stop with an error rather than crash
    if (args->rc == SQLITE_OK) args->rc = stack_thread_functions(db);
    if (args->rc == SQLITE_OK) args->rc = check_error(db, "SELECT forever();", "Maximum call stack size exceeded");
    if (db) sqlite3_close(db);
    return NULL;
}

// Runs fn(args) on a new thread with a stack of stack_size bytes, or the default if 0. Returns
// SQLITE_NOTFOUND where the platform has no stack that small, which a case that needs an unusually
// small one skips rather than reporting as a failure: PTHREAD_STACK_MIN is 128 KB on aarch64 glibc,
// where a 64 KB stack cannot be had at all, and it is a run-time value there rather than a constant.
static int stack_thread (void *(*fn)(void *), stack_thread_args *args, size_t stack_size) {
    pthread_attr_t attr;
    pthread_t thread;
    if (pthread_attr_init(&attr) != 0) { printf("FAIL: pthread_attr_init\n"); return SQLITE_ERROR; }
    if (stack_size > 0) {
        int set = pthread_attr_setstacksize(&attr, stack_size);
        if (set != 0) {
            pthread_attr_destroy(&attr);
            if (set == EINVAL) {
                printf("SKIP: this platform has no thread stack of %zu bytes\n", stack_size);
                return SQLITE_NOTFOUND;
            }
            printf("FAIL: a thread stack of %zu bytes: error %d\n", stack_size, set);
            return SQLITE_ERROR;
        }
    }
    int rc = pthread_create(&thread, &attr, fn, args);
    pthread_attr_destroy(&attr);
    if (rc != 0) { printf("FAIL: pthread_create: error %d\n", rc); return SQLITE_ERROR; }
    pthread_join(thread, NULL);
    return args->rc;
}

// A connection runs on whichever thread calls it, as in an app's connection pool. QuickJS measured
// the stack from the thread that opened the connection, so on another thread JavaScript failed at
// once with "Maximum call stack size exceeded", or code that called itself without end crashed.
int test_stack_threads (void) {
    // each level of JavaScript recursion takes about 1.5 KB of native stack, and the small stack
    // below leaves about 500 KB after SQLITE_JS_STACK_MARGIN. It is under JS_DEFAULT_STACK_SIZE,
    // so the size is still lowered to fit, with room to spare for a build whose frames are
    // larger, such as one under AddressSanitizer.
    stack_thread_args args = {NULL, 100, SQLITE_OK};

    // opened on this thread, used on another. The stack of that thread is asked for rather than
    // left to the platform, whose default is 8 MB on Linux but 512 KB on macOS, which leaves
    // 100 levels little room once SQLITE_JS_STACK_MARGIN is kept free.
    int rc = test_open(&args.db);
    if (rc == SQLITE_OK) rc = stack_thread_functions(args.db);
    if (rc == SQLITE_OK) rc = stack_thread(stack_thread_run, &args, 1024 * 1024);
    if (rc != SQLITE_OK) goto abort_test;

    // and on another thread with a small stack, where the size is lowered to fit
    rc = stack_thread(stack_thread_run, &args, 768 * 1024);
    if (rc != SQLITE_OK) goto abort_test;

    // and back on this thread
    stack_thread_run(&args);
    rc = args.rc;
    if (rc != SQLITE_OK) goto abort_test;
    sqlite3_close(args.db);
    args.db = NULL;

    // opened on another thread, used on this one
    rc = stack_thread(stack_thread_open, &args, 0);
    if (rc != SQLITE_OK) goto abort_test;
    stack_thread_run(&args);
    rc = args.rc;
    if (rc != SQLITE_OK) goto abort_test;

    // and on a thread with less stack left than SQLITE_JS_STACK_MIN: the size is still lowered to
    // fit, since the default would let code that calls itself without end run past the stack.
    // Skipped where the platform has no stack that small, as aarch64 glibc has not.
    rc = stack_thread(stack_thread_forever, &args, 64 * 1024);
    if (rc == SQLITE_NOTFOUND) rc = SQLITE_OK;
    if (rc != SQLITE_OK) goto abort_test;

    // a thread whose whole stack is under twice the margin still gets a usable runtime
    rc = stack_thread(stack_thread_tiny, &args, 256 * 1024);

abort_test:
    if (rc != SQLITE_OK && args.db) printf("Error: %s\n", sqlite3_errmsg(args.db));
    if (args.db) sqlite3_close(args.db);
    return rc;
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
    result += test_report("Blobs", test_blobs());
    result += test_report("Numbers", test_numbers());
    result += test_report("Memory limit", test_memory_limit());
    result += test_report("Stack size", test_stack_size());
    result += test_report("Stack on other threads", test_stack_threads());

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
