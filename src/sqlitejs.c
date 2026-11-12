//
//  sqlitejs.c
//  sqlitejs
//
//  Created by Marco Bambini on 31/03/25.
//

// glibc declares pthread_getattr_np() (see js_stack_room) only for _GNU_SOURCE
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <math.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(_WIN32) || defined(__linux__) || defined(__ANDROID__) || defined(__GLIBC__)
#include <malloc.h>
#endif
#include "sqlitejs.h"
#include "quickjs.h"
#include "quickjs-libc.h"

#if defined(_WIN32)
#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

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
    int                 ref_count;
    JSValue             typed_array_byte_offset;    // the getters of every typed array's own
    JSValue             typed_array_byte_length;    // offset and length: see js_get_bytes()
    size_t              memory_limit;   // in bytes, 0 for none: see js_set_memory_limit()
    size_t              gc_threshold;   // QuickJS's GC threshold when last checked: see js_keep_gc_below_limit()
    size_t              mem_used;       // memory the runtime has allocated: see js_mem_malloc()
    size_t              mem_peak;       // the most in use since the last result or error
} globaljs_context;

typedef struct {
    globaljs_context    *js_ctx;        // never to release
    
    const char          *init_code;     // release only if complete (aggregate functions only)
    const char          *step_code;     // release only if complete (scalar, collation, windows and aggregate functions)
    const char          *final_code;    // release only if complete (windows and aggregate functions)
    const char          *value_code;    // release only if complete (window functions only)
    const char          *inverse_code;  // release only if complete (window functions only)
    
    JSValue             func;      // to release (scalar, collation)
} functionjs_context;

typedef struct {
    JSContext           *context;       // to release (windows and aggregate functions)
    JSValue             step_func;      // to release (scalar, collation, windows and aggregate functions)
    JSValue             final_func;     // to release (windows and aggregate functions)
    JSValue             value_func;     // to release (window functions only)
    JSValue             inverse_func;   // to release (window functions only)
} functionjs_aggregate_context;

static char *sqlite_strdup (const char *str);
static bool js_global_init (JSContext *ctx, globaljs_context *js);
static JSValue sqlite_value_to_js (JSContext *ctx, sqlite3_value *value);

#define FUNCTION_TYPE_SCALAR            "scalar"
#define FUNCTION_TYPE_WINDOW            "window"
#define FUNCTION_TYPE_AGGREGATE         "aggregate"
#define FUNCTION_TYPE_COLLATION         "collation"

#define SAFE_STRCMP(a,b)                (((a) != (b)) && ((a) == NULL || (b) == NULL || strcmp((a), (b)) != 0))
#define UNUSED(x)                       (void)(x)

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
    UNUSED(argc); UNUSED(argv);
    globaljs_context *js = JS_GetContextOpaque(ctx);
    rowset *rs = JS_GetOpaque(this_val, js->rowSetClassID);
    if (!rs) return JS_ThrowTypeError(ctx, "Not a Rowset object");
    
    if (!rs->vm) return JS_FALSE;
    int rc = sqlite3_step(rs->vm);
    if (rc == SQLITE_ROW) return JS_TRUE;

    // an error while the statement runs (a constraint, an overflow, an interrupt) is an
    // exception, as an error while it is prepared is in js_sqlite_exec; only the end of the
    // rows is false
    JSValue result = (rc == SQLITE_DONE) ? JS_FALSE : JS_ThrowInternalError(ctx, "%s", sqlite3_errmsg(sqlite3_db_handle(rs->vm)));
    sqlite3_finalize(rs->vm);
    rs->vm = NULL;
    return result;
}

static JSValue js_rowset_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    UNUSED(argc);
    globaljs_context *js = JS_GetContextOpaque(ctx);
    rowset *rs = JS_GetOpaque(this_val, js->rowSetClassID);
    if (!rs) return JS_ThrowTypeError(ctx, "Not a Rowset object");
    
    // never return JS_EXCEPTION without throwing something: QuickJS would propagate a
    // null exception and the caller would only get a generic error message
    if (!rs->vm) return JS_ThrowInternalError(ctx, "No row available, call next() first");
    uint32_t index = 0;
    if (JS_ToUint32(ctx, &index, argv[0])) return JS_EXCEPTION;
    if (index >= (uint32_t)rs->ncols) return JS_ThrowRangeError(ctx, "Column index %u out of range, column count is %d", index, rs->ncols);

    sqlite3_value *value = sqlite3_column_value(rs->vm, (int)index);
    return sqlite_value_to_js(ctx, value);
}

static JSValue js_rowset_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    UNUSED(argc);
    globaljs_context *js = JS_GetContextOpaque(ctx);
    rowset *rs = JS_GetOpaque(this_val, js->rowSetClassID);
    if (!rs) return JS_ThrowTypeError(ctx, "Not a Rowset object");

    if (!rs->vm) return JS_ThrowInternalError(ctx, "Rowset statement is no longer available");
    uint32_t index = 0;
    if (JS_ToUint32(ctx, &index, argv[0])) return JS_EXCEPTION;
    if (index >= (uint32_t)rs->ncols) return JS_ThrowRangeError(ctx, "Column index %u out of range, column count is %d", index, rs->ncols);
    
    const char *name = sqlite3_column_name(rs->vm, (int)index);
    if (!name) return JS_ThrowOutOfMemory(ctx);
    
    return JS_NewString(ctx, name);
}

static JSValue js_rowset_to_array(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    UNUSED(argc); UNUSED(argv);
    globaljs_context *js = JS_GetContextOpaque(ctx);
    rowset *rs = JS_GetOpaque(this_val, js->rowSetClassID);
    if (!rs) return JS_ThrowTypeError(ctx, "Not a Rowset object");
    if (!rs->vm) return JS_NewArray(ctx);

    JSValue result = JS_NewArray(ctx);
    if (JS_IsException(result)) return JS_EXCEPTION;
    
    int ncols = rs->ncols;
    int nrows = 0;
    int rc;
    while (true) {
        rc = sqlite3_step(rs->vm);
        if (rc != SQLITE_ROW) break;
        
        JSValue row = JS_NewArray(ctx);
        if (JS_IsException(row)) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
        
        for (int i=0; i<ncols; ++i) {
            sqlite3_value *value = sqlite3_column_value(rs->vm, i);
            JSValue jvalue = sqlite_value_to_js(ctx, value);
            JS_SetPropertyUint32(ctx, row, i, jvalue);
        }
        
        JS_SetPropertyUint32(ctx, result, nrows++, row);
    }

    // as in js_rowset_next: an error is an exception, not a shorter array
    if (rc != SQLITE_DONE) {
        JS_FreeValue(ctx, result);
        result = JS_ThrowInternalError(ctx, "%s", sqlite3_errmsg(sqlite3_db_handle(rs->vm)));
    }
    sqlite3_finalize(rs->vm);
    rs->vm = NULL;
    return result;
}

