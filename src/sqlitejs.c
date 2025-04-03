//
//  sqlitejs.c
//  sqlitejs
//
//  Created by Marco Bambini on 31/03/25.
//

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <assert.h>
#include "sqlitejs.h"
#include "quickjs.h"

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT1
#endif

#ifdef _WIN32
#define APIEXPORT       __declspec(dllexport)
#else
#define APIEXPORT
#endif

typedef struct {
    JSRuntime           *runtime;
    JSContext           *context;
    sqlite3             *db;
} globaljs_context;

typedef struct {
    bool                inited;
    
    globaljs_context    *js_ctx;        // never to release
    JSContext           *agg_ctx;       // to release (windows and aggregate functions)
    
    const char          *init_code;     // release only if complete (aggregate functions only)
    const char          *step_code;     // release only if complete (scalar, collation, windows and aggregate functions)
    const char          *final_code;    // release only if complete (windows and aggregate functions)
    const char          *value_code;    // release only if complete (window functions only)
    const char          *inverse_code;  // release only if complete (window functions only)
    JSValue             step_func;      // to release (scalar, collation, windows and aggregate functions)
    JSValue             final_func;     // to release (windows and aggregate functions)
    JSValue             value_func;     // to release (window functions only)
    JSValue             inverse_func;   // to release (window functions only)
} functionjs_context;

#define SQLITEJS_VERSION        "1.0.0"
static char gversion[128];

// MARK: - Initializer -

static globaljs_context *globaljs_init (sqlite3 *db) {
    JSRuntime *rt = NULL;
    JSContext *ctx = NULL;
    
    globaljs_context *js = (globaljs_context *)calloc(1, sizeof(globaljs_context));
    if (!js) return NULL;
    
    rt = JS_NewRuntime();
    if (!rt) goto abort_init;
    
    //JS_SetMemoryLimit(rt, 80 * 1024);
    //JS_SetMaxStackSize(rt, 10 * 1024);
    
    ctx = JS_NewContext(rt);
    if (!ctx) goto abort_init;
    
    JSValue global = JS_GetGlobalObject(ctx);
    // add any global objects or functions here
    JS_FreeValue(ctx, global);
    
    js->db = db;
    js->runtime = rt;
    js->context = ctx;
    return js;
    
abort_init:
    if (rt) JS_FreeRuntime(rt);
    if (ctx) JS_FreeContext(ctx);
    if (js) free(js);
    return NULL;
}

static void globaljs_free (globaljs_context *js) {
    if (!js) return;
    
    if (js->runtime) JS_FreeRuntime(js->runtime);
    if (js->context) JS_FreeContext(js->context);
    free(js);
}

static functionjs_context *functionjs_init (globaljs_context *jsctx, const char *init_code, const char *step_code, const char *final_code, const char *value_code, const char *inverse_code) {
    // make a copy of all the code
    functionjs_context *fctx = NULL;
    char *init_code_copy = NULL;
    char *step_code_copy = NULL;
    char *final_code_copy = NULL;
    char *value_code_copy = NULL;
    char *inverse_code_copy = NULL;
    
    fctx = (functionjs_context *)calloc(1, sizeof(functionjs_context));
    if (!fctx) goto cleanup;
    
    if (init_code) {
        init_code_copy = strdup(init_code);
        if (!init_code_copy) goto cleanup;
    }
    
    if (step_code) {
        step_code_copy = strdup(step_code);
        if (!step_code_copy) goto cleanup;
    }
    
    if (final_code) {
        final_code_copy = strdup(final_code);
        if (!final_code_copy) goto cleanup;
    }
    
    if (value_code) {
        value_code_copy = strdup(value_code);
        if (!value_code_copy) goto cleanup;
    }
    
    if (inverse_code) {
        inverse_code_copy = strdup(inverse_code);
        if (!inverse_code_copy) goto cleanup;
    }
    
    fctx->inited = false;
    fctx->js_ctx = jsctx;
    fctx->agg_ctx = NULL;
    
    fctx->init_code = init_code_copy;
    fctx->step_code = step_code_copy;
    fctx->final_code = final_code_copy;
    fctx->value_code = value_code_copy;
    fctx->inverse_code = inverse_code_copy;
    
    fctx->step_func = JS_NULL;
    fctx->final_func = JS_NULL;
    fctx->value_func = JS_NULL;
    fctx->inverse_func = JS_NULL;
    
    return fctx;
    
cleanup:
    if (init_code_copy) free(init_code_copy);
    if (step_code_copy) free(step_code_copy);
    if (final_code_copy) free(final_code_copy);
    if (value_code_copy) free(value_code_copy);
    if (inverse_code_copy) free(inverse_code_copy);
    if (fctx) free(fctx);
    return NULL;
}

