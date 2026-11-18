// bun:ffi through libffi: every symbol (the SDK's and an app's own libraries)
// and JSCallback is bound at run time from its declared types, so a released
// host needs no rebuild for an app's libraries (T1113). The system libffi:
// macOS ships it; on Linux GLib depends on it.
// Included by natives.c, after support.h.
#ifdef __APPLE__
#include <ffi/ffi.h>
#else
#include <ffi.h>
#endif

// The canonical type names bun-ffi.js passes (its canonicalType()).
enum { DT_VOID, DT_BOOL, DT_I8, DT_U8, DT_I16, DT_U16, DT_I32, DT_U32, DT_I64, DT_U64, DT_I64_FAST, DT_U64_FAST, DT_F32, DT_F64, DT_PTR, DT_CSTRING, DT_FUNCTION, DT_COUNT };
static const char *const ffi_dyn_names[DT_COUNT] = {
    "void", "bool", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "i64_fast", "u64_fast", "f32", "f64", "ptr", "cstring", "function",
};
#define FFI_DYN_MAX_ARGS 32

static int ffi_dyn_type_named(const char *name, size_t len) {
    for (int t = 0; t < DT_COUNT; t++)
        if (strlen(ffi_dyn_names[t]) == len && !strncmp(ffi_dyn_names[t], name, len)) return t;
    return -1;
}
static ffi_type *ffi_dyn_ffi_type(int t) {
    switch (t) {
    case DT_VOID: return &ffi_type_void;
    case DT_BOOL: case DT_U8: return &ffi_type_uint8;
    case DT_I8: return &ffi_type_sint8;
    case DT_I16: return &ffi_type_sint16;
    case DT_U16: return &ffi_type_uint16;
    case DT_I32: return &ffi_type_sint32;
    case DT_U32: return &ffi_type_uint32;
    case DT_I64: case DT_I64_FAST: return &ffi_type_sint64;
    case DT_U64: case DT_U64_FAST: return &ffi_type_uint64;
    case DT_F32: return &ffi_type_float;
    case DT_F64: return &ffi_type_double;
    default: return &ffi_type_pointer;
    }
}

// One value of any type. ffi_call widens a small integer return to a whole
// ffi_arg, so the return buffer is read through word/sword (ffi_dyn_narrow).
typedef union { int8_t i8; uint8_t u8; int16_t i16; uint16_t u16; int32_t i32; uint32_t u32; int64_t i64; uint64_t u64; float f32; double f64; void *p; ffi_arg word; ffi_sarg sword; } ffi_dyn_value;
_Static_assert(sizeof(ffi_dyn_value) == sizeof(((ffi_cb_call *)0)->a[0]), "a callback argument is stored in an ffi_cb_call slot");

// A JS number as an integer: NaN is 0, and anything past int64's range is
// clamped, instead of the undefined behaviour of a plain cast.
static int64_t ffi_dyn_num_i64(double d) {
    if (!(d == d)) return 0;
    if (d >= 9223372036854775807.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)d;
}
static int64_t ffi_dyn_arg_i64(JSContextRef ctx, JSValueRef v) {
    if (!v) return 0;
    if (JSValueIsBigInt(ctx, v)) return JSValueToInt64(ctx, v, NULL);
    return ffi_dyn_num_i64(JSValueToNumber(ctx, v, NULL));
}