static void js_rowset_finalizer(JSRuntime *rt, JSValue val) {
    globaljs_context *js = JS_GetRuntimeOpaque(rt);
    rowset *rs = (rowset *)JS_GetOpaque(val, js->rowSetClassID);
    if (!rs) return;
    
    if (rs->vm) sqlite3_finalize(rs->vm);
    sqlite3_free(rs);
}

// MARK: - Initializer -

// QuickJS collects cyclic garbage only when its memory use passes a threshold, which it resets
// after each collection to 1.5 times the memory still in use. With more than two thirds of the
// memory limit in use, that is beyond the limit: garbage would fill the rest, and code that fits
// in the limit would fail. So the threshold is lowered to halfway between the memory in use and
// the limit. Called often, so it does nothing until a collection has changed the threshold.
static void js_keep_gc_below_limit (globaljs_context *js) {
    size_t threshold = JS_GetGCThreshold(js->runtime);
    if (threshold == js->gc_threshold) return;
    size_t limit = js->memory_limit;
    if ((limit > 0) && (threshold > limit - limit / 4)) {
        size_t used = threshold / 3 * 2;
        if (used > limit) used = limit;
        threshold = used + (limit - used) / 2;
        JS_SetGCThreshold(js->runtime, threshold);
    }
    js->gc_threshold = threshold;
}

// Collects, then puts the GC threshold back under the limit. Only to be called where no
// JavaScript is running: between a call from SQLite and the next, or as the limit changes.
static void js_collect_below_limit (globaljs_context *js) {
    if (!js || !js->runtime) return;
    JS_RunGC(js->runtime);
    js->gc_threshold = 0;   // JS_RunGC left a threshold of its own
    js_keep_gc_below_limit(js);
}

// QuickJS calls this from time to time while it runs code. A non-zero result stops the code
// with an uncatchable "interrupted" error, so sqlite3_interrupt() (Ctrl-C in the sqlite3 shell,
// a cancelled query in an app) also stops JavaScript that never returns to SQLite.
static int js_interrupt_handler (JSRuntime *rt, void *opaque) {
    UNUSED(rt);
    globaljs_context *js = (globaljs_context *)opaque;
    js_keep_gc_below_limit(js);
    return (js->db != NULL) && sqlite3_is_interrupted(js->db);
}

// The most memory, in bytes, the JavaScript runtime of a connection may allocate until
// js_set_memory_limit() changes it; 0 means no limit. Without a limit, code that keeps
// allocating takes all the memory of the process, which on a phone ends with the operating
// system killing it. 64 MB is room for over a thousand aggregate contexts (about 54 KB each).
#ifndef SQLITE_JS_DEFAULT_MEMORY_LIMIT
#define SQLITE_JS_DEFAULT_MEMORY_LIMIT  (64 * 1024 * 1024)
#endif

// The runtime's allocator: the system's, as QuickJS's default is, counting the memory in use the
// way QuickJS counts it against the memory limit, and the most in use since the last result or
// error (see js_reached_memory_limit). QuickJS refuses an allocation over the limit before it
// gets here.
#if defined(__APPLE__)
#define js_mem_usable_size(ptr)     malloc_size(ptr)
#define JS_MEM_OVERHEAD             0
#elif defined(_WIN32)
#define js_mem_usable_size(ptr)     _msize((void *)(ptr))
#define JS_MEM_OVERHEAD             8
#elif defined(__linux__) || defined(__ANDROID__) || defined(__GLIBC__)
#define js_mem_usable_size(ptr)     malloc_usable_size((void *)(ptr))
#define JS_MEM_OVERHEAD             8
#else
#define js_mem_usable_size(ptr)     ((size_t)0)
#define JS_MEM_OVERHEAD             0
#endif

static void js_mem_add (globaljs_context *js, void *ptr) {
    if (!ptr) return;
    js->mem_used += js_mem_usable_size(ptr) + JS_MEM_OVERHEAD;
    if (js->mem_used > js->mem_peak) js->mem_peak = js->mem_used;
}

static void js_mem_sub (globaljs_context *js, void *ptr) {
    if (ptr) js->mem_used -= js_mem_usable_size(ptr) + JS_MEM_OVERHEAD;
}

static void *js_mem_calloc (void *opaque, size_t count, size_t size) {
    void *ptr = calloc(count, size);
    js_mem_add((globaljs_context *)opaque, ptr);
    return ptr;
}

static void *js_mem_malloc (void *opaque, size_t size) {
    void *ptr = malloc(size);
    js_mem_add((globaljs_context *)opaque, ptr);
    return ptr;
}

static void js_mem_free (void *opaque, void *ptr) {
    js_mem_sub((globaljs_context *)opaque, ptr);
    free(ptr);
}

static void *js_mem_realloc (void *opaque, void *ptr, size_t size) {
    globaljs_context *js = (globaljs_context *)opaque;
    size_t old = (ptr) ? js_mem_usable_size(ptr) + JS_MEM_OVERHEAD : 0;
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr && size) return NULL; // the old block is unchanged
    js->mem_used -= old;
    js_mem_add(js, new_ptr);
    return new_ptr;
}

static size_t js_mem_size (const void *ptr) {
    return js_mem_usable_size(ptr);
}

static const JSMallocFunctions js_mem_funcs = {js_mem_calloc, js_mem_malloc, js_mem_free, js_mem_realloc, js_mem_size};

static void js_mem_reset_peak (globaljs_context *js) {
    if (js) js->mem_peak = js->mem_used;
}

// Stack, in bytes, kept free below the JavaScript stack limit for the native code that runs
// between two of QuickJS's stack checks: QuickJS's own C functions, and SQLite when JavaScript
// runs SQL through db.exec.
#ifndef SQLITE_JS_STACK_MARGIN
#define SQLITE_JS_STACK_MARGIN  (256 * 1024)
#endif

// The stack, in bytes, that the calling thread has left below this function, less the margin.
// QuickJS trusts its stack size: one larger than the thread has never trips its check, and code
// that calls itself without end then crashes the process. Returns false where the thread's stack
// cannot be found.
static bool js_stack_room (size_t *room) {
    // where the stack is now, found the way QuickJS finds it: the address of a local variable
    // can be elsewhere, for example on AddressSanitizer's separate stack
#if defined(__clang__) || defined(__GNUC__)
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
#elif defined(_MSC_VER)
    uintptr_t sp = (uintptr_t)_AddressOfReturnAddress();
#else
    volatile char here = 0;
    uintptr_t sp = (uintptr_t)&here;
#endif
    uintptr_t low = 0;
#if defined(_WIN32)
    // the stack is one allocation, reserved from its lowest address
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)sp, &mbi, sizeof(mbi)) == 0) return false;
    low = (uintptr_t)mbi.AllocationBase;
