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
#include "quickjs-libc.h"

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
    JSClassID           rowSetClassID;
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

static char *sqlite_strdup (const char *str);
static bool js_global_init (JSContext *ctx, globaljs_context *js);
static JSValue sqlite_value_to_js (JSContext *ctx, sqlite3_value *value);

#define FUNCTION_TYPE_SCALAR            "scalar"
#define FUNCTION_TYPE_WINDOW            "window"
#define FUNCTION_TYPE_AGGREGATE         "aggregate"
#define FUNCTION_TYPE_COLLATION         "collation"

#define SAFE_STRCMP(a,b)                (((a) != (b)) && ((a) == NULL || (b) == NULL || strcmp((a), (b)) != 0))

// MARK: - RowSet -

typedef struct {
    int             ncols;
    sqlite3_stmt    *vm;
} rowset;

static void js_rowset_finalizer(JSRuntime *rt, JSValue val);
static JSValue js_rowset_next(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_rowset_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_rowset_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_rowset_to_array(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

// Define the Rowset class
static const JSClassDef js_rowset_class = {
    "Rowset",
    .finalizer = js_rowset_finalizer,
};

// Define the Rowset prototype with methods
static const JSCFunctionListEntry js_rowset_proto_funcs[] = {
    JS_CFUNC_DEF("next", 0, js_rowset_next),
    JS_CFUNC_DEF("get", 1, js_rowset_get),
    JS_CFUNC_DEF("name", 1, js_rowset_name),
    JS_CFUNC_DEF("toArray", 0, js_rowset_to_array),
};

static JSValue js_rowset_next(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    globaljs_context *js = JS_GetContextOpaque(ctx);
    rowset *rs = JS_GetOpaque(this_val, js->rowSetClassID);
    if (!rs) return JS_EXCEPTION;
    
    if (sqlite3_step(rs->vm) == SQLITE_ROW) return JS_TRUE;
    
    sqlite3_finalize(rs->vm);
    rs->vm = NULL;
    return JS_FALSE;
}

static JSValue js_rowset_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    globaljs_context *js = JS_GetContextOpaque(ctx);
    rowset *rs = JS_GetOpaque(this_val, js->rowSetClassID);
    if (!rs) return JS_EXCEPTION;
    
    uint32_t index = 0;
    JS_ToUint32(ctx, &index, argv[0]);
    if (index >= rs->ncols) return JS_EXCEPTION;
    
    sqlite3_value *value = sqlite3_column_value(rs->vm, (int)index);
    return sqlite_value_to_js(ctx, value);
}

static JSValue js_rowset_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    globaljs_context *js = JS_GetContextOpaque(ctx);
    rowset *rs = JS_GetOpaque(this_val, js->rowSetClassID);
    if (!rs) return JS_EXCEPTION;
    
    uint32_t index = 0;
    JS_ToUint32(ctx, &index, argv[0]);
    if (index >= rs->ncols) return JS_EXCEPTION;
    
    const char *name = sqlite3_column_name(rs->vm, (int)index);
    if (!name) return JS_EXCEPTION;
    
    return JS_NewString(ctx, name);
}

static JSValue js_rowset_to_array(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    globaljs_context *js = JS_GetContextOpaque(ctx);
    rowset *rs = JS_GetOpaque(this_val, js->rowSetClassID);
    if (!rs) return JS_EXCEPTION;
    
    JSValue result = JS_NewArray(ctx);
    if (JS_IsException(result)) return JS_EXCEPTION;
    
    int ncols = rs->ncols;
    int nrows = 0;
    while (true) {
        int rc = sqlite3_step(rs->vm);
        if (rc != SQLITE_ROW) break;
        
        JSValue row = JS_NewArray(ctx);
        if (JS_IsException(row)) return JS_EXCEPTION;
        
        for (int i=0; i<ncols; ++i) {
            sqlite3_value *value = sqlite3_column_value(rs->vm, i);
            JSValue jvalue = sqlite_value_to_js(ctx, value);
            JS_SetPropertyUint32(ctx, row, i, jvalue);
        }
        
        JS_SetPropertyUint32(ctx, result, nrows++, row);
    }
    
    sqlite3_finalize(rs->vm);
    rs->vm = NULL;
    return result;
}