// A JS argument as type t; a JS string given for a cstring is copied to *owned.
static void ffi_dyn_from_js(JSContextRef ctx, int t, JSValueRef a, ffi_dyn_value *out, char **owned) {
    memset(out, 0, sizeof *out);
    switch (t) {
    case DT_BOOL: out->u8 = ffi_arg_bool(ctx, a); break;
    case DT_I8: out->i8 = (int8_t)ffi_dyn_num_i64(ffi_arg_num(ctx, a)); break;
    case DT_U8: out->u8 = (uint8_t)ffi_dyn_num_i64(ffi_arg_num(ctx, a)); break;
    case DT_I16: out->i16 = (int16_t)ffi_dyn_num_i64(ffi_arg_num(ctx, a)); break;
    case DT_U16: out->u16 = (uint16_t)ffi_dyn_num_i64(ffi_arg_num(ctx, a)); break;
    case DT_I32: out->i32 = (int32_t)ffi_dyn_num_i64(ffi_arg_num(ctx, a)); break;
    case DT_U32: out->u32 = (uint32_t)ffi_dyn_num_i64(ffi_arg_num(ctx, a)); break;
    case DT_I64: case DT_I64_FAST: out->i64 = ffi_dyn_arg_i64(ctx, a); break;
    case DT_U64: case DT_U64_FAST: out->u64 = ffi_arg_u64(ctx, a); break;
    case DT_F32: out->f32 = (float)ffi_arg_num(ctx, a); break;
    case DT_F64: out->f64 = ffi_arg_num(ctx, a); break;
    case DT_CSTRING: out->p = (void *)ffi_arg_cstring(ctx, a, owned); break;
    default: out->p = ffi_arg_ptr(ctx, a); break;
    }
}
static void ffi_dyn_narrow(int t, ffi_dyn_value *r) {
    ffi_arg w = r->word;
    switch (t) {
    case DT_BOOL: case DT_U8: r->u8 = (uint8_t)w; break;
    case DT_I8: r->i8 = (int8_t)w; break;
    case DT_U16: r->u16 = (uint16_t)w; break;
    case DT_I16: r->i16 = (int16_t)w; break;
    case DT_U32: r->u32 = (uint32_t)w; break;
    case DT_I32: r->i32 = (int32_t)w; break;
    default: break;
    }
}
// Bun's results: 64-bit integers are bigints, the _fast ones numbers when exact.
static JSValueRef ffi_dyn_to_js(JSContextRef ctx, int t, const ffi_dyn_value *v) {
    const int64_t safe = 9007199254740991LL;
    switch (t) {
    case DT_VOID: return JSValueMakeUndefined(ctx);
    case DT_BOOL: return JSValueMakeBoolean(ctx, v->u8 != 0);
    case DT_I8: return JSValueMakeNumber(ctx, v->i8);
    case DT_U8: return JSValueMakeNumber(ctx, v->u8);
    case DT_I16: return JSValueMakeNumber(ctx, v->i16);
    case DT_U16: return JSValueMakeNumber(ctx, v->u16);
    case DT_I32: return JSValueMakeNumber(ctx, v->i32);
    case DT_U32: return JSValueMakeNumber(ctx, v->u32);
    case DT_I64: return JSBigIntCreateWithInt64(ctx, v->i64, NULL);
    case DT_U64: return ffi_ret_u64(ctx, v->u64);
    case DT_I64_FAST: return v->i64 >= -safe && v->i64 <= safe ? JSValueMakeNumber(ctx, (double)v->i64) : JSBigIntCreateWithInt64(ctx, v->i64, NULL);
    case DT_U64_FAST: return v->u64 <= (uint64_t)safe ? JSValueMakeNumber(ctx, (double)v->u64) : ffi_ret_u64(ctx, v->u64);
    case DT_F32: return JSValueMakeNumber(ctx, v->f32);
    case DT_F64: return JSValueMakeNumber(ctx, v->f64);
    case DT_CSTRING: return ffi_ret_cstring(ctx, v->p);
    default: return ffi_ret_ptr(ctx, v->p);
    }
}

// --- symbols -----------------------------------------------------------------
typedef struct {
    void *fn;
    char *name;
    ffi_cif cif;
    int argc;
    int ret;
    int args[FFI_DYN_MAX_ARGS];
    ffi_type *types[FFI_DYN_MAX_ARGS];
} ffi_dyn_fn;

