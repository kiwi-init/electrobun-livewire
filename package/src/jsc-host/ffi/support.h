// Hand-written support for the generated bun:ffi glue (ffi_glue.c): value
// conversions with Bun's semantics, and the callback slots native code calls
// into. Included once, by the generated file.
#pragma once
#include "../runtime.h"

#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- arguments ---------------------------------------------------------------
// A pointer argument: a typed array or ArrayBuffer (its bytes), a number or
// bigint (an address), or null/undefined.
static void *ffi_arg_ptr(JSContextRef ctx, JSValueRef v) {
    if (!v || JSValueIsNull(ctx, v) || JSValueIsUndefined(ctx, v)) return NULL;
    if (JSValueIsObject(ctx, v)) {
        JSObjectRef o = (JSObjectRef)v;
        JSTypedArrayType t = JSValueGetTypedArrayType(ctx, v, NULL);
        if (t == kJSTypedArrayTypeArrayBuffer) return JSObjectGetArrayBufferBytesPtr(ctx, o, NULL);
        if (t != kJSTypedArrayTypeNone) {
            uint8_t *base = JSObjectGetTypedArrayBytesPtr(ctx, o, NULL);
            return base ? base + JSObjectGetTypedArrayByteOffset(ctx, o, NULL) : NULL;
        }
        // A JSCallback, or anything else with a numeric .ptr.
        JSStringRef name = JSStringCreateWithUTF8CString("ptr");
        JSValueRef p = JSObjectGetProperty(ctx, o, name, NULL);
        JSStringRelease(name);
        if (JSValueIsNumber(ctx, p)) return (void *)(uintptr_t)JSValueToNumber(ctx, p, NULL);
        return NULL;
    }
    if (JSValueIsBigInt(ctx, v)) return (void *)(uintptr_t)JSValueToUInt64(ctx, v, NULL);
    return (void *)(uintptr_t)JSValueToNumber(ctx, v, NULL);
}

// A cstring argument is a pointer too (the SDK passes toCString() bytes). A JS
// string is accepted as a convenience and copied into *owned.
static const char *ffi_arg_cstring(JSContextRef ctx, JSValueRef v, char **owned) {
    if (v && JSValueIsString(ctx, v)) {
        *owned = jsr_to_utf8(ctx, v);
        return *owned;
    }
    return ffi_arg_ptr(ctx, v);
}
static uint64_t ffi_arg_u64(JSContextRef ctx, JSValueRef v) {
    if (!v) return 0;
    if (JSValueIsBigInt(ctx, v)) return JSValueToUInt64(ctx, v, NULL);
    return (uint64_t)JSValueToNumber(ctx, v, NULL);
}
#define FFI_ARG(i) ((i) < argc ? argv[i] : NULL)
static double ffi_arg_num(JSContextRef ctx, JSValueRef v) { return v ? JSValueToNumber(ctx, v, NULL) : 0; }
static bool ffi_arg_bool(JSContextRef ctx, JSValueRef v) { return v ? JSValueToBoolean(ctx, v) : false; }

// --- returns -----------------------------------------------------------------
static JSObjectRef g_cstring_class; // the shim's CString constructor
static JSValueRef ffi_ret_ptr(JSContextRef ctx, const void *p) {
    return p ? JSValueMakeNumber(ctx, (double)(uintptr_t)p) : JSValueMakeNull(ctx);
}
static JSValueRef ffi_ret_cstring(JSContextRef ctx, const char *p) {
    if (!p) return JSValueMakeNull(ctx);
    JSValueRef args[1] = {ffi_ret_ptr(ctx, p)};
    if (!g_cstring_class) return jsr_string(ctx, p);
    return JSObjectCallAsConstructor(ctx, g_cstring_class, 1, args, NULL);
}
static JSValueRef ffi_ret_u64(JSContextRef ctx, uint64_t v) { return JSBigIntCreateWithUInt64(ctx, v, NULL); }