static void functionjs_free (functionjs_context *fctx, bool complete) {
    if (!fctx) return;
    JSContext *context = (fctx->agg_ctx) ? fctx->agg_ctx : fctx->js_ctx->context;
    
    // always to release
    if (!JS_IsNull(fctx->step_func)) JS_FreeValue(context, fctx->step_func);
    if (!JS_IsNull(fctx->final_func)) JS_FreeValue(context, fctx->final_func);
    if (!JS_IsNull(fctx->value_func)) JS_FreeValue(context, fctx->value_func);
    if (!JS_IsNull(fctx->inverse_func)) JS_FreeValue(context, fctx->inverse_func);
    if (fctx->agg_ctx) JS_FreeContext(fctx->agg_ctx);
    
    fctx->step_func = JS_NULL;
    fctx->final_func = JS_NULL;
    fctx->value_func = JS_NULL;
    fctx->inverse_func = JS_NULL;
    fctx->agg_ctx = NULL;
    
    if (complete) {
        if (fctx->init_code) free((void *)fctx->init_code);
        if (fctx->step_code) free((void *)fctx->step_code);
        if (fctx->final_code) free((void *)fctx->final_code);
        if (fctx->value_code) free((void *)fctx->value_code);
        if (fctx->inverse_code) free((void *)fctx->inverse_code);
        free(fctx);
    }
}

// MARK: - Utils -

static void compute_version_string (void) {
    const char *s1 = SQLITEJS_VERSION;
    const char *s2 = JS_GetVersion();
    
    const char *p1 = s1;
    const char *p2 = s2;
    char *res_ptr = gversion;
    bool first_component = true;
    
    while (*p1 != 0 || *p2 != 0) {
        // extract a numeric component from each string
        int num1 = 0, num2 = 0;
        
        // parse number from s1
        while (*p1 != '\0' && isdigit((unsigned char)*p1)) {
            num1 = num1 * 10 + (*p1 - '0');
            p1++;
        }
        
        // parse number from s2
        while (*p2 != '\0' && isdigit((unsigned char)*p2)) {
            num2 = num2 * 10 + (*p2 - '0');
            p2++;
        }
        
        // add the numbers
        int sum = num1 + num2;
        
        // add a delimiter if this isn't the first component
        if (!first_component) {
            *res_ptr++ = '.';
        } else {
            first_component = false;
        }
        
        // Convert sum to string and append to result
        char temp[32];
        sprintf(temp, "%d", sum);
        strcpy(res_ptr, temp);
        res_ptr += strlen(temp);
        
        // skip delimiters
        if (*p1 == '.') p1++;
        if (*p2 == '.') p2++;
    }
}