static void js_rowset_finalizer(JSRuntime *rt, JSValue val) {
    rowset *rs = (rowset *)JS_VALUE_GET_PTR(val);
    if (!rs) return;
    
    if (rs->vm) sqlite3_finalize(rs->vm);
    sqlite3_free(rs);
}

// MARK: - Initializer -

static globaljs_context *globaljs_init (sqlite3 *db) {
    JSRuntime *rt = NULL;
    JSContext *ctx = NULL;
    
    globaljs_context *js = (globaljs_context *)sqlite3_malloc(sizeof(globaljs_context));
    if (!js) return NULL;
    bzero(js, sizeof(globaljs_context));
    
    rt = JS_NewRuntime();
    if (!rt) goto abort_init;
    
    //JS_SetMemoryLimit(rt, 80 * 1024);
    //JS_SetMaxStackSize(rt, 10 * 1024);
    
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc(rt, NULL, js_module_loader, NULL);
    
    ctx = JS_NewContext(rt);
    if (!ctx) goto abort_init;
    
    js->db = db;
    js->runtime = rt;
    js->context = ctx;
    
    JS_SetContextOpaque(ctx, js);
    js_global_init(ctx, js);
    
    return js;
    
abort_init:
    if (rt) JS_FreeRuntime(rt);
    if (ctx) JS_FreeContext(ctx);
    if (js) sqlite3_free(js);
    return NULL;
}

static void globaljs_free (globaljs_context *js) {
    if (!js) return;
    
    if (js->runtime) JS_FreeRuntime(js->runtime);
    if (js->context) JS_FreeContext(js->context);
    sqlite3_free(js);
}

static functionjs_context *functionjs_init (globaljs_context *jsctx, const char *init_code, const char *step_code, const char *final_code, const char *value_code, const char *inverse_code) {
    // make a copy of all the code
    functionjs_context *fctx = NULL;
    char *init_code_copy = NULL;
    char *step_code_copy = NULL;
    char *final_code_copy = NULL;
    char *value_code_copy = NULL;
    char *inverse_code_copy = NULL;
    
    fctx = (functionjs_context *)sqlite3_malloc(sizeof(functionjs_context));
    if (!fctx) goto cleanup;
    bzero(fctx, sizeof(functionjs_context));
    
    if (init_code) {
        init_code_copy = sqlite_strdup(init_code);
        if (!init_code_copy) goto cleanup;
    }
    
    if (step_code) {
        step_code_copy = sqlite_strdup(step_code);
        if (!step_code_copy) goto cleanup;
    }
    
    if (final_code) {
        final_code_copy = sqlite_strdup(final_code);
        if (!final_code_copy) goto cleanup;
    }
    
    if (value_code) {
        value_code_copy = sqlite_strdup(value_code);
        if (!value_code_copy) goto cleanup;
    }
    
    if (inverse_code) {
        inverse_code_copy = sqlite_strdup(inverse_code);
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
    if (init_code_copy) sqlite3_free(init_code_copy);
    if (step_code_copy) sqlite3_free(step_code_copy);
    if (final_code_copy) sqlite3_free(final_code_copy);
    if (value_code_copy) sqlite3_free(value_code_copy);
    if (inverse_code_copy) sqlite3_free(inverse_code_copy);
    if (fctx) sqlite3_free(fctx);
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
        if (fctx->init_code) sqlite3_free((void *)fctx->init_code);
        if (fctx->step_code) sqlite3_free((void *)fctx->step_code);
        if (fctx->final_code) sqlite3_free((void *)fctx->final_code);
        if (fctx->value_code) sqlite3_free((void *)fctx->value_code);
        if (fctx->inverse_code) sqlite3_free((void *)fctx->inverse_code);
        sqlite3_free(fctx);
    }
}

// MARK: - Utils -

