// Hand-written natives behind the bun:ffi shim (bun-ffi.js), included at the
// end of the generated ffi_glue.c so they can see its tables. What the tables
// don't have goes through libffi (dynamic.inc).

static const ffi_library *ffi_find_library(const char *key) {
    for (size_t i = 0; i < sizeof ffi_libraries / sizeof ffi_libraries[0]; i++)
        if (!strcmp(ffi_libraries[i].key, key)) return &ffi_libraries[i];
    return NULL;
}
static JSValueRef ffi_throw(JSContextRef ctx, JSValueRef *exc, const char *message) {
    JSValueRef msg = jsr_string(ctx, message);
    *exc = JSObjectMakeError(ctx, 1, &msg, NULL);
    return NULL;
}
static void ffi_set(JSContextRef ctx, JSObjectRef obj, const char *name, JSValueRef value) {
    JSStringRef n = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(ctx, obj, n, value, kJSPropertyAttributeNone, NULL);
    JSStringRelease(n);
}
#define NATIVE(name) static JSValueRef name(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc)
#define NATIVE_UNUSED (void)f; (void)self; (void)exc

#include "dynamic.inc"

// signatures(libKey) -> { symbol: "args->ret" } or null
NATIVE(n_signatures) {
    NATIVE_UNUSED;
    char *key = argc > 0 ? jsr_to_utf8(ctx, argv[0]) : strdup("");
    const ffi_library *lib = ffi_find_library(key);
    free(key);
    if (!lib) return JSValueMakeNull(ctx);
    JSObjectRef out = JSObjectMake(ctx, NULL, NULL);
    for (int i = 0; i < lib->count; i++) ffi_set(ctx, out, lib->symbols[i].name, jsr_string(ctx, lib->symbols[i].sig));
    return out;
}

// open(libKey, path, names[]) -> { symbol: function }
NATIVE(n_open) {
    NATIVE_UNUSED;
    if (argc < 3) return ffi_throw(ctx, exc, "open(libKey, path, names)");
    char *key = jsr_to_utf8(ctx, argv[0]);
    char *path = jsr_to_utf8(ctx, argv[1]);
    const ffi_library *lib = ffi_find_library(key);
    free(key);
    if (!lib) { free(path); return ffi_throw(ctx, exc, "no generated glue for this library"); }
    void *handle = dlopen(path, RTLD_NOW);
    if (!handle) {
        char message[1024];
        snprintf(message, sizeof message, "Failed to open library \"%s\": %s", path, dlerror());
        free(path);
        return ffi_throw(ctx, exc, message);
    }
    free(path);
    JSObjectRef names = (JSObjectRef)argv[2];
    JSStringRef length_name = JSStringCreateWithUTF8CString("length");
    int count = (int)JSValueToNumber(ctx, JSObjectGetProperty(ctx, names, length_name, NULL), NULL);
    JSStringRelease(length_name);
    JSObjectRef out = JSObjectMake(ctx, NULL, NULL);
    for (int i = 0; i < count; i++) {
        char *name = jsr_to_utf8(ctx, JSObjectGetPropertyAtIndex(ctx, names, (unsigned)i, NULL));
        const ffi_symbol *sym = NULL;
        for (int s = 0; s < lib->count; s++) if (!strcmp(lib->symbols[s].name, name)) { sym = &lib->symbols[s]; break; }
        void *address = sym ? dlsym(handle, name) : NULL;
        if (!address) {
            char message[512];
            snprintf(message, sizeof message, "Symbol \"%s\" not found", name);
            free(name);
            return ffi_throw(ctx, exc, message);
        }
        *sym->target = address;
        JSStringRef n = JSStringCreateWithUTF8CString(name);
        JSObjectSetProperty(ctx, out, n, JSObjectMakeFunctionWithCallback(ctx, n, sym->fn), kJSPropertyAttributeNone, NULL);
        JSStringRelease(n);
        free(name);
    }
    return out;
}