// --- callbacks -----------------------------------------------------------------
// Each signature gets a fixed set of generated trampolines ("slots"). A
// JSCallback takes a free slot; native code calls the slot's trampoline.
// On the JS thread the call is direct. From another thread a threadsafe
// callback is queued to the JS thread and returns 0 at once (Bun's behaviour);
// one that isn't waits for the JS thread, up to a timeout.
typedef struct ffi_slot {
    JSObjectRef fn;      // NULL when free
    bool threadsafe;
    char *last_cstring;  // keeps a cstring return alive until the next call
} ffi_slot;

typedef struct ffi_call {
    ffi_slot *slot;
    int argc;
    JSValueRef (*make_args)(JSContextRef ctx, struct ffi_call *call, JSValueRef *out);
    union { uint32_t u32; int32_t i32; uint64_t u64; double f64; bool b; void *p; char *s; } a[8];
    bool owns_string[8];
    JSValueRef result;
    dispatch_semaphore_t done; // set when someone waits for the result
    bool timed_out;
} ffi_call;

static jsr_runtime *g_ffi_rt;

static void ffi_free_call(ffi_call *call) {
    for (int i = 0; i < call->argc; i++)
        if (call->owns_string[i]) free(call->a[i].s);
    free(call);
}

static void ffi_run_call(jsr_runtime *rt, void *data) {
    ffi_call *call = data;
    JSContextRef ctx = jsr_context(rt);
    JSValueRef args[8];
    call->make_args(ctx, call, args);
    call->result = call->slot->fn ? jsr_call(rt, call->slot->fn, NULL, (size_t)call->argc, args) : NULL;
    if (call->done) {
        if (call->result) JSValueProtect(ctx, call->result);
        dispatch_semaphore_signal(call->done);
    } else {
        ffi_free_call(call);
    }
}

// Runs the call on the JS thread; returns the JS result (protected when it
// came from another thread, see ffi_release_result) or NULL.
static JSValueRef ffi_dispatch(ffi_call *call, bool wants_result) {
    jsr_runtime *rt = g_ffi_rt;
    if (jsr_on_js_thread(rt)) {
        call->done = NULL;
        JSContextRef ctx = jsr_context(rt);
        JSValueRef args[8];
        call->make_args(ctx, call, args);
        return call->slot->fn ? jsr_call(rt, call->slot->fn, NULL, (size_t)call->argc, args) : NULL;
    }
    if (call->slot->threadsafe || !wants_result) {
        call->done = NULL;
        jsr_post(rt, ffi_run_call, call);
        return NULL;
    }
    call->done = dispatch_semaphore_create(0);
    jsr_post(rt, ffi_run_call, call);
    if (dispatch_semaphore_wait(call->done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) != 0) {
        fprintf(stderr, "[jsc-ffi] a non-threadsafe callback timed out waiting for the JS thread\n");
        call->timed_out = true;
        return NULL; // the call still runs later; it leaks its record
    }
    return call->result;
}

static double ffi_result_num(JSValueRef r) { return r ? JSValueToNumber(jsr_context(g_ffi_rt), r, NULL) : 0; }
static const char *ffi_result_cstring(ffi_slot *slot, JSValueRef r) {
    JSContextRef ctx = jsr_context(g_ffi_rt);
    if (!r || JSValueIsNull(ctx, r) || JSValueIsUndefined(ctx, r)) return NULL;
    if (JSValueIsString(ctx, r)) {
        free(slot->last_cstring);
        slot->last_cstring = jsr_to_utf8(ctx, r);
        return slot->last_cstring;
    }
    return ffi_arg_ptr(ctx, r); // ptr(bytes) the callback keeps alive
}
static void ffi_release_result(JSValueRef r, bool from_other_thread) {
    if (r && from_other_thread) JSValueUnprotect(jsr_context(g_ffi_rt), r);
}