static JSValue js_sqlite_exec (JSContext *ctx, sqlite3 *db, const char *sql, int argc, JSValueConst *argv) {
    sqlite3_stmt *vm = NULL;
    const char *tail = NULL;
    rowset *rs = NULL;
    
    // compile statement
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, &tail);
    if (rc != SQLITE_OK) goto abort_with_dberror;
    
    // count if statement contains bindings
    int nbind = sqlite3_bind_parameter_count(vm);
    if (nbind > 0 && argc > 0) {
        // loop to bind
        if (nbind > argc) nbind = argc;
        for (int i=1; i<=nbind; ++i) {
            
        }
    }
    
    // create and initialize internal rowset
    rs = (rowset *)sqlite3_malloc(sizeof(rowset));
    if (!rs) goto abort_with_dberror;
    rs->vm = vm;
    rs->ncols = sqlite3_column_count(vm);
    
    // create Rowset JS object
    globaljs_context *js = JS_GetContextOpaque(ctx);    
    JSValue obj = JS_NewObjectClass(ctx, js->rowSetClassID);
    if (JS_IsException(obj)) goto abort_with_jserror;
    JS_SetOpaque(obj, rs);
    JS_SetPropertyStr(ctx, obj, "columnCount", JS_NewInt32(ctx, rs->ncols));
    
    return obj;
    
abort_with_dberror:
    if (rs) {
        if (rs->vm) rs->vm = NULL;
        sqlite3_free(rs);
    }
    if (vm) sqlite3_finalize(vm);
    return JS_ThrowInternalError(ctx, "%s", sqlite3_errmsg(db));
    
abort_with_jserror:
    if (rs) {
        if (rs->vm) rs->vm = NULL;
        sqlite3_free(rs);
    }
    if (vm) sqlite3_finalize(vm);
    return obj;
}

static JSValue js_dbfunc_exec (JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    // check if we have at least one argument
    if (argc < 1) return JS_EXCEPTION;
    
    // get the SQL string from the first argument
    size_t len;
    const char *sql = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!sql) return JS_EXCEPTION;
    
    // perform statement
    globaljs_context *js = JS_GetContextOpaque(ctx);
    JSValue value = js_sqlite_exec(ctx, js->db, sql, argc-1, argv);
    
    // free the string when done
    JS_FreeCString(ctx, sql);
    
    return value;
}

static bool js_global_init (JSContext *ctx, globaljs_context *js) {
    js_std_add_helpers(ctx, 0, NULL);
    
    // add any global objects or functions here
    JSValue global_obj = JS_GetGlobalObject(ctx);
    
    // create a new db object
    JSValue db_obj =  JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, db_obj, "exec", JS_NewCFunction(ctx, js_dbfunc_exec, "exec", 1));
    JS_SetPropertyStr(ctx, global_obj, "db", db_obj);
    
    // register rowset class
    JS_NewClassID(js->runtime, &js->rowSetClassID);
    JS_NewClass(js->runtime, js->rowSetClassID, &js_rowset_class);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_rowset_proto_funcs, sizeof(js_rowset_proto_funcs)/sizeof(js_rowset_proto_funcs[0]));
    JS_SetClassProto(ctx, js->rowSetClassID, proto);
    
    // register standard modules
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    js_init_module_bjson(ctx, "bjson");
    
    // release the global object reference
    JS_FreeValue(ctx, global_obj);
    
    // we don't free db_obj because it's now owned by the global object
    return true;
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

static bool js_value_to_sqlite (sqlite3_context *context, JSContext *js_ctx, JSValue value) {
    // check for exceptions first and convert it to a proper error message (if any)
    if (JS_IsException(value)) {
        js_error_to_sqlite(context, js_ctx, value, NULL);
        return false;
    }
    
    // check for null or undefined
    if (JS_IsNull(value) || JS_IsUndefined(value)) {
        sqlite3_result_null(context);
        return false;
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
        return false;
    }
    
    // handle booleans
    if (JS_IsBool(value)) {
        sqlite3_result_int(context, JS_ToBool(js_ctx, value));
        return false;
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
        return false;
    }
    
    if (JS_IsObject(value)) {
        sqlite3_result_null(context);
        return true;
    }
    
    // fallback for unsupported types
    sqlite3_result_error(context, "Unsupported JS value type", -1);
    return false;
}