#elif defined(__APPLE__)
    pthread_t self = pthread_self();
    low = (uintptr_t)pthread_get_stackaddr_np(self) - pthread_get_stacksize_np(self);
#elif defined(__linux__)
    pthread_attr_t attr;
    void *addr = NULL;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return false;
    int rc = pthread_attr_getstack(&attr, &addr, &size);
    pthread_attr_destroy(&attr);
    if (rc != 0) return false;
    low = (uintptr_t)addr;
#else
    return false;
#endif
    *room = (sp > low + SQLITE_JS_STACK_MARGIN) ? (size_t)(sp - low - SQLITE_JS_STACK_MARGIN) : 0;
    return true;
}

static globaljs_context *globaljs_init (sqlite3 *db) {
    JSRuntime *rt = NULL;
    JSContext *ctx = NULL;
    
    globaljs_context *js = (globaljs_context *)sqlite3_malloc(sizeof(globaljs_context));
    if (!js) return NULL;
    memset(js, 0, sizeof(globaljs_context));
    js->typed_array_byte_offset = JS_UNDEFINED;
    js->typed_array_byte_length = JS_UNDEFINED;
    
    rt = JS_NewRuntime2(&js_mem_funcs, js);
    if (!rt) goto abort_init;
    
    //JS_SetMemoryLimit(rt, 80 * 1024);
    //JS_SetMaxStackSize(rt, 10 * 1024);
    
    // sqlite3_is_interrupted() is new in SQLite 3.41.0: an older library has no such entry
    // in its sqlite3_api_routines, so JavaScript cannot be interrupted there
    if (sqlite3_libversion_number() >= 3041000) JS_SetInterruptHandler(rt, js_interrupt_handler, js);
    
    // the default stack size must fit in the stack of the thread that loads the extension: Windows
    // gives a main thread 1 MB, and apps often give their threads no more
    size_t room = 0;
    if (js_stack_room(&room) && (room < JS_DEFAULT_STACK_SIZE)) JS_SetMaxStackSize(rt, (room > 0) ? room : 1);

    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    
    ctx = JS_NewContext(rt);
    if (!ctx) goto abort_init;
    
    js->db = db;
    js->runtime = rt;
    js->context = ctx;
    js->ref_count = 0;
    
    JS_SetContextOpaque(ctx, js);
    JS_SetRuntimeOpaque(rt, js);
    js_global_init(ctx, js);
    
    // taken before any other code runs: a script can shadow or redefine a typed array's
    // byteOffset and byteLength, but these are its own
    static const char offset_getter[] = "Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), 'byteOffset').get";
    static const char length_getter[] = "Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), 'byteLength').get";
    js->typed_array_byte_offset = JS_Eval(ctx, offset_getter, sizeof(offset_getter) - 1, "<init>", JS_EVAL_TYPE_GLOBAL);
    js->typed_array_byte_length = JS_Eval(ctx, length_getter, sizeof(length_getter) - 1, "<init>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsFunction(ctx, js->typed_array_byte_offset) || !JS_IsFunction(ctx, js->typed_array_byte_length)) goto abort_init;
    // set only now: QuickJS cannot always recover from running out of memory while it creates
    // a context (see js_setup_aggregate)
    js->memory_limit = SQLITE_JS_DEFAULT_MEMORY_LIMIT;
    JS_SetMemoryLimit(rt, js->memory_limit);
    
    return js;
    
abort_init:
    if (ctx) {
        JS_FreeValue(ctx, js->typed_array_byte_offset);
        JS_FreeValue(ctx, js->typed_array_byte_length);
        JS_FreeContext(ctx);
    }
    if (rt) {
        js_std_free_handlers(rt);
        JS_FreeRuntime(rt);
    }
    if (js) sqlite3_free(js);
    return NULL;
}

static void globaljs_free (globaljs_context *js) {
    if (!js) return;

    // order matters
    if (js->runtime) js_std_free_handlers(js->runtime);
    if (js->context) {
        JS_FreeValue(js->context, js->typed_array_byte_offset);
        JS_FreeValue(js->context, js->typed_array_byte_length);
        JS_FreeContext(js->context);
    }
    if (js->runtime) JS_FreeRuntime(js->runtime);
    sqlite3_free(js);
}

void globaljs_dec_and_free_if_needed (void *ptr) {
    if (!ptr) return;
    globaljs_context *js = (globaljs_context *)ptr;
    
    // Decrement reference count and free global context if needed
    js->ref_count--;
    if (js->ref_count == 0) globaljs_free(js);
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
    memset(fctx, 0, sizeof(functionjs_context));
    
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
    
    fctx->js_ctx = jsctx;
    jsctx->ref_count++;  // Increment reference count
    
    fctx->init_code = init_code_copy;
    fctx->step_code = step_code_copy;
    fctx->final_code = final_code_copy;
    fctx->value_code = value_code_copy;
    fctx->inverse_code = inverse_code_copy;
    
    fctx->func = JS_NULL;

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

static void functionjs_aggregate_free (functionjs_aggregate_context *agg_ctx) {
    if (!agg_ctx) return;
    
    JSContext *context = agg_ctx->context;
    
    // always to release
    if (!JS_IsNull(agg_ctx->step_func)) JS_FreeValue(context, agg_ctx->step_func);
    if (!JS_IsNull(agg_ctx->final_func)) JS_FreeValue(context, agg_ctx->final_func);
    if (!JS_IsNull(agg_ctx->value_func)) JS_FreeValue(context, agg_ctx->value_func);
    if (!JS_IsNull(agg_ctx->inverse_func)) JS_FreeValue(context, agg_ctx->inverse_func);
    JS_FreeContext(context);
    
    agg_ctx->step_func = JS_NULL;
    agg_ctx->final_func = JS_NULL;
    agg_ctx->value_func = JS_NULL;
    agg_ctx->inverse_func = JS_NULL;
    agg_ctx->context = NULL;
}

static void functionjs_free (functionjs_context *fctx) {
    if (!fctx) return;
    globaljs_context *js = fctx->js_ctx;
    
    if (!JS_IsNull(fctx->func)) JS_FreeValue(js->context, fctx->func);
    
    if (fctx->init_code) sqlite3_free((void *)fctx->init_code);
    if (fctx->step_code) sqlite3_free((void *)fctx->step_code);
    if (fctx->final_code) sqlite3_free((void *)fctx->final_code);
    if (fctx->value_code) sqlite3_free((void *)fctx->value_code);
    if (fctx->inverse_code) sqlite3_free((void *)fctx->inverse_code);
    sqlite3_free(fctx);
    
    globaljs_dec_and_free_if_needed(js);
}

// MARK: - Utils -

// A JavaScript number is a double. QuickJS keeps the small whole numbers it can as 32-bit
// integers, but an INTEGER outside that range, or the result of arithmetic such as 2.5 * 2,
// is a double too. A whole number that a double holds exactly, from -(2^53 - 1) to 2^53 - 1
// (Number.isSafeInteger), goes back to SQL as an INTEGER; any other number is a REAL.
static bool js_number_to_int64 (double num, sqlite3_int64 *out) {
    const double max_safe_integer = 9007199254740991.0;
    // NaN fails both comparisons
    if (!(num >= -max_safe_integer && num <= max_safe_integer) || (num != floor(num))) return false;
    *out = (sqlite3_int64)num;
    return true;
}

// Gets the bytes of an ArrayBuffer, or of the part of an ArrayBuffer that a typed array such
// as a Uint8Array views: the forms a BLOB takes in JavaScript (a BLOB arrives as an
// ArrayBuffer). Returns 1 and sets *pbytes and *psize if value is one of them (*pbytes may be
// NULL when *psize is 0), 0 if it is not, and -1 with a pending JavaScript exception if its
// buffer is detached or the typed array is out of its bounds.
static int js_get_bytes (JSContext *ctx, JSValueConst value, const uint8_t **pbytes, size_t *psize) {
    *pbytes = NULL;
    *psize = 0;
    
    if (JS_IsArrayBuffer(value)) {
        uint8_t *buf = JS_GetArrayBuffer(ctx, psize, value);
        if (!buf && JS_HasException(ctx)) return -1;
        *pbytes = buf;
        return 1;
    }
    
    if (JS_GetTypedArrayType(value) >= 0) {
        // The offset and length come from the byteOffset and byteLength getters: a typed array
        // can track the length of a resizable buffer, and the length QuickJS records when the
        // array is made goes stale when the buffer is resized. They are the getters taken when
        // the runtime was made (see globaljs_init), not the properties, which a script can
        // shadow, so they run no JavaScript.
        globaljs_context *js = (globaljs_context *)JS_GetContextOpaque(ctx);
        int64_t offset = 0, length = 0;
        JSValue prop = JS_Call(ctx, js->typed_array_byte_offset, value, 0, NULL);
        int rc = JS_IsException(prop) ? -1 : JS_ToInt64(ctx, &offset, prop);
        JS_FreeValue(ctx, prop);
        if (rc < 0) return -1;
        prop = JS_Call(ctx, js->typed_array_byte_length, value, 0, NULL);
        rc = JS_IsException(prop) ? -1 : JS_ToInt64(ctx, &length, prop);
        JS_FreeValue(ctx, prop);
        if (rc < 0) return -1;
        
        size_t size = 0;
        JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, NULL, NULL, NULL);
        if (JS_IsException(buffer)) return -1;
        uint8_t *buf = JS_GetArrayBuffer(ctx, &size, buffer);
        // the typed array keeps its buffer alive, so buf stays valid
        JS_FreeValue(ctx, buffer);
        if (!buf && JS_HasException(ctx)) return -1;
        if ((offset < 0) || (length < 0) || ((uint64_t)offset > size) || ((uint64_t)length > size - (size_t)offset)) {
            JS_ThrowRangeError(ctx, "typed array is out of the bounds of its buffer");
            return -1;
        }
        *pbytes = (buf) ? buf + offset : NULL;
        *psize = (size_t)length;
        return 1;
    }
    
    return 0;
}

static int js_bind_value (JSContext *ctx, sqlite3_stmt *vm, int index, JSValueConst val) {
    if (JS_IsNull(val) || JS_IsUndefined(val)) {
        return sqlite3_bind_null(vm, index);
    }
    if (JS_IsNumber(val)) {
        int tag = JS_VALUE_GET_TAG(val);
        if (tag == JS_TAG_INT) {
            int32_t num;
            JS_ToInt32(ctx, &num, val);
            return sqlite3_bind_int(vm, index, num);
        } else {
            double num;
            sqlite3_int64 inum;
            JS_ToFloat64(ctx, &num, val);
            if (js_number_to_int64(num, &inum)) return sqlite3_bind_int64(vm, index, inum);
            return sqlite3_bind_double(vm, index, num);
        }
    }
    if (JS_IsBigInt(val)) {
        int64_t num;
        if (JS_ToBigInt64(ctx, &num, val) == 0) {
            return sqlite3_bind_int64(vm, index, num);
        }
        return sqlite3_bind_null(vm, index);
    }
    if (JS_IsBool(val)) {
        return sqlite3_bind_int(vm, index, JS_ToBool(ctx, val));
    }
    if (JS_IsString(val)) {
        size_t len = 0;
        const char *str = JS_ToCStringLen(ctx, &len, val);
        if (!str) return SQLITE_NOMEM;
        int rc = sqlite3_bind_text(vm, index, str, (int)len, SQLITE_TRANSIENT);
        JS_FreeCString(ctx, str);
        return rc;
    }
    const uint8_t *bytes = NULL;
    size_t size = 0;
    int is_blob = js_get_bytes(ctx, val, &bytes, &size);
    // a buffer whose bytes cannot be read (detached, or a view out of its bounds) is an error, as
    // returning it from a function is: the JavaScript exception stays pending for js_sqlite_exec
    if (is_blob < 0) return SQLITE_ERROR;
    if (is_blob) {
        if (size == 0) return sqlite3_bind_zeroblob(vm, index, 0);
        return sqlite3_bind_blob64(vm, index, bytes, (sqlite3_uint64)size, SQLITE_TRANSIENT);
    }
    return sqlite3_bind_null(vm, index);
}

static JSValue js_sqlite_exec (JSContext *ctx, sqlite3 *db, const char *sql, int argc, JSValueConst *argv) {
    sqlite3_stmt *vm = NULL;
    rowset *rs = NULL;

    // compile statement
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto abort_with_dberror;

    // bind parameters from JS arguments
    int nbind = sqlite3_bind_parameter_count(vm);
    if (nbind > 0 && argc > 0) {
        if (nbind > argc) nbind = argc;
        for (int i = 0; i < nbind; ++i) {
            // argv[0] is the first binding argument (SQL string already consumed by caller)
            rc = js_bind_value(ctx, vm, i + 1, argv[i + 1]);
            if ((rc != SQLITE_OK) && JS_HasException(ctx)) {
                // a JavaScript error, such as a buffer that cannot be read: throw it as it is
                sqlite3_finalize(vm);
                return JS_EXCEPTION;
            }
            if (rc != SQLITE_OK) goto abort_with_dberror;
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
    UNUSED(this_val);
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

#if 0
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
#endif

// True if JavaScript reached its memory limit since the last result or error. QuickJS then may
// have had no memory left to make its "out of memory" error, and have thrown null instead.
static bool js_reached_memory_limit (globaljs_context *js) {
    return js && (js->memory_limit > 0) && (js->mem_peak + 64 * 1024 >= js->memory_limit);
}

static void js_error_to_sqlite (sqlite3_context *context, JSContext *js_ctx, JSValue value, const char *default_error) {
    if (!default_error) default_error = "Unknown JavaScript exception";
    const char *err_msg = NULL;
    JSValue exception = JS_NULL;

    if (JS_IsException(value)) {
        exception = JS_GetException(js_ctx);
        if (JS_IsObject(exception)) {
            JSValue message = JS_GetPropertyStr(js_ctx, exception, "message");
            if (!JS_IsException(message) && JS_IsString(message)) {
                err_msg = JS_ToCString(js_ctx, message);
            }
            JS_FreeValue(js_ctx, message);
        } else if (JS_IsString(exception)) {
            err_msg = JS_ToCString(js_ctx, exception);
        } else if (JS_IsNull(exception) && js_reached_memory_limit(JS_GetContextOpaque(js_ctx))) {
            default_error = "out of memory";
        }
    }
    bool out_of_memory = js_reached_memory_limit(JS_GetContextOpaque(js_ctx));
    js_mem_reset_peak(JS_GetContextOpaque(js_ctx));

    // set a default error message and code
    sqlite3_result_error(context, (err_msg) ? err_msg : default_error, -1);
    sqlite3_result_error_code(context, SQLITE_ERROR);

    // clean-up
    if (err_msg) JS_FreeCString(js_ctx, err_msg);
    if (!JS_IsNull(exception)) JS_FreeValue(js_ctx, exception);

    // QuickJS refuses every allocation once the memory in use is over the limit, and it collects
    // only when it creates an object, which it cannot do without allocating: the connection
    // would stay out of memory for good. No JavaScript is running here, so collect now, while
    // the garbage that filled the limit is still reachable from nothing.
    if (out_of_memory) js_collect_below_limit(JS_GetContextOpaque(js_ctx));
}

static void js_value_to_sqlite (sqlite3_context *context, JSContext *js_ctx, JSValue value) {
    // check for exceptions first and convert it to a proper error message (if any)
    if (JS_IsException(value)) {
        js_error_to_sqlite(context, js_ctx, value, NULL);
        return;
    }
    js_mem_reset_peak(JS_GetContextOpaque(js_ctx));
    
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
        } else {
            double num;
            sqlite3_int64 inum;
            JS_ToFloat64(js_ctx, &num, value);
            if (js_number_to_int64(num, &inum)) sqlite3_result_int64(context, inum);
            else sqlite3_result_double(context, num);
        }
        return;
    }

    // handle BigInt
    if (JS_IsBigInt(value)) {
        int64_t num;
        if (JS_ToBigInt64(js_ctx, &num, value) == 0) {
            sqlite3_result_int64(context, num);
        } else {
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
    
    // handle ArrayBuffer and typed arrays, the forms a BLOB takes in JavaScript
    const uint8_t *bytes = NULL;
    size_t size = 0;
    int is_blob = js_get_bytes(js_ctx, value, &bytes, &size);
    if (is_blob < 0) {
        js_error_to_sqlite(context, js_ctx, JS_EXCEPTION, NULL);
        return;
    }
    if (is_blob) {
        if (size == 0) sqlite3_result_zeroblob(context, 0);
        else sqlite3_result_blob64(context, bytes, (sqlite3_uint64)size, SQLITE_TRANSIENT);
        return;
    }
    
    if (JS_IsObject(value)) {
        sqlite3_result_null(context);
        return;
    }
    
    // fallback for unsupported types
    sqlite3_result_error(context, "Unsupported JS value type", -1);
}

static bool js_setup_aggregate (sqlite3_context *context, globaljs_context *js, functionjs_aggregate_context *agg_ctx, const char *init_code, const char *step_code, const char *final_code, const char *value_code, const char *inverse_code) {
    bool result = false;
    
    if (agg_ctx) {
        // initialize aggregate context functions with null values
        agg_ctx->step_func = JS_NULL;
        agg_ctx->final_func = JS_NULL;
        agg_ctx->value_func = JS_NULL;
        agg_ctx->inverse_func = JS_NULL;
    }
    
    // step and final code are mandatory because they are evaluated below with strlen()
    if (!step_code || !final_code) {
        sqlite3_result_error(context, "JavaScript step and final code are both required", -1);
        return false;
    }
    
    // create a separate context for testing purpose
    // Each finished aggregate leaves its context behind as cyclic garbage, so the collector must
    // be able to run before the limit is reached.
    js_keep_gc_below_limit(js);
    // QuickJS cannot always recover from running out of memory while it creates a context: it
    // can crash. So the context is created without the memory limit (it takes about 54 KB),
    // which then applies again to the code that runs in it.
    JS_SetMemoryLimit(js->runtime, 0);
    JSContext *ctx = JS_NewContext(js->runtime);
    if (ctx) {
        // setup global object
        JS_SetContextOpaque(ctx, js);
        js_global_init(ctx, js);
    }
    JS_SetMemoryLimit(js->runtime, js->memory_limit);
    js_mem_reset_peak(js);
    if (!ctx) {
        sqlite3_result_error(context, "Unable to create a JS context.", -1);
        return false;
    }
    
    // init code is optional
    if (init_code) {
        JSValue init_result = JS_Eval(ctx, init_code, strlen(init_code), NULL, JS_EVAL_TYPE_GLOBAL);
        bool is_error = JS_IsException(init_result);
        if (is_error) js_error_to_sqlite(context, ctx, init_result, NULL);
        JS_FreeValue(ctx, init_result);
        if (is_error) {
            JS_FreeContext(ctx);
            return false;
        }
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
    if (agg_ctx) {
        // setup a proper function aggregate context
        agg_ctx->context = ctx;
        agg_ctx->step_func = step_func;
        agg_ctx->final_func = final_func;
        agg_ctx->value_func = value_func;
        agg_ctx->inverse_func = inverse_func;
        return true;
    }
    
    result = true;
    
cleanup:
    if (!result) {
        if (!JS_IsFunction(ctx, step_func)) js_error_to_sqlite(context, ctx, step_func, "JavaScript step code must evaluate to a function in the form (function(args){ your_code_here })");
        else if (!JS_IsFunction(ctx, final_func)) js_error_to_sqlite(context, ctx, final_func, "JavaScript final code must evaluate to a function in the form (function(){ your_code_here })");
        else if (!JS_IsFunction(ctx, value_func)) js_error_to_sqlite(context, ctx, value_func, "JavaScript value code must evaluate to a function in the form (function(){ your_code_here })");
        else if (!JS_IsFunction(ctx, inverse_func)) js_error_to_sqlite(context, ctx, inverse_func, "JavaScript inverse code must evaluate to a function in the form (function(args){ your_code_here })");
    }
    
    JS_FreeValue(ctx, step_func);
    JS_FreeValue(ctx, final_func);
    JS_FreeValue(ctx, value_func);
    JS_FreeValue(ctx, inverse_func);
    
    JS_FreeContext(ctx);
    return result;
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

static void js_execute_common (sqlite3_context *context, JSContext *js_context, int nvalues, sqlite3_value **values, JSValue func, JSValue this_obj, bool return_value) {
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
    // step and inverse functions return nothing, but an exception they throw is still an error:
    // reported from xStep or xInverse, it makes SQLite abort the statement
    else if (JS_IsException(result)) js_error_to_sqlite(context, js_context, result, NULL);
    JS_FreeValue(js_context, result);
}

static void js_execute_scalar (sqlite3_context *context, int nvalues, sqlite3_value **values) {
    functionjs_context *fctx = (functionjs_context *)sqlite3_user_data(context);
    js_execute_common(context, fctx->js_ctx->context, nvalues, values, fctx->func, JS_UNDEFINED, true);
}

static void js_execute_step (sqlite3_context *context, int nvalues, sqlite3_value **values) {
    functionjs_context *fctx = (functionjs_context *)sqlite3_user_data(context);
    globaljs_context *js = fctx->js_ctx;
    functionjs_aggregate_context *agg_ctx = sqlite3_aggregate_context(context, sizeof(*agg_ctx));
    if (!agg_ctx) {
        // sqlite3_aggregate_context returns NULL when the allocation fails
        sqlite3_result_error_nomem(context);
        return;
    }
    
    // if there is an init code then create a separate aggregate context
    // to avoid shared state corruption across parallel aggregates
    if (!agg_ctx->context) {
        // set up a new isolated environment for this aggregation
        if (js_setup_aggregate(context, js, agg_ctx, fctx->init_code, fctx->step_code, fctx->final_code, fctx->value_code, fctx->inverse_code) == false) return;
    }

    js_execute_common(context, agg_ctx->context, nvalues, values, agg_ctx->step_func, JS_UNDEFINED, false);
}

static void js_execute_value (sqlite3_context *context) {
    // ask for 0 bytes: the context has already been allocated by js_execute_step, so a
    // new allocation here could only fail and there would be nothing to report anyway
    functionjs_aggregate_context *agg_ctx = sqlite3_aggregate_context(context, 0);
    if (!agg_ctx || !agg_ctx->context) { sqlite3_result_null(context); return; }
    js_execute_common(context, agg_ctx->context, 0, NULL, agg_ctx->value_func, JS_UNDEFINED, true);
}

static void js_execute_inverse (sqlite3_context *context, int nvalues, sqlite3_value **values) {
    functionjs_aggregate_context *agg_ctx = sqlite3_aggregate_context(context, 0);
    if (!agg_ctx || !agg_ctx->context) return;
    js_execute_common(context, agg_ctx->context, nvalues, values, agg_ctx->inverse_func, JS_UNDEFINED, false);
}

static void js_execute_final (sqlite3_context *context) {
    functionjs_aggregate_context *agg_ctx = sqlite3_aggregate_context(context, 0);

    if (agg_ctx && agg_ctx->context) {
        js_execute_common(context, agg_ctx->context, 0, NULL, agg_ctx->final_func, JS_UNDEFINED, true);
        functionjs_aggregate_free(agg_ctx);
    } else {
        sqlite3_result_null(context);
    }
}

static int js_execute_collation (void *xdata, int len1, const void *v1, int len2, const void *v2) {
    functionjs_context *fctx = (functionjs_context *)xdata;
    globaljs_context *js = fctx->js_ctx;

    // create arguments
    JSValue val1 = (v1) ? JS_NewStringLen(js->context, (const char *)v1, (size_t)len1) : JS_NULL;
    JSValue val2 = (v2) ? JS_NewStringLen(js->context, (const char *)v2, (size_t)len2) : JS_NULL;
    JSValue args[] = {val1, val2};
    
    // call the JavaScript function with arguments
    JSValue result = JS_Call(js->context, fctx->func, JS_UNDEFINED, 2, args);
    JS_FreeValue(js->context, val1);
    JS_FreeValue(js->context, val2);
    
    // default result if the JS code throws or does not return a number is 0, meaning
    // "equal": the answer must stay consistent, because a collation that reports
    // "less than" for both (a,b) and (b,a) breaks the ordering invariants SQLite relies on
    int nresult = 0;
    if (JS_IsException(result)) {
        // discard the pending exception, it would otherwise leak into the next call
        JS_FreeValue(js->context, JS_GetException(js->context));
    } else if (JS_IsNumber(result)) {
        // only the sign matters, so that fractional return values work as expected too
        double num = 0.0;
        if (JS_ToFloat64(js->context, &num, result) == 0) nresult = (num < 0.0) ? -1 : ((num > 0.0) ? 1 : 0);
    }
    JS_FreeValue(js->context, result);
    
    return nresult;
}

static void js_execute_cleanup (void *xdata) {
    if (!xdata) return;
    functionjs_free((functionjs_context *)xdata);
}

// MARK: - Functions -

void js_version (sqlite3_context *context, bool internal_engine) {
    sqlite3_result_text(context, (internal_engine) ? quickjs_version() : sqlitejs_version(), -1, NULL);
}

void js_version1 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED(argc);
    bool internal_engine = (sqlite3_value_int(argv[0]) != 0);
    js_version(context, internal_engine);
}

void js_version0 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED(argc); UNUSED(argv);
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

    sqlite3_bind_text(vm, 1, name, -1, SQLITE_STATIC);
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) {
        // no functions with that name exist, so add it to the table
        force_reinsert = true;
        goto insert_function;
    } else if (rc != SQLITE_ROW) {
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
    
    if ((type2 == NULL) || (strcasecmp(type, type2) != 0) ||
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
    rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(vm, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(vm, 2, type, -1, SQLITE_STATIC);
    (init_code == NULL) ? sqlite3_bind_null(vm, 3) : sqlite3_bind_text(vm, 3, init_code, -1, SQLITE_STATIC);
    (step_code == NULL) ? sqlite3_bind_null(vm, 4) : sqlite3_bind_text(vm, 4, step_code, -1, SQLITE_STATIC);
    (final_code == NULL) ? sqlite3_bind_null(vm, 5) : sqlite3_bind_text(vm, 5, final_code, -1, SQLITE_STATIC);
    (value_code == NULL) ? sqlite3_bind_null(vm, 6) : sqlite3_bind_text(vm, 6, value_code, -1, SQLITE_STATIC);
    (inverse_code == NULL) ? sqlite3_bind_null(vm, 7) : sqlite3_bind_text(vm, 7, inverse_code, -1, SQLITE_STATIC);

    rc = sqlite3_step(vm);
    sqlite3_finalize(vm);
    
    return (rc == SQLITE_DONE);
}

bool js_create_common (sqlite3_context *context, const char *type, const char *name, const char *init_code, const char *step_code, const char *final_code, const char *value_code, const char *inverse_code, bool is_load) {
    
    globaljs_context *js = (globaljs_context *)sqlite3_user_data(context);
    
    // the js_create_xxx entry points validate their own arguments, but a definition loaded
    // from the js_functions table can have any shape because that table does not constrain
    // its columns (they are all nullable), so everything must be validated here as well
    if ((type == NULL) || (name == NULL)) {
        sqlite3_result_error(context, "A function definition requires a non-NULL name and kind", -1);
        return false;
    }
    
    bool is_scalar = (strcasecmp(type, FUNCTION_TYPE_SCALAR) == 0);
    bool is_aggregate = (is_scalar) ? false : (strcasecmp(type, FUNCTION_TYPE_AGGREGATE) == 0);
    bool is_window = (is_aggregate) ? false : (strcasecmp(type, FUNCTION_TYPE_WINDOW) == 0);
    bool is_collation = (is_window) ? false : (strcasecmp(type, FUNCTION_TYPE_COLLATION) == 0);
    bool step_code_null = (is_scalar || is_collation);
    
    // an unrecognized kind would register nothing while still reporting success, leaking the
    // function context and the reference it holds on the global JS context
    if (!is_scalar && !is_aggregate && !is_window && !is_collation) {
        sqlite3_result_error(context, "Unsupported function kind: expected 'scalar', 'aggregate', 'window' or 'collation'", -1);
        return false;
    }
    
    // every kind needs step code, aggregate and window functions need final code too and
    // window functions also need value and inverse code
    if (step_code == NULL) {
        sqlite3_result_error(context, "JavaScript step code is required", -1);
        return false;
    }
    if ((is_aggregate || is_window) && (final_code == NULL)) {
        sqlite3_result_error(context, "JavaScript final code is required by aggregate and window functions", -1);
        return false;
    }
    if (is_window && ((value_code == NULL) || (inverse_code == NULL))) {
        sqlite3_result_error(context, "JavaScript value and inverse code are required by window functions", -1);
        return false;
    }
    
    if (is_aggregate || is_window) {
        // sanity check aggregate code, including the window specific code so that an invalid
        // value or inverse function is reported now and not at the first query
        if (js_setup_aggregate(context, js, NULL, init_code, step_code, final_code, value_code, inverse_code) == false) return false;
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
            JS_FreeValue(js->context, func);
            functionjs_free(fctx);
            return false;
        }
        fctx->func = func;
    }
    
    int rc = SQLITE_OK;
    if (is_scalar) rc = sqlite3_create_function_v2(sqlite3_context_db_handle(context), name, -1, SQLITE_UTF8, (void *)fctx, js_execute_scalar, NULL, NULL, js_execute_cleanup);
    else if (is_aggregate) rc = sqlite3_create_function_v2(sqlite3_context_db_handle(context), name, -1, SQLITE_UTF8, (void *)fctx, NULL, js_execute_step, js_execute_final, js_execute_cleanup);
    else if (is_window) rc = sqlite3_create_window_function(sqlite3_context_db_handle(context), name, -1, SQLITE_UTF8, (void *)fctx, js_execute_step, js_execute_final, js_execute_value, js_execute_inverse, js_execute_cleanup);
    else rc = sqlite3_create_collation_v2(sqlite3_context_db_handle(context), name, SQLITE_UTF8, (void *)fctx, js_execute_collation, js_execute_cleanup);
    
    if (rc == SQLITE_BUSY) {
        // Due to this: https://www3.sqlite.org/src/info/cabab62bc10568d4
        // it is not possible to update or delete a previously registered function
        // within the same database connection.
        // There is nothing we can do because this is an SQLite implementation detail.
        // Function updates must be performed using a separate database connection.
        sqlite3_result_error(context, "Function updates must be performed using a separate database connection.", -1);
    }
    
    // sqlite3_create_function_v2 and sqlite3_create_window_function always invoke the
    // xDestroy callback when registration fails, but sqlite3_create_collation_v2 does not,
    // so for collations the function context has to be released here
    if ((rc != SQLITE_OK) && is_collation) functionjs_free(fctx);
    
    if ((is_load == false) && (rc == SQLITE_OK)) {
        js_add_to_table(context, type, name, init_code, step_code, final_code, value_code, inverse_code);
    }
    
    // js_execute_cleanup is automatically called in case of error (collations excluded, see above)
    (rc == SQLITE_OK) ? sqlite3_result_int(context, SQLITE_OK) : sqlite3_result_error_code(context, rc);
    return (rc == SQLITE_OK);
}

void js_create_scalar (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED(argc);
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
    UNUSED(argc);
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
    UNUSED(argc);
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
    UNUSED(argc);
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
    UNUSED(argc);
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

static void js_load_fromfile (sqlite3_context *context, int argc, sqlite3_value **argv, bool is_blob) {
    UNUSED(argc);
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
    
    // report a real error instead of turning a failed fseek/ftell into a bogus length
    long fsize = -1;
    if (fseek(f, 0, SEEK_END) == 0) fsize = ftell(f);
    if (fsize < 0) {
        fclose(f);
        sqlite3_result_error(context, "Unable to compute the file size", -1);
        return;
    }
    rewind(f);
    
    size_t length = (size_t)fsize;
    if (length > (size_t)INT_MAX) {
        // sqlite3_result_text and sqlite3_result_blob take an int length
        fclose(f);
        sqlite3_result_error_toobig(context);
        return;
    }
    
    // an empty file is a valid result, not an out of memory condition: sqlite3_malloc(0)
    // returns NULL and would otherwise be reported as SQLITE_NOMEM
    if (length == 0) {
        fclose(f);
        (is_blob) ? sqlite3_result_zeroblob(context, 0) : sqlite3_result_text(context, "", 0, SQLITE_STATIC);
        return;
    }
    
    char *buffer = (char *)sqlite3_malloc((int)length);
    if (!buffer) {
        fclose(f);
        sqlite3_result_error_nomem(context);
        return;
    }
    
    size_t nread = fread(buffer, 1, length, f);
    if (ferror(f)) {
        sqlite3_free(buffer);
        sqlite3_result_error(context, "Unable to correctly read the file", -1);
    } else {
        // in text mode fread can legitimately return less than the file size, for instance
        // when CRLF line endings are translated on Windows, so use nread and not length
        (is_blob) ? sqlite3_result_blob(context, buffer, (int)nread, sqlite3_free) : sqlite3_result_text(context, buffer, (int)nread, sqlite3_free);
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
    UNUSED(names);
    
    // js_load_from_table lists the seven columns it needs explicitly, so a different count
    // can only mean an unexpected table layout: abort the load instead of reading past the
    // end of the values array (assert would be compiled out in a release build)
    if (ncols != 7) {
        sqlite3_result_error(context, "Unexpected js_functions table layout", -1);
        return SQLITE_ERROR;
    }
    
    const char *name = values[0];
    const char *type = values[1];
    const char *init_code = values[2];
    const char *step_code = values[3];
    const char *final_code = values[4];
    const char *value_code = values[5];
    const char *inverse_code = values[6];
    
    bool result = js_create_common(context, type, name, init_code, step_code, final_code, value_code, inverse_code, true);
    return (result) ? SQLITE_OK : SQLITE_ERROR;
}

int js_load_from_table (sqlite3_context *context, char **err_msg) {
    sqlite3 *db = sqlite3_context_db_handle(context);
    // never use SELECT *: js_functions could have been created elsewhere with a different
    // layout and js_load_from_table_callback relies on this exact column order
    const char *sql = "SELECT name,kind,init_code,step_code,final_code,value_code,inverse_code FROM js_functions;";
    return sqlite3_exec(db, sql, js_load_from_table_callback, context, err_msg);
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
    if (load_functions) {
        char *err_msg = NULL;
        rc = js_load_from_table(context, &err_msg);
        if (rc != SQLITE_OK) {
            // SQLITE_ABORT means that js_load_from_table_callback stopped the loop, and in
            // that case js_create_common has already set a detailed error message on the
            // context: keep it instead of the generic "query aborted" that sqlite3_exec
            // reports for every aborted callback
            if (rc != SQLITE_ABORT) {
                if (err_msg) sqlite3_result_error(context, err_msg, -1);
                sqlite3_result_error_code(context, rc);
            }
            if (err_msg) sqlite3_free(err_msg);
            return;
        }
    }
    
    sqlite3_result_int(context, rc);
}

void js_init_table1 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED(argc);
    bool load_functions = (sqlite3_value_int(argv[0]) != 0);
    js_init_table(context, load_functions);
}

void js_init_table0 (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED(argc); UNUSED(argv);
    js_init_table(context, false);
}

void js_set_max_stack_size (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED(argc);
    globaljs_context *js = (globaljs_context *)sqlite3_user_data(context);

    // 0 turns QuickJS's check off, for a connection used from several threads (the check measures
    // from the stack of the thread that loaded the extension). Text would read as 0, and a size
    // larger than the thread's stack is no limit either: code that calls itself without end
    // would crash the process. Where the stack cannot be measured, allow up to the default.
    size_t most = JS_DEFAULT_STACK_SIZE;
    js_stack_room(&most);
    sqlite3_int64 size = sqlite3_value_int64(argv[0]);
    if ((sqlite3_value_type(argv[0]) != SQLITE_INTEGER) || (size < 0) || ((sqlite3_uint64)size > most)) {
        char *msg = sqlite3_mprintf("The stack size must be an integer number of bytes from 1 to %llu, which this thread's stack allows, or 0 for no check", (unsigned long long)most);
        if (msg) sqlite3_result_error(context, msg, -1);
        else sqlite3_result_error_nomem(context);
        sqlite3_free(msg);
        return;
    }

    JS_SetMaxStackSize(js->runtime, (size_t)size);
}

void js_set_memory_limit (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED(argc);
    globaljs_context *js = (globaljs_context *)sqlite3_user_data(context);
    
    if ((sqlite3_value_type(argv[0]) != SQLITE_INTEGER) || (sqlite3_value_int64(argv[0]) < 0)) {
        sqlite3_result_error(context, "The memory limit must be an integer number of bytes, or 0 for no limit", -1);
        return;
    }
    
    // a limit beyond the address space (on a 32-bit system) can never be reached, so it is no limit
    sqlite3_uint64 limit = (sqlite3_uint64)sqlite3_value_int64(argv[0]);
    js->memory_limit = (limit > SIZE_MAX) ? 0 : (size_t)limit;
    JS_SetMemoryLimit(js->runtime, js->memory_limit);
    js->gc_threshold = 0;   // check the GC threshold against the new limit
    // a limit at or under the memory in use leaves nothing to allocate from, and QuickJS cannot
    // collect without allocating, so collect here instead
    if ((js->memory_limit > 0) && (js->mem_used + 64 * 1024 >= js->memory_limit)) js_collect_below_limit(js);
    else js_keep_gc_below_limit(js);
    js_mem_reset_peak(js);
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
    #else
    UNUSED(pApi);
    #endif
    
    globaljs_context *js = globaljs_init(db);
    if (!js) return SQLITE_NOMEM;
    
    const char *f_name[] = {"js_version", "js_version", "js_create_scalar", "js_create_aggregate", "js_create_window", "js_create_collation", "js_eval", "js_load_text", "js_load_blob", "js_init_table", "js_init_table", "js_set_max_stack_size", "js_set_memory_limit"};
    const void *f_ptr[] = {js_version0, js_version1, js_create_scalar, js_create_aggregate, js_create_window, js_create_collation, js_eval, js_load_text, js_load_blob, js_init_table0, js_init_table1, js_set_max_stack_size, js_set_memory_limit};
    int f_arg[] = {0, 1, 2, 4, 6, 2, 1, 1, 1, 0, 1, 1, 1};
    
    size_t f_count = sizeof(f_name) / sizeof(const char *);
    for (size_t i=0; i<f_count; ++i) {
        js->ref_count++;
        // Every function but js_version runs JavaScript, reads files or changes the JavaScript
        // runtime. SQLITE_DIRECTONLY keeps them out of the schema (views, triggers, CHECK
        // constraints, generated columns, indexes), so that a database from an untrusted source
        // cannot run JavaScript just by being queried, whatever PRAGMA trusted_schema says.
        int flags = (strcmp(f_name[i], "js_version") == 0) ? 0 : SQLITE_DIRECTONLY;
        int rc = sqlite3_create_function_v2(db, f_name[i], f_arg[i], SQLITE_UTF8 | flags, (void *)js, f_ptr[i], NULL, NULL, globaljs_dec_and_free_if_needed);
        if (rc != SQLITE_OK) {
            if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("Error creating function %s: %s", f_name[i], sqlite3_errmsg(db));
            // sqlite3_create_function_v2 always invokes the xDestroy callback when it fails,
            // so the reference taken just above has already been released. Releasing it a
            // second time here would either free a context that the functions registered so
            // far still reference, or touch the one that was just freed.
            // The functions already registered keep their own reference and are destroyed,
            // together with the global context, when the connection is closed.
            return rc;
        }
    }
    
    return SQLITE_OK;
}