static void js_dump_globals (JSContext *ctx) {
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSPropertyEnum *props;
    uint32_t len;

    if (JS_GetOwnPropertyNames(ctx, &props, &len, global_obj,
        JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        
        for (uint32_t i = 0; i < len; i++) {
            JSAtom atom = props[i].atom;
            const char *key = JS_AtomToCString(ctx, atom);
            JSValue val = JS_GetProperty(ctx, global_obj, atom);

            if (JS_IsFunction(ctx, val)) {
                // Try to get function source
                JSValue str = JS_ToString(ctx, val);
                const char *func_code = JS_ToCString(ctx, str);
                printf("%s\n", func_code ? func_code : "// [function: unknown source]");
                JS_FreeCString(ctx, func_code);
                JS_FreeValue(ctx, str);
            } else {
                JSValue str = JS_JSONStringify(ctx, val, JS_UNDEFINED, JS_UNDEFINED);
                const char *val_str = JS_ToCString(ctx, str);
                printf("let %s = %s;\n", key, val_str ? val_str : "undefined");
                JS_FreeCString(ctx, val_str);
                JS_FreeValue(ctx, str);
            }

            JS_FreeValue(ctx, val);
            JS_FreeCString(ctx, key);
            JS_FreeAtom(ctx, atom);
        }
        js_free(ctx, props);
    }
    JS_FreeValue(ctx, global_obj);
}

static void js_error_to_sqlite (sqlite3_context *context, JSContext *js_ctx, JSValue value, const char *default_error) {
    if (!default_error) default_error = "Unknown JavaScript exception";
    const char *err_msg = NULL;
    JSValue exception = JS_NULL;
    
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(js_ctx);
        if (JS_IsObject(exception)) {
            JSValue message = JS_GetPropertyStr(js_ctx, exception, "message");
            if (!JS_IsException(message) && JS_IsString(message)) {
                err_msg = JS_ToCString(js_ctx, message);
            }
            JS_FreeValue(js_ctx, message);
        }
    }
    
    // set a default error message and code
    sqlite3_result_error(context, (err_msg) ? err_msg : default_error, -1);
    sqlite3_result_error_code(context, SQLITE_ERROR);
    
    // clean-up
    if (err_msg) JS_FreeCString(js_ctx, err_msg);
    if (!JS_IsNull(exception)) JS_FreeValue(js_ctx, exception);
}

static void js_value_to_sqlite (sqlite3_context *context, JSContext *js_ctx, JSValue value) {
    // check for exceptions first and convert it to a proper error message (if any)
    if (JS_IsException(value)) {
        js_error_to_sqlite(context, js_ctx, value, NULL);
        return;
    }
    
    // check for null or undefined
    if (JS_IsNull(value) || JS_IsUndefined(value)) {
        sqlite3_result_null(context);
        return;
    }
    
    // handle numbers
    if (JS_IsNumber(value)) {
        int tag = JS_VALUE_GET_TAG(value);
        if (tag == JS_TAG_INT) {
            int32_t num;
            JS_ToInt32(js_ctx, &num, value);
            sqlite3_result_int(context, num);
        } else if (tag == JS_TAG_FLOAT64) {
            double num;
            JS_ToFloat64(js_ctx, &num, value);
            sqlite3_result_double(context, num);
        } else {
            // handle BigInt if needed
            sqlite3_result_null(context);
        }
        return;
    }
    
    // handle booleans
    if (JS_IsBool(value)) {
        sqlite3_result_int(context, JS_ToBool(js_ctx, value));
        return;
    }
    
    // handle strings
    if (JS_IsString(value)) {
        size_t len = 0;
        const char * str= JS_ToCStringLen(js_ctx, &len, value);
        if (str) {
            sqlite3_result_text(context, str, (int)len, SQLITE_TRANSIENT);
            JS_FreeCString(js_ctx, str);
        } else {
            sqlite3_result_error(context, "Failed to convert JS string", -1);
        }
        return;
    }
    
    if (JS_IsObject(value)) {
        sqlite3_result_error(context, "Value is an object", -1);
        return;
    }
    
    // fallback for unsupported types
    sqlite3_result_error(context, "Unsupported JS value type", -1);
}