// callbackCreate(sigKey, fn, threadsafe) -> pointer number: a generated
// trampoline's when the signature has a free one, else a libffi closure's.
NATIVE(n_callback_create) {
    NATIVE_UNUSED;
    if (argc < 2 || !JSValueIsObject(ctx, argv[1])) return ffi_throw(ctx, exc, "callbackCreate(sig, fn, threadsafe)");
    if (jsr_from_context(ctx) != g_ffi_rt) return ffi_throw(ctx, exc, "JSCallback isn't available in a Worker on the JSC runtime");
    char *key = jsr_to_utf8(ctx, argv[0]);
    bool threadsafe = argc > 2 && JSValueToBoolean(ctx, argv[2]);
    for (size_t i = 0; i < sizeof ffi_callback_sigs / sizeof ffi_callback_sigs[0]; i++) {
        const ffi_callback_sig *sig = &ffi_callback_sigs[i];
        if (strcmp(sig->key, key)) continue;
        for (int s = 0; s < FFI_CALLBACK_SLOTS; s++) {
            if (sig->slots[s].fn) continue;
            free(key);
            sig->slots[s].fn = (JSObjectRef)argv[1];
            sig->slots[s].threadsafe = threadsafe;
            JSValueProtect(ctx, argv[1]);
            return JSValueMakeNumber(ctx, (double)(uintptr_t)sig->tramps[s]);
        }
    }
    const char *error = NULL;
    void *code = ffi_dyn_callback_create(ctx, key, (JSObjectRef)argv[1], threadsafe, &error);
    free(key);
    return code ? JSValueMakeNumber(ctx, (double)(uintptr_t)code) : ffi_throw(ctx, exc, error);
}

NATIVE(n_callback_close) {
    NATIVE_UNUSED;
    void *p = argc > 0 ? ffi_arg_ptr(ctx, argv[0]) : NULL;
    if (p && ffi_dyn_callback_close(ctx, p)) return JSValueMakeUndefined(ctx);
    for (size_t i = 0; p && i < sizeof ffi_callback_sigs / sizeof ffi_callback_sigs[0]; i++)
        for (int s = 0; s < FFI_CALLBACK_SLOTS; s++)
            if (ffi_callback_sigs[i].tramps[s] == p && ffi_callback_sigs[i].slots[s].fn) {
                JSValueUnprotect(ctx, ffi_callback_sigs[i].slots[s].fn);
                ffi_callback_sigs[i].slots[s].fn = NULL;
            }
    return JSValueMakeUndefined(ctx);
}

// ptr(view, byteOffset) -> number
NATIVE(n_ptr) {
    NATIVE_UNUSED;
    uint8_t *p = argc > 0 ? ffi_arg_ptr(ctx, argv[0]) : NULL;
    double offset = argc > 1 ? ffi_arg_num(ctx, argv[1]) : 0;
    return p ? JSValueMakeNumber(ctx, (double)(uintptr_t)(p + (size_t)offset)) : JSValueMakeNull(ctx);
}

// readCString(ptr, byteOffset, byteLength or -1) -> string
NATIVE(n_read_cstring) {
    NATIVE_UNUSED;
    const char *p = argc > 0 ? ffi_arg_ptr(ctx, argv[0]) : NULL;
    if (!p) return jsr_string(ctx, "");
    p += (size_t)(argc > 1 ? ffi_arg_num(ctx, argv[1]) : 0);
    double length = argc > 2 ? ffi_arg_num(ctx, argv[2]) : -1;
    if (length < 0) return jsr_string(ctx, p);
    char *copy = strndup(p, (size_t)length);
    JSValueRef v = jsr_string(ctx, copy);
    free(copy);
    return v;
}