static bool js_setup_aggregate (sqlite3_context *context, globaljs_context *js, functionjs_context *fctx, const char *init_code, const char *step_code, const char *final_code, const char *value_code, const char *inverse_code) {
    
    // create a separate context for testing purpose
    JSContext *ctx = JS_NewContext(js->runtime);
    if (!ctx) {
        sqlite3_result_error(context, "Unable to create a JS context.", -1);
        return false;
    }
    
    // setup global object
    JS_SetContextOpaque(ctx, js);
    js_global_init(ctx, js);
    
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

static char *sqlite_strdup (const char *str) {
    if (!str) return NULL;
    
    size_t len = strlen(str) + 1;
    char *result = (char*)sqlite3_malloc((int)len);
    if (result) memcpy(result, str, len);
    
    return result;
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
    
    bool is_object = false;
    if (return_value) is_object = js_value_to_sqlite(context, js_context, result);
    if (!is_object) JS_FreeValue(js_context, result);
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

void js_version (sqlite3_context *context, bool internal_engine) {
    sqlite3_result_text(context, (internal_engine) ? quickjs_version() : sqlitejs_version(), -1, NULL);
}

void js_version1 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    bool internal_engine = (sqlite3_value_int(argv[0]) != 0);
    js_version(context, internal_engine);
}

void js_version0 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    js_version(context, false);
}

bool js_add_to_table (sqlite3_context *context, const char *type, const char *name, const char *init_code, const char *step_code, const char *final_code, const char *value_code, const char *inverse_code) {
    
    // add function to table under the following conditions:
    // 1. js_functions table exists
    // 2. the same function with the same code is not already in the table (we prevented replacing it with the same values to avoid unnecessary CRDT operations in case this table is synced with sqlite-sync)
    sqlite3 *db = sqlite3_context_db_handle(context);
    bool force_reinsert = false;
    sqlite3_stmt *vm = NULL;
    
    // query table first
    const char *sql = "SELECT kind,init_code,step_code,final_code,value_code,inverse_code FROM js_functions WHERE name=?1 LIMIT 1;";
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) {
        // table js_functions does not exist
        sqlite3_finalize(vm);
        return true;
    }
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) {
        // no functions with that name exist, so add it to the table
        force_reinsert = true;
        goto insert_function;
    } if (rc != SQLITE_ROW) {
        // something bad happened, just abort
        sqlite3_finalize(vm);
        return false;
    }
    
    const char *type2 = (const char *)sqlite3_column_text(vm, 0);
    const char *init_code2 = (sqlite3_column_type(vm, 1) == SQLITE_NULL) ? NULL : (const char *)sqlite3_column_text(vm, 1);
    const char *step_code2 = (sqlite3_column_type(vm, 2) == SQLITE_NULL) ? NULL : (const char *)sqlite3_column_text(vm, 2);
    const char *final_code2 = (sqlite3_column_type(vm, 3) == SQLITE_NULL) ? NULL : (const char *)sqlite3_column_text(vm, 3);
    const char *value_code2 = (sqlite3_column_type(vm, 4) == SQLITE_NULL) ? NULL : (const char *)sqlite3_column_text(vm, 4);
    const char *inverse_code2 = (sqlite3_column_type(vm, 5) == SQLITE_NULL) ? NULL : (const char *)sqlite3_column_text(vm, 5);
    
    if ((strcasecmp(type, type2) != 0) ||
        SAFE_STRCMP(init_code, init_code2) ||
        SAFE_STRCMP(step_code, step_code2) ||
        SAFE_STRCMP(final_code, final_code2) ||
        SAFE_STRCMP(value_code, value_code2) ||
        SAFE_STRCMP(inverse_code, inverse_code2)) force_reinsert = true;
    
    // the following logic:
    // if ((init_code == NULL) && (init_code2 != NULL)) force_reinsert = true;
    // if ((init_code != NULL) && (init_code2 == NULL)) force_reinsert = true;
    // if ((init_code != NULL) && (init_code2 != NULL) && (strcmp(init_code, init_code2) != 0)) force_reinsert = true;
    // can be simplified as:
    // if ((init_code != init_code2) && (init_code == NULL || init_code2 == NULL || strcmp(init_code, init_code2) != 0)) force_reinsert = true;
    // as a macro:
    //  SAFE_STRCMP(a,b) (((a) != (b)) && ((a) == NULL || (b) == NULL || strcmp((a), (b)) != 0))
    
    // we use strcmp because even if a single character case changes, we force the update of that function
    
