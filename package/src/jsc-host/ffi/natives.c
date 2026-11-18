// The natives behind the bun:ffi shim (bun-ffi.js), installed as
// globalThis.__ffi: symbols and JSCallbacks through libffi (dynamic.inc), from
// the types each declaration gives at run time, and Bun's pointer helpers.
#include "support.h"
#include "natives.h"

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

// callbackCreate(sigKey, fn, threadsafe) -> the libffi closure's address
NATIVE(n_callback_create) {
    NATIVE_UNUSED;
    if (argc < 2 || !JSValueIsObject(ctx, argv[1])) return ffi_throw(ctx, exc, "callbackCreate(sig, fn, threadsafe)");
    if (jsr_from_context(ctx) != g_ffi_rt) return ffi_throw(ctx, exc, "JSCallback isn't available in a Worker on the JSC runtime");
    char *key = jsr_to_utf8(ctx, argv[0]);
    bool threadsafe = argc > 2 && JSValueToBoolean(ctx, argv[2]);
    const char *error = NULL;
    void *code = ffi_dyn_callback_create(ctx, key, (JSObjectRef)argv[1], threadsafe, &error);
    free(key);
    return code ? JSValueMakeNumber(ctx, (double)(uintptr_t)code) : ffi_throw(ctx, exc, error);
}

NATIVE(n_callback_close) {
    NATIVE_UNUSED;
    void *p = argc > 0 ? ffi_arg_ptr(ctx, argv[0]) : NULL;
    if (p) ffi_dyn_callback_close(ctx, p);
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
    else if (!strcmp(kind, "u64")) v = ffi_bigint_from_u64(ctx, READ(uint64_t));
    else if (!strcmp(kind, "i64")) v = ffi_bigint_from_i64(ctx, READ(int64_t));
    else if (!strcmp(kind, "ptr") || !strcmp(kind, "intptr")) v = JSValueMakeNumber(ctx, (double)READ(uintptr_t));
    else v = NULL;
#undef READ
    free(kind);
    return v ? v : ffi_throw(ctx, exc, "read: unknown kind");
}

void ffi_install(jsr_runtime *rt) {
    if (!g_ffi_rt) g_ffi_rt = rt; // JSCallbacks run on the main runtime
    JSContextRef ctx = jsr_context(rt);
    JSObjectRef ffi = JSObjectMake(ctx, NULL, NULL);
    jsr_set_function(ctx, ffi, "bind", n_bind);
    jsr_set_function(ctx, ffi, "callbackCreate", n_callback_create);
    jsr_set_function(ctx, ffi, "callbackClose", n_callback_close);
    jsr_set_function(ctx, ffi, "ptr", n_ptr);
    jsr_set_function(ctx, ffi, "readCString", n_read_cstring);
    jsr_set_function(ctx, ffi, "toArrayBuffer", n_to_array_buffer);
    jsr_set_function(ctx, ffi, "read", n_read);
    ffi_set(ctx, JSContextGetGlobalObject(ctx), "__ffi", ffi);
}