static void ffi_free_bytes(void *bytes, void *ctx) { (void)ctx; free(bytes); }
// toArrayBuffer(ptr, byteOffset, byteLength) -> ArrayBuffer (a copy)
NATIVE(n_to_array_buffer) {
    NATIVE_UNUSED;
    const uint8_t *p = argc > 0 ? ffi_arg_ptr(ctx, argv[0]) : NULL;
    size_t offset = (size_t)(argc > 1 ? ffi_arg_num(ctx, argv[1]) : 0);
    double length = argc > 2 ? ffi_arg_num(ctx, argv[2]) : -1;
    if (!p) return ffi_throw(ctx, exc, "toArrayBuffer: null pointer");
    size_t n = length < 0 ? strlen((const char *)p + offset) : (size_t)length;
    void *copy = malloc(n ? n : 1);
    memcpy(copy, p + offset, n);
    return JSObjectMakeArrayBufferWithBytesNoCopy(ctx, copy, n, ffi_free_bytes, NULL, NULL);
}

// read(kind, ptr, byteOffset)
NATIVE(n_read) {
    NATIVE_UNUSED;
    char *kind = argc > 0 ? jsr_to_utf8(ctx, argv[0]) : strdup("");
    const uint8_t *p = argc > 1 ? ffi_arg_ptr(ctx, argv[1]) : NULL;
    if (!p) { free(kind); return ffi_throw(ctx, exc, "read: null pointer"); }
    p += (size_t)(argc > 2 ? ffi_arg_num(ctx, argv[2]) : 0);
    JSValueRef v;
#define READ(T) ({ T x; memcpy(&x, p, sizeof x); x; })
    if (!strcmp(kind, "u8")) v = JSValueMakeNumber(ctx, READ(uint8_t));
    else if (!strcmp(kind, "i8")) v = JSValueMakeNumber(ctx, READ(int8_t));
    else if (!strcmp(kind, "u16")) v = JSValueMakeNumber(ctx, READ(uint16_t));
    else if (!strcmp(kind, "i16")) v = JSValueMakeNumber(ctx, READ(int16_t));
    else if (!strcmp(kind, "u32")) v = JSValueMakeNumber(ctx, READ(uint32_t));
    else if (!strcmp(kind, "i32")) v = JSValueMakeNumber(ctx, READ(int32_t));
    else if (!strcmp(kind, "f32")) v = JSValueMakeNumber(ctx, READ(float));
    else if (!strcmp(kind, "f64")) v = JSValueMakeNumber(ctx, READ(double));
    else if (!strcmp(kind, "u64")) v = JSBigIntCreateWithUInt64(ctx, READ(uint64_t), NULL);
    else if (!strcmp(kind, "i64")) v = JSBigIntCreateWithInt64(ctx, READ(int64_t), NULL);
    else if (!strcmp(kind, "ptr") || !strcmp(kind, "intptr")) v = JSValueMakeNumber(ctx, (double)READ(uintptr_t));
    else v = NULL;
#undef READ
    free(kind);
    return v ? v : ffi_throw(ctx, exc, "read: unknown kind");
}

#include "glue.h"
void ffi_install(jsr_runtime *rt) {
    if (!g_ffi_rt) g_ffi_rt = rt; // JSCallbacks run on the main runtime
    JSContextRef ctx = jsr_context(rt);
    JSObjectRef ffi = JSObjectMake(ctx, NULL, NULL);
    jsr_set_function(ctx, ffi, "signatures", n_signatures);
    jsr_set_function(ctx, ffi, "open", n_open);
    jsr_set_function(ctx, ffi, "bind", n_bind);
    jsr_set_function(ctx, ffi, "callbackCreate", n_callback_create);
    jsr_set_function(ctx, ffi, "callbackClose", n_callback_close);
    jsr_set_function(ctx, ffi, "ptr", n_ptr);
    jsr_set_function(ctx, ffi, "readCString", n_read_cstring);
    jsr_set_function(ctx, ffi, "toArrayBuffer", n_to_array_buffer);
    jsr_set_function(ctx, ffi, "read", n_read);
    ffi_set(ctx, JSContextGetGlobalObject(ctx), "__ffi", ffi);
}