static JSValueRef ffi_dyn_call(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)self; (void)exc;
    ffi_dyn_fn *d = JSObjectGetPrivate(f);
    if (!d) return JSValueMakeUndefined(ctx);
    if (ffi_tracing() && !g_ffi_trace_out) ffi_trace_line("%s", d->name); // to a file: only slow calls, below
    ffi_dyn_value v[FFI_DYN_MAX_ARGS];
    void *values[FFI_DYN_MAX_ARGS];
    char *owned[FFI_DYN_MAX_ARGS];
    int nowned = 0;
    for (int i = 0; i < d->argc; i++) {
        char *own = NULL;
        ffi_dyn_from_js(ctx, d->args[i], FFI_ARG((size_t)i), &v[i], &own);
        if (own) owned[nowned++] = own;
        values[i] = &v[i];
    }
    ffi_dyn_value r;
    memset(&r, 0, sizeof r);
    double started = g_ffi_trace > 0 ? ffi_trace_now() : 0;
    ffi_enter_native();
    ffi_call(&d->cif, FFI_FN(d->fn), &r, values);
    ffi_leave_native();
    if (started > 0 && ffi_trace_now() - started >= 1.0) ffi_trace_line("done %s after %.1f ms", d->name, ffi_trace_now() - started);
    for (int i = 0; i < nowned; i++) free(owned[i]);
    ffi_dyn_narrow(d->ret, &r);
    return ffi_dyn_to_js(ctx, d->ret, &r);
}
static void ffi_dyn_finalize(JSObjectRef o) {
    ffi_dyn_fn *d = JSObjectGetPrivate(o);
    if (d) free(d->name);
    free(d);
}
static JSClassRef g_ffi_dyn_class;
static void ffi_dyn_make_class(void) {
    JSClassDefinition def = kJSClassDefinitionEmpty;
    def.className = "FFIFunction";
    def.callAsFunction = ffi_dyn_call;
    def.finalize = ffi_dyn_finalize;
    g_ffi_dyn_class = JSClassCreate(&def);
}

static JSValueRef ffi_dyn_get(JSContextRef ctx, JSObjectRef o, const char *name) {
    JSStringRef n = JSStringCreateWithUTF8CString(name);
    JSValueRef v = JSObjectGetProperty(ctx, o, n, NULL);
    JSStringRelease(n);
    return v;
}

// bind(path, name, [arg types], return type) -> a function calling the symbol
// through libffi. Types are canonical names; bun-ffi.js maps Bun's aliases.
NATIVE(n_bind) {
    NATIVE_UNUSED;
    if (argc < 4 || !JSValueIsObject(ctx, argv[2])) return ffi_throw(ctx, exc, "bind(path, name, args, returns)");
    char *path = jsr_to_utf8(ctx, argv[0]);
    void *handle = dlopen(path, RTLD_NOW);
    if (!handle) {
        char message[1024];
        snprintf(message, sizeof message, "Failed to open library \"%s\": %s", path, dlerror());
        free(path);
        return ffi_throw(ctx, exc, message);
    }
    free(path);
    char *name = jsr_to_utf8(ctx, argv[1]);
    char message[512];
    void *address = dlsym(handle, name);
    if (!address) {
        snprintf(message, sizeof message, "Symbol \"%s\" not found", name);
        free(name);
        return ffi_throw(ctx, exc, message);
    }
    JSObjectRef list = (JSObjectRef)argv[2];
    double count = JSValueToNumber(ctx, ffi_dyn_get(ctx, list, "length"), NULL);
    if (!(count >= 0 && count <= FFI_DYN_MAX_ARGS)) {
        snprintf(message, sizeof message, "bun:ffi: %s takes more than %d arguments", name, FFI_DYN_MAX_ARGS);
        free(name);
        return ffi_throw(ctx, exc, message);
    }
    ffi_dyn_fn *d = calloc(1, sizeof *d);
    d->fn = address;
    d->name = name;
    d->argc = (int)count;
    for (int i = 0; i <= d->argc; i++) {
        JSValueRef tv = i < d->argc ? JSObjectGetPropertyAtIndex(ctx, list, (unsigned)i, NULL) : argv[3];
        char *type = jsr_to_utf8(ctx, tv);
        int t = ffi_dyn_type_named(type, strlen(type));
        if (t < 0 || (i < d->argc && t == DT_VOID)) {
            snprintf(message, sizeof message, "bun:ffi: %s: unsupported %s type %s", name, i < d->argc ? "argument" : "return", type);
            free(type);
            free(d->name);
            free(d);
            return ffi_throw(ctx, exc, message);
        }
        free(type);
        if (i < d->argc) {
            d->args[i] = t;
            d->types[i] = ffi_dyn_ffi_type(t);
        } else {
            d->ret = t;
        }
    }
    if (ffi_prep_cif(&d->cif, FFI_DEFAULT_ABI, (unsigned)d->argc, ffi_dyn_ffi_type(d->ret), d->types) != FFI_OK) {
        snprintf(message, sizeof message, "bun:ffi: libffi can't call %s with these types", name);
        free(d->name);
        free(d);
        return ffi_throw(ctx, exc, message);
    }
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, ffi_dyn_make_class);
    JSObjectRef fn = JSObjectMake(ctx, g_ffi_dyn_class, d);
    // A real function's prototype, so call/apply/bind work on it.
    JSValueRef function = ffi_dyn_get(ctx, JSContextGetGlobalObject(ctx), "Function");
    if (JSValueIsObject(ctx, function)) JSObjectSetPrototype(ctx, fn, ffi_dyn_get(ctx, (JSObjectRef)function, "prototype"));
    return fn;
}