static bool js_setup_aggregate (sqlite3_context *context, globaljs_context *js, functionjs_context *fctx, const char *init_code, const char *step_code, const char *final_code, const char *value_code, const char *inverse_code) {
    
    // create a separate context for testing purpose
    JSContext *ctx = JS_NewContext(js->runtime);
    if (!ctx) {
        sqlite3_result_error(context, "Unable to create a JS context.", -1);
        return false;
    }
    
    // init code is optional
    if (init_code) {
        JSValue result = JS_Eval(ctx, init_code, strlen(init_code), NULL, JS_EVAL_TYPE_GLOBAL);
        bool is_error = JS_IsException(result);
        if (is_error) js_error_to_sqlite(context, ctx, result, NULL);
        JS_FreeValue(ctx, result);
        if (is_error) return false;
    }
    
    JSValue step_func = JS_NULL;
    JSValue final_func = JS_NULL;
    JSValue value_func = JS_NULL;
    JSValue inverse_func = JS_NULL;
    
    // generate JavaScript functions
    step_func = JS_Eval(ctx, step_code, strlen(step_code), NULL, JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsFunction(ctx, step_func)) goto cleanup;
    
    final_func = JS_Eval(ctx, final_code, strlen(final_code), NULL, JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsFunction(ctx, final_func)) goto cleanup;
    
    if (value_code) {
        value_func = JS_Eval(ctx, value_code, strlen(value_code), NULL, JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsFunction(ctx, value_func)) goto cleanup;
    }
    
    if (inverse_code) {
        inverse_func = JS_Eval(ctx, inverse_code, strlen(inverse_code), NULL, JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsFunction(ctx, inverse_func)) goto cleanup;
    }
    
    // when here I am sure everything is OK
    if (fctx) {
        // setup a proper function aggregate context
        fctx->agg_ctx = ctx;
        fctx->step_func = step_func;
        fctx->final_func = final_func;
        fctx->value_func = value_func;
        fctx->inverse_func = inverse_func;
        fctx->inited = true;
    }
    return true;
    
cleanup:
    if (!JS_IsFunction(ctx, step_func)) js_error_to_sqlite(context, ctx, step_func, "JavaScript step code must evaluate to a function in the form (function(args){ your_code_here })");
    else if (!JS_IsFunction(ctx, final_func)) js_error_to_sqlite(context, ctx, final_func, "JavaScript final code must evaluate to a function in the form (function(){ your_code_here })");
    else if (!JS_IsFunction(ctx, value_func)) js_error_to_sqlite(context, ctx, final_func, "JavaScript value code must evaluate to a function in the form (function(){ your_code_here })");
    else if (!JS_IsFunction(ctx, inverse_func)) js_error_to_sqlite(context, ctx, step_func, "JavaScript inverse code must evaluate to a function in the form (function(args){ your_code_here })");
    
    
    JS_FreeValue(ctx, step_func);
    JS_FreeValue(ctx, final_func);
    JS_FreeValue(ctx, value_func);
    JS_FreeValue(ctx, inverse_func);
    
    JS_FreeContext(ctx);
    return false;
}

static JSValue sqlite_value_to_js (JSContext *ctx, sqlite3_value *value) {
    int type = sqlite3_value_type(value);
    
    switch (type) {
        case SQLITE_NULL:
            return JS_NULL;
            break;
            
        case SQLITE_INTEGER:
            return JS_NewInt64(ctx, sqlite3_value_int64(value));
            break;
            
        case SQLITE_FLOAT:
            return JS_NewFloat64(ctx, sqlite3_value_double(value));
            break;
            
        case SQLITE_TEXT:
            return JS_NewString(ctx, (const char *)sqlite3_value_text(value));
            break;
            
        case SQLITE_BLOB: {
            int size = sqlite3_value_bytes(value);
            const void *blob = sqlite3_value_blob(value);
            return JS_NewArrayBufferCopy(ctx, (const uint8_t *)blob, (size_t)size);
            }
            break;
    }
    
    return JS_NULL;
}

static const char *sqlite_value_text (sqlite3_value *value) {
    if (sqlite3_value_type(value) != SQLITE_TEXT) return NULL;
    return (const char *)sqlite3_value_text(value);
}

// MARK: - Execution -

static void js_execute_commong (sqlite3_context *context, int nvalues, sqlite3_value **values, JSValue func, JSValue this_obj, bool return_value) {
    functionjs_context *fctx = (functionjs_context *)sqlite3_user_data(context);
    globaljs_context *js = fctx->js_ctx;
    
    JSContext *js_context = (fctx->agg_ctx) ? fctx->agg_ctx : js->context;
    
    // create JS array for arguments
    JSValue args = (values) ? JS_NewArray(js_context) : JS_NULL;
    for (int i=0; i<nvalues; ++i) {
        JSValue js_val = sqlite_value_to_js(js_context, values[i]);
        JS_SetPropertyUint32(js_context, args, i, js_val);
    }
    
    // call the JavaScript function with args_array as the argument
    JSValueConst args_val[] = {args};
    JSValue result = JS_Call(js_context, func, this_obj, (values) ? 1 : 0, (values) ? args_val : NULL);
    JS_FreeValue(js_context, args);
    
    if (return_value) js_value_to_sqlite(context, js_context, result);
    JS_FreeValue(js_context, result);
}