insert_function:
    sqlite3_finalize(vm);
    if (force_reinsert == false) return true;
    
    sql = "REPLACE INTO js_functions (name, kind, init_code, step_code, final_code, value_code, inverse_code) VALUES (?, ?, ?, ?, ?, ?, ?);";
    rc = sqlite3_prepare(db, sql, -1, &vm, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_text(vm, 1, name, -1, NULL);
        rc = sqlite3_bind_text(vm, 2, type, -1, NULL);
        rc = (init_code == NULL) ? sqlite3_bind_null(vm, 3) : sqlite3_bind_text(vm, 3, init_code, -1, NULL);
        rc = (step_code == NULL) ? sqlite3_bind_null(vm, 4) : sqlite3_bind_text(vm, 4, step_code, -1, NULL);
        rc = (final_code == NULL) ? sqlite3_bind_null(vm, 5) : sqlite3_bind_text(vm, 5, final_code, -1, NULL);
        rc = (value_code == NULL) ? sqlite3_bind_null(vm, 6) : sqlite3_bind_text(vm, 6, value_code, -1, NULL);
        rc = (inverse_code == NULL) ? sqlite3_bind_null(vm, 7) : sqlite3_bind_text(vm, 7, inverse_code, -1, NULL);
    }
    
    rc = sqlite3_step(vm);
    sqlite3_finalize(vm);
    
    return (rc == SQLITE_DONE);
}

bool js_create_common (sqlite3_context *context, const char *type, const char *name, const char *init_code, const char *step_code, const char *final_code, const char *value_code, const char *inverse_code, bool is_load) {
    
    globaljs_context *js = (globaljs_context *)sqlite3_user_data(context);
    
    bool is_scalar = (strcasecmp(type, FUNCTION_TYPE_SCALAR) == 0);
    bool is_aggregate = (is_scalar) ? false : (strcasecmp(type, FUNCTION_TYPE_AGGREGATE) == 0);
    bool is_window = (is_aggregate) ? false : (strcasecmp(type, FUNCTION_TYPE_WINDOW) == 0);
    bool is_collation = (is_window) ? false : (strcasecmp(type, FUNCTION_TYPE_COLLATION) == 0);
    bool step_code_null = (is_scalar || is_collation);
    
    if (is_aggregate || is_window) {
        // sanity check aggregate code
        if (js_setup_aggregate(context, js, NULL, init_code, step_code, final_code, NULL, NULL) == false) return false;
    }
    
    // create function context
    functionjs_context *fctx = functionjs_init(js, init_code, (step_code_null) ? NULL : step_code, final_code, value_code, inverse_code);
    if (!fctx) {
        sqlite3_result_error_nomem(context);
        return false;
    }
    
    if (is_scalar || is_collation) {
        // prepare the JavaScript function
        JSValue func = JS_Eval(js->context, step_code, strlen(step_code), NULL, JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsFunction(js->context, func)) {
            const char *err_msg = (is_scalar) ? "JavaScript code must evaluate to a function in the form (function(args){ your_code_here })" : "JavaScript code must evaluate to a function in the form (function(str1, str2){ your_code_here })";
            js_error_to_sqlite(context, js->context, func, err_msg);
            functionjs_free(fctx, true);
            return false;
        }
        fctx->step_func = func;
    }
    
    int rc = SQLITE_OK;
    if (is_scalar) rc = sqlite3_create_function_v2(sqlite3_context_db_handle(context), name, -1, SQLITE_UTF8, (void *)fctx, js_execute_scalar, NULL, NULL, js_execute_cleanup);
    else if (is_aggregate) rc = sqlite3_create_function_v2(sqlite3_context_db_handle(context), name, -1, SQLITE_UTF8, (void *)fctx, NULL, js_execute_step, js_execute_final, js_execute_cleanup);
    else if (is_window) rc = sqlite3_create_window_function(sqlite3_context_db_handle(context), name, -1, SQLITE_UTF8, (void *)fctx, js_execute_step, js_execute_final, js_execute_value, js_execute_inverse, js_execute_cleanup);
    else if (is_collation) rc = sqlite3_create_collation_v2(sqlite3_context_db_handle(context), name, SQLITE_UTF8, (void *)fctx, js_execute_collation, js_execute_cleanup);
    
    if (rc == SQLITE_BUSY) {
        // Due to this: https://www3.sqlite.org/src/info/cabab62bc10568d4
        // it is not possible to update or delete a previously registered function
        // within the same database connection.
        // There is nothing we can do because this is an SQLite implementation detail.
        // Function updates must be performed using a separate database connection.
        sqlite3_result_error(context, "Function updates must be performed using a separate database connection.", -1);
    }
    
    if ((is_load == false) && (rc == SQLITE_OK)) {
        js_add_to_table(context, type, name, init_code, step_code, final_code, value_code, inverse_code);
    }
    
    // js_execute_cleanup is automatically called in case of error
    (rc == SQLITE_OK) ? sqlite3_result_int(context, SQLITE_OK) : sqlite3_result_error_code(context, rc);
    return (rc == SQLITE_OK);
}