// --- callbacks ---------------------------------------------------------------
// A libffi closure per JSCallback, run through support.h's dispatch. A closed
// one is kept for the next callback of its signature instead of freed: native
// code may still hold its address, and calling it then finds no function and
// returns 0.
typedef struct ffi_dyn_cb {
    ffi_slot slot; // first: make_args finds the callback from call->slot
    ffi_closure *closure;
    void *code;
    ffi_cif cif;
    char *key;
    int argc;
    int ret;
    int args[8];
    ffi_type *types[8];
    struct ffi_dyn_cb *next;
} ffi_dyn_cb;
static ffi_dyn_cb *g_ffi_dyn_cbs; // created and closed on the main JS thread

static JSValueRef ffi_dyn_cb_args(JSContextRef ctx, ffi_cb_call *call, JSValueRef *out) {
    const ffi_dyn_cb *cb = (const ffi_dyn_cb *)call->slot;
    for (int i = 0; i < cb->argc; i++) {
        ffi_dyn_value v;
        memcpy(&v, &call->a[i], sizeof v);
        out[i] = ffi_dyn_to_js(ctx, cb->args[i], &v);
    }
    return NULL;
}

// A JS result written back as the closure's return (small integers widened
// to an ffi_arg, as libffi expects).
static void ffi_dyn_cb_return(ffi_dyn_cb *cb, JSValueRef r, void *ret) {
    JSContextRef ctx = jsr_context(g_ffi_rt);
    int64_t n = 0;
    switch (cb->ret) {
    case DT_I8: case DT_U8: case DT_I16: case DT_U16: case DT_I32: case DT_U32: n = ffi_dyn_num_i64(ffi_result_num(r)); break;
    default: break;
    }
    switch (cb->ret) {
    case DT_VOID: break;
    case DT_BOOL: *(ffi_arg *)ret = r ? JSValueToBoolean(ctx, r) : 0; break;
    case DT_I8: *(ffi_sarg *)ret = (int8_t)n; break;
    case DT_U8: *(ffi_arg *)ret = (uint8_t)n; break;
    case DT_I16: *(ffi_sarg *)ret = (int16_t)n; break;
    case DT_U16: *(ffi_arg *)ret = (uint16_t)n; break;
    case DT_I32: *(ffi_sarg *)ret = (int32_t)n; break;
    case DT_U32: *(ffi_arg *)ret = (uint32_t)n; break;
    case DT_I64: case DT_I64_FAST: *(int64_t *)ret = r ? ffi_dyn_arg_i64(ctx, r) : 0; break;
    case DT_U64: case DT_U64_FAST: *(uint64_t *)ret = r ? ffi_arg_u64(ctx, r) : 0; break;
    case DT_F32: *(float *)ret = (float)ffi_result_num(r); break;
    case DT_F64: *(double *)ret = ffi_result_num(r); break;
    case DT_CSTRING: *(const char **)ret = ffi_result_cstring(&cb->slot, r); break;
    default: *(void **)ret = ffi_arg_ptr(ctx, r); break;
    }
}