static void js_execute_scalar (sqlite3_context *context, int nvalues, sqlite3_value **values) {
    functionjs_context *fctx = (functionjs_context *)sqlite3_user_data(context);
    js_execute_commong(context, nvalues, values, fctx->step_func, JS_UNDEFINED, true);
}

static void js_execute_step (sqlite3_context *context, int nvalues, sqlite3_value **values) {
    functionjs_context *fctx = (functionjs_context *)sqlite3_user_data(context);
    globaljs_context *js = fctx->js_ctx;
    
    // if there is an init code then create a separate aggregate context
    // to avoid shared state corruption across parallel aggregates
    if (!fctx->inited) {
        // set up a new isolated environment for this aggregation
        if (js_setup_aggregate(context, js, fctx, fctx->init_code, fctx->step_code, fctx->final_code, fctx->value_code, fctx->inverse_code) == false) return;
        fctx->inited = true;
    }
    
    js_execute_commong(context, nvalues, values, fctx->step_func, JS_UNDEFINED, false);
}

static void js_execute_value (sqlite3_context *context) {
    functionjs_context *fctx = (functionjs_context *)sqlite3_user_data(context);
    js_execute_commong(context, 0, NULL, fctx->value_func, JS_UNDEFINED, true);
}

static void js_execute_inverse (sqlite3_context *context, int nvalues, sqlite3_value **values) {
    functionjs_context *fctx = (functionjs_context *)sqlite3_user_data(context);
    js_execute_commong(context, nvalues, values, fctx->inverse_func, JS_UNDEFINED, false);
}

static void js_execute_final (sqlite3_context *context) {
    functionjs_context *fctx = (functionjs_context *)sqlite3_user_data(context);
    js_execute_commong(context, 0, NULL, fctx->final_func, JS_UNDEFINED, true);
    functionjs_free(fctx, false);
    fctx->inited = false;
}

static int js_execute_collation (void *xdata, int len1, const void *v1, int len2, const void *v2) {
    functionjs_context *fctx = (functionjs_context *)xdata;
    globaljs_context *js = fctx->js_ctx;
    
    // create arguments
    JSValue val1 = (v1) ? JS_NewStringLen(js->context, (const char *)v1, (size_t)len1) : JS_NULL;
    JSValue val2 = (v2) ? JS_NewStringLen(js->context, (const char *)v2, (size_t)len2) : JS_NULL;
    JSValue args[] = {val1, val2};
    
    // call the JavaScript function with arguments
    JSValue result = JS_Call(js->context, fctx->step_func, JS_UNDEFINED, 2, args);
    JS_FreeValue(js->context, val1);
    JS_FreeValue(js->context, val2);
    
    // default result if JS result is not a number
    int nresult = -1;
    if (JS_IsNumber(result)) JS_ToInt32(js->context, &nresult, result);
    JS_FreeValue(js->context, result);
    
    return nresult;
}

static void js_execute_cleanup (void *xdata) {
    if (!xdata) return;
    functionjs_free((functionjs_context *)xdata, true);
}

// MARK: - Functions -

void js_create_scalar (sqlite3_context *context, int argc, sqlite3_value **argv) {
    globaljs_context *js = (globaljs_context *)sqlite3_user_data(context);
    
    // get/check parameters first
    const char *name = sqlite_value_text(argv[0]);
    const char *code = sqlite_value_text(argv[1]);
    
    if (name == NULL || code == NULL) {
        sqlite3_result_error(context, "Two parameters of type TEXT are required", -1);
        return;
    }
    
    // create function context
    functionjs_context *fctx = functionjs_init(js, NULL, NULL, NULL, NULL, NULL);
    if (!fctx) {
        sqlite3_result_error_nomem(context);
        return;
    }
    
    // prepare the JavaScript function
    JSValue func = JS_Eval(js->context, code, strlen(code), NULL, JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsFunction(js->context, func)) {
        js_error_to_sqlite(context, js->context, func, "JavaScript code must evaluate to a function in the form (function(args){ your_code_here })");
        functionjs_free(fctx, true);
        return;
    }
    
    // create the SQLite scalar function
    fctx->step_func = func;
    int rc = sqlite3_create_function_v2(sqlite3_context_db_handle(context), name, -1, SQLITE_UTF8, (void *)fctx, js_execute_scalar, NULL, NULL, js_execute_cleanup);
    
    // js_execute_cleanup is automatically called in case of error
    (rc == SQLITE_OK) ? sqlite3_result_int(context, SQLITE_OK) : sqlite3_result_error_code(context, rc);
}