void js_create_scalar (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // get/check parameters first
    const char *name = sqlite_value_text(argv[0]);
    const char *code = sqlite_value_text(argv[1]);
    
    if (name == NULL || code == NULL) {
        sqlite3_result_error(context, "Two parameters of type TEXT are required", -1);
        return;
    }
    
    js_create_common(context, FUNCTION_TYPE_SCALAR, name, NULL, code, NULL, NULL, NULL, false);
}

void js_create_aggregate (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // get/check parameters first
    const char *name = sqlite_value_text(argv[0]);
    const char *init_code = sqlite_value_text(argv[1]);
    const char *step_code = sqlite_value_text(argv[2]);
    const char *final_code = sqlite_value_text(argv[3]);
    
    if (name == NULL || step_code == NULL || final_code == NULL) {
        sqlite3_result_error(context, "The required name, step and final code parameters must be of type TEXT", -1);
        return;
    }
    
    js_create_common(context, FUNCTION_TYPE_AGGREGATE, name, init_code, step_code, final_code, NULL, NULL, false);
}

void js_create_window (sqlite3_context *context, int argc, sqlite3_value **argv) {
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
    
    js_create_common(context, FUNCTION_TYPE_WINDOW, name, init_code, step_code, final_code, value_code, inverse_code, false);
}

void js_create_collation (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // get/check parameters first
    const char *name = sqlite_value_text(argv[0]);
    const char *code = sqlite_value_text(argv[1]);
    
    if (name == NULL || code == NULL) {
        sqlite3_result_error(context, "Two parameters of type TEXT are required", -1);
        return;
    }
    
    js_create_common(context, FUNCTION_TYPE_COLLATION, name, NULL, code, NULL, NULL, NULL, false);
}

void js_eval (sqlite3_context *context, int argc, sqlite3_value **argv) {
    globaljs_context *data = (globaljs_context *)sqlite3_user_data(context);
    
    const char *code = (const char *)sqlite3_value_text(argv[0]);
    if (!code) {
        sqlite3_result_error(context, "A parameter of type TEXT is required", -1);
        return;
    }
    
    JSValue value = JS_Eval(data->context, code, strlen(code), NULL, JS_EVAL_TYPE_GLOBAL);
    bool is_object = js_value_to_sqlite(context, data->context, value);
    if (!is_object) JS_FreeValue(data->context, value);
}