static void ffi_dyn_cb_run(ffi_cif *cif, void *ret, void **args, void *user) {
    (void)cif;
    ffi_dyn_cb *cb = user;
    ffi_cb_call *call = calloc(1, sizeof *call);
    call->slot = &cb->slot;
    call->name = cb->key;
    call->argc = cb->argc;
    call->make_args = ffi_dyn_cb_args;
    for (int i = 0; i < cb->argc; i++) {
        ffi_dyn_value v;
        memset(&v, 0, sizeof v);
        memcpy(&v, args[i], cb->types[i]->size);
        if (cb->args[i] == DT_CSTRING) { // copied: native code may free it once we return
            call->a[i].s = v.p ? strdup(v.p) : NULL;
            call->owns_string[i] = true;
        } else {
            memcpy(&call->a[i], &v, sizeof v);
        }
    }
    if (ffi_tracing()) { // the first string argument says which event it is
        const char *text = NULL;
        for (int i = 0; i < cb->argc && !text; i++) if (cb->args[i] == DT_CSTRING) text = call->a[i].s;
        ffi_trace_line("callback %s%s %.80s", cb->key, cb->slot.threadsafe ? " threadsafe" : "", text ? text : "");
        call->made_at = ffi_trace_now();
    }
    bool wants = cb->ret != DT_VOID;
    bool direct = ffi_runs_js_here() && !cb->slot.threadsafe;
    bool waits = !ffi_runs_js_here() && wants && !cb->slot.threadsafe;
    JSValueRef r = ffi_dispatch(call, wants);
    ffi_dyn_cb_return(cb, r, ret);
    if (direct) ffi_free_call(call);
    else if (waits) { if (!call->timed_out) ffi_release_result(r, true); ffi_call_unref(call); }
}

// The closure for a signature key ("u32,cstring->void"), set to call fn. NULL
// with *error set when the key has a type libffi callbacks don't take.
static void *ffi_dyn_callback_create(JSContextRef ctx, const char *key, JSObjectRef fn, bool threadsafe, const char **error) {
    ffi_dyn_cb *cb = NULL;
    for (ffi_dyn_cb *c = g_ffi_dyn_cbs; c && !cb; c = c->next)
        if (!c->slot.fn && !strcmp(c->key, key)) cb = c;
    if (!cb) {
        cb = calloc(1, sizeof *cb);
        const char *arrow = strstr(key, "->");
        bool ok = arrow != NULL;
        if (ok) {
            cb->ret = ffi_dyn_type_named(arrow + 2, strlen(arrow + 2));
            ok = cb->ret >= 0;
        }
        for (const char *at = key; ok && at < arrow;) {
            const char *end = memchr(at, ',', (size_t)(arrow - at));
            if (!end) end = arrow;
            int t = ffi_dyn_type_named(at, (size_t)(end - at));
            if (t < 0 || t == DT_VOID || cb->argc == 8) { ok = false; break; }
            cb->args[cb->argc] = t;
            cb->types[cb->argc++] = ffi_dyn_ffi_type(t);
            at = end + 1;
        }
        if (!ok) {
            free(cb);
            *error = "JSCallback: unsupported signature (at most 8 arguments, of bun:ffi's number, bool, pointer and cstring types)";
            return NULL;
        }
        cb->closure = ffi_closure_alloc(sizeof(ffi_closure), &cb->code);
        if (!cb->closure ||
            ffi_prep_cif(&cb->cif, FFI_DEFAULT_ABI, (unsigned)cb->argc, ffi_dyn_ffi_type(cb->ret), cb->types) != FFI_OK ||
            ffi_prep_closure_loc(cb->closure, &cb->cif, ffi_dyn_cb_run, cb, cb->code) != FFI_OK) {
            if (cb->closure) ffi_closure_free(cb->closure);
            free(cb);
            *error = "JSCallback: libffi couldn't make a closure";
            return NULL;
        }
        cb->key = strdup(key);
        cb->next = g_ffi_dyn_cbs;
        g_ffi_dyn_cbs = cb;
    }
    cb->slot.threadsafe = threadsafe;
    cb->slot.fn = fn;
    JSValueProtect(ctx, fn);
    return cb->code;
}

static bool ffi_dyn_callback_close(JSContextRef ctx, void *code) {
    for (ffi_dyn_cb *c = g_ffi_dyn_cbs; c; c = c->next) {
        if (c->code != code) continue;
        if (c->slot.fn) JSValueUnprotect(ctx, c->slot.fn);
        c->slot.fn = NULL;
        return true;
    }
    return false;
}