void js_create_aggregate (sqlite3_context *context, int argc, sqlite3_value **argv) {
    globaljs_context *js = (globaljs_context *)sqlite3_user_data(context);
    
    // get/check parameters first
    const char *name = sqlite_value_text(argv[0]);
    const char *init_code = sqlite_value_text(argv[1]);
    const char *step_code = sqlite_value_text(argv[2]);
    const char *final_code = sqlite_value_text(argv[3]);
    
    if (name == NULL || step_code == NULL || final_code == NULL) {
        sqlite3_result_error(context, "The required name, step and final code parameters must be of type TEXT", -1);
        return;
    }
    
    // sanity check aggregate code
    if (js_setup_aggregate(context, js, NULL, init_code, step_code, final_code, NULL, NULL) == false) return;
    
    // create function context
    functionjs_context *fctx = functionjs_init(js, init_code, step_code, final_code, NULL, NULL);
    if (!fctx) {
        sqlite3_result_error_nomem(context);
        return;
    }
     
    // create the SQLite aggregate function
    int rc = sqlite3_create_function_v2(sqlite3_context_db_handle(context), name, -1, SQLITE_UTF8, (void *)fctx, NULL, js_execute_step, js_execute_final, js_execute_cleanup);
    
    // js_execute_cleanup is automatically called in case of error
    (rc == SQLITE_OK) ? sqlite3_result_int(context, SQLITE_OK) : sqlite3_result_error_code(context, rc);
}

void js_create_window (sqlite3_context *context, int argc, sqlite3_value **argv) {
    globaljs_context *js = (globaljs_context *)sqlite3_user_data(context);
    
    // get/check parameters first
    const char *name = sqlite_value_text(argv[0]);
    const char *init_code = sqlite_value_text(argv[1]);
    const char *step_code = sqlite_value_text(argv[2]);
    const char *final_code = sqlite_value_text(argv[3]);
    const char *value_code = sqlite_value_text(argv[4]);
    const char *inverse_code = sqlite_value_text(argv[5]);
    
    if (name == NULL || step_code == NULL || final_code == NULL || value_code == NULL || inverse_code == NULL) {
        sqlite3_result_error(context, "The required name, step, final, value and inverse code parameters must be of type TEXT", -1);
        return;
    }
    
    // sanity check aggregate code
    if (js_setup_aggregate(context, js, NULL, init_code, step_code, final_code, value_code, inverse_code) == false) return;
    
    // create function context
    functionjs_context *fctx = functionjs_init(js, init_code, step_code, final_code, value_code, inverse_code);
    if (!fctx) {
        sqlite3_result_error_nomem(context);
        return;
    }
    
    // create the SQLite window function
    int rc = sqlite3_create_window_function(sqlite3_context_db_handle(context), name, -1, SQLITE_UTF8, (void *)fctx, js_execute_step, js_execute_final, js_execute_value, js_execute_inverse, js_execute_cleanup);
    
    // js_execute_cleanup is automatically called in case of error
    (rc == SQLITE_OK) ? sqlite3_result_int(context, SQLITE_OK) : sqlite3_result_error_code(context, rc);
}