static void js_load_fromfile (sqlite3_context *context, int argc, sqlite3_value **argv, bool is_blob) {
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
    
    char *buffer = (char *)sqlite3_malloc((int)length);
    if (!buffer) {
        fclose(f);
        sqlite3_result_error_nomem(context);
        return;
    }
    
    size_t nread = fread(buffer, length, 1, f);
    if (nread == length) {
        (is_blob) ? sqlite3_result_blob(context, buffer, (int)length, sqlite3_free) : sqlite3_result_text(context, buffer, (int)length, sqlite3_free);
    } else {
        sqlite3_result_error(context, "Unable to correctly read the file", -1);
        if (buffer) sqlite3_free(buffer);
    }
    
    fclose(f);
}

void js_load_text (sqlite3_context *context, int argc, sqlite3_value **argv) {
    js_load_fromfile(context, argc, argv, false);
}

void js_load_blob (sqlite3_context *context, int argc, sqlite3_value **argv) {
    js_load_fromfile(context, argc, argv, true);
}

int js_load_from_table_callback (void *xdata, int ncols, char **values, char **names) {
    sqlite3_context *context = (sqlite3_context *)xdata;
    assert(ncols == 7);
    
    const char *type = values[1];
    
    const char *name = values[0];
    const char *init_code = values[2];
    const char *step_code = values[3];
    const char *final_code = values[4];
    const char *value_code = values[5];
    const char *inverse_code = values[6];
    
    bool result = js_create_common(context, type, name, init_code, step_code, final_code, value_code, inverse_code, true);
    return (result) ? SQLITE_OK : SQLITE_ERROR;
}

int js_load_from_table (sqlite3_context *context) {
    sqlite3 *db = sqlite3_context_db_handle(context);
    const char *sql = "SELECT * FROM js_functions;";
    return sqlite3_exec(db, sql, js_load_from_table_callback, context, NULL);
}

void js_init_table (sqlite3_context *context, bool load_functions) {
    sqlite3 *db = sqlite3_context_db_handle(context);
    const char *sql = "CREATE TABLE IF NOT EXISTS js_functions ("
    "name TEXT PRIMARY KEY COLLATE NOCASE," // Name of the SQLite function or collation
    "kind TEXT NOT NULL,"                   // 'scalar', 'aggregate', 'window', 'collation'
    "init_code TEXT DEFAULT NULL,"          // Only for aggregate/window
    "step_code TEXT DEFAULT NULL,"          // Used in all functions
    "final_code TEXT DEFAULT NULL,"         // Only for aggregate/window
    "value_code TEXT DEFAULT NULL,"         // Only for window
    "inverse_code TEXT DEFAULT NULL"        // Only for window
    ");";
    
    // create table
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_result_error_code(context, rc);
        return;
    }
    
    // load js functions from table
    if (load_functions) rc = js_load_from_table(context);
    
    sqlite3_result_int(context, rc);
}

void js_init_table1 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    bool load_functions = (sqlite3_value_int(argv[0]) != 0);
    js_init_table(context, load_functions);
}

void js_init_table0 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    js_init_table(context, false);
}

// MARK: -

const char *sqlitejs_version (void) {
    return SQLITE_JS_VERSION;
}

const char *quickjs_version (void) {
    return JS_GetVersion();
}

APIEXPORT int sqlite3_js_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    #ifndef SQLITE_CORE
    SQLITE_EXTENSION_INIT2(pApi);
    #endif
    
    globaljs_context *data = globaljs_init(db);
    if (!data) return SQLITE_NOMEM;
    
    const char *f_name[] = {"js_version", "js_version", "js_create_scalar", "js_create_aggregate", "js_create_window", "js_create_collation", "js_eval", "js_load_text", "js_load_blob", "js_init_table", "js_init_table"};
    const void *f_ptr[] = {js_version0, js_version1, js_create_scalar, js_create_aggregate, js_create_window, js_create_collation, js_eval, js_load_text, js_load_blob, js_init_table0, js_init_table1};
    int f_arg[] = {0, 1, 2, 4, 6, 2, 1, 1, 1, 0, 1};
    
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