void js_create_collation (sqlite3_context *context, int argc, sqlite3_value **argv) {
    globaljs_context *js = (globaljs_context *)sqlite3_user_data(context);
    
    // get/check parameters first
    const char *name = sqlite_value_text(argv[0]);
    const char *code = sqlite_value_text(argv[1]);
    
    if (name == NULL || code == NULL) {
        sqlite3_result_error(context, "Two parameters of type TEXT are required", -1);
        return;
    }
    
    // create function context
    functionjs_context *fctx = functionjs_init(js, NULL, NULL, NULL, NULL, NULL);
    if (!fctx) {
        sqlite3_result_error_nomem(context);
        return;
    }
    
    // prepare the JavaScript function
    JSValue func = JS_Eval(js->context, code, strlen(code), NULL, JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsFunction(js->context, func)) {
        js_error_to_sqlite(context, js->context, func, "JavaScript code must evaluate to a function in the form (function(str1, str2){ your_code_here })");
        functionjs_free(fctx, true);
        return;
    }
    
    // create the SQLite function
    fctx->step_func = func;
    int rc = sqlite3_create_collation_v2(sqlite3_context_db_handle(context), name, SQLITE_UTF8, (void *)fctx, js_execute_collation, js_execute_cleanup);
    
    // js_execute_cleanup is automatically called in case of error
    (rc == SQLITE_OK) ? sqlite3_result_int(context, SQLITE_OK) : sqlite3_result_error_code(context, rc);
}

void js_eval (sqlite3_context *context, int argc, sqlite3_value **argv) {
    globaljs_context *data = (globaljs_context *)sqlite3_user_data(context);
    
    const char *code = (const char *)sqlite3_value_text(argv[0]);
    if (!code) {
        sqlite3_result_error(context, "A parameter of type TEXT is required", -1);
        return;
    }
    
    JSValue value = JS_Eval(data->context, code, strlen(code), NULL, JS_EVAL_TYPE_GLOBAL);
    js_value_to_sqlite(context, data->context, value);
    JS_FreeValue(data->context, value);
}

static void js_load_file (sqlite3_context *context, int argc, sqlite3_value **argv, bool is_blob) {
    const char *path = (const char *)sqlite3_value_text(argv[0]);
    if (!path) {
        sqlite3_result_error(context, "A parameter of type TEXT is required", -1);
        return;
    }
    
    FILE *f = fopen(path, (is_blob) ? "rb": "r");
    if (!f) {
        sqlite3_result_error(context, "Unable to open the file", -1);
        return;
    }
    
    fseek(f, 0, SEEK_END);
    size_t length = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buffer = malloc(length);
    if (!buffer) {
        fclose(f);
        sqlite3_result_error_nomem(context);
        return;
    }
    
    size_t nread = fread(buffer, length, 1, f);
    if (nread == length) {
        (is_blob) ? sqlite3_result_blob(context, buffer, (int)length, free) : sqlite3_result_text(context, buffer, (int)length, free);
    } else {
        sqlite3_result_error(context, "Unable to correctly read the file", -1);
        if (buffer) free(buffer);
    }
    
    fclose(f);
}

void js_load_text (sqlite3_context *context, int argc, sqlite3_value **argv) {
    js_load_file(context, argc, argv, false);
}

void js_load_blob (sqlite3_context *context, int argc, sqlite3_value **argv) {
    js_load_file(context, argc, argv, true);
}

void js_version (sqlite3_context *context, int argc, sqlite3_value **argv) {
    sqlite3_result_text(context, gversion, -1, NULL);
}

// MARK: -

const char *sqlitejs_version (void) {
    return gversion;
}

APIEXPORT int sqlite3_sqlitejs_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    #ifndef SQLITE_CORE
    SQLITE_EXTENSION_INIT2(pApi);
    #endif
    
    // setup version
    compute_version_string();
    
    globaljs_context *data = globaljs_init(db);
    if (!data) return SQLITE_NOMEM;
    
    const char *f_name[] = {"js_version", "js_create_scalar", "js_create_aggregate", "js_create_window", "js_create_collation", "js_eval", "js_load_text", "js_load_blob"};
    const void *f_ptr[] = {js_version, js_create_scalar, js_create_aggregate, js_create_window, js_create_collation, js_eval, js_load_text, js_load_blob};
    int f_arg[] = {0, 2, 4, 6, 2, 1, 1, 1};
    
    size_t f_count = sizeof(f_name) / sizeof(const char *);
    for (size_t i=0; i<f_count; ++i) {
        int rc = sqlite3_create_function_v2(db, f_name[i], f_arg[i], SQLITE_UTF8, (void *)data, f_ptr[i], NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("Error creating function %s: %s", f_name[i], sqlite3_errmsg(db));
            return rc;
        }
    }
    
    return SQLITE_OK;
}
