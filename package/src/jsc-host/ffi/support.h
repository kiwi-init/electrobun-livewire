// Hand-written support for the generated bun:ffi glue (ffi_glue.c): value
// conversions with Bun's semantics, and the callback slots native code calls
// into. Included once, by the generated file.
#pragma once
#include "../runtime.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// LIVEWIRE_FFI_TRACE=1 prints every native call and callback to stderr, with
// whether it ran on the JS thread: for debugging a host on a new platform.
static int g_ffi_trace = -1;
#define FFI_TRACE(what) do { \
    if (g_ffi_trace < 0) g_ffi_trace = getenv("LIVEWIRE_FFI_TRACE") != NULL; \
    if (g_ffi_trace) fprintf(stderr, "[ffi] %s%s\n", what, g_ffi_rt && jsr_on_js_thread(g_ffi_rt) ? "" : " (other thread)"); \
} while (0)

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
static JSValueRef ffi_ret_ptr(JSContextRef ctx, const void *p) {
    return p ? JSValueMakeNumber(ctx, (double)(uintptr_t)p) : JSValueMakeNull(ctx);
}
static JSValueRef ffi_ret_cstring(JSContextRef ctx, const char *p) {
    return p ? jsr_string(ctx, p) : JSValueMakeNull(ctx);
}
static JSValueRef ffi_ret_u64(JSContextRef ctx, uint64_t v) { return JSBigIntCreateWithUInt64(ctx, v, NULL); }

// --- callbacks -----------------------------------------------------------------
// Each signature gets a fixed set of generated trampolines ("slots"). A
// JSCallback takes a free slot; native code calls the slot's trampoline.
// A threadsafe callback is always queued to the JS thread and returns 0 at
// once, even when called on the JS thread (Bun's behaviour). One that isn't
// is direct on the JS thread, and from another thread waits for the JS
// thread, up to a timeout.
typedef struct ffi_slot {
    JSObjectRef fn;      // NULL when free
    bool threadsafe;
    char *last_cstring;  // keeps a cstring return alive until the next call
} ffi_slot;

// A one-shot signal a foreign thread waits on for a JS result (portable: a
// mutex and a condition variable, instead of a dispatch semaphore).
typedef struct ffi_waiter {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool signaled;
} ffi_waiter;
static ffi_waiter *ffi_waiter_create(void) {
    ffi_waiter *w = calloc(1, sizeof *w);
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cond, NULL);
    return w;
}
static void ffi_waiter_signal(ffi_waiter *w) {
    pthread_mutex_lock(&w->lock);
    w->signaled = true;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
}
// True once signaled, false after timeout_s seconds.
static bool ffi_waiter_wait(ffi_waiter *w, int timeout_s) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_s;
    pthread_mutex_lock(&w->lock);
    int rc = 0;
    while (!w->signaled && rc != ETIMEDOUT) rc = pthread_cond_timedwait(&w->cond, &w->lock, &deadline);
    bool ok = w->signaled;
    pthread_mutex_unlock(&w->lock);
    return ok;
}

typedef struct ffi_call {
    ffi_slot *slot;
    int argc;
    JSValueRef (*make_args)(JSContextRef ctx, struct ffi_call *call, JSValueRef *out);
    union { uint32_t u32; int32_t i32; uint64_t u64; double f64; bool b; void *p; char *s; } a[8];
    bool owns_string[8];
    JSValueRef result;
    ffi_waiter *done;          // set when someone waits for the result
    bool timed_out;
} ffi_call;

static jsr_runtime *g_ffi_rt;

static void ffi_free_call(ffi_call *call) {
    for (int i = 0; i < call->argc; i++)
        if (call->owns_string[i]) free(call->a[i].s);
    if (call->done) {
        pthread_mutex_destroy(&call->done->lock);
        pthread_cond_destroy(&call->done->cond);
        free(call->done);
    }
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
        ffi_waiter_signal(call->done);
    } else {
        ffi_free_call(call);
    }
}

// Runs the call on the JS thread; returns the JS result (protected when it
// came from another thread, see ffi_release_result) or NULL.
static JSValueRef ffi_dispatch(ffi_call *call, bool wants_result) {
    jsr_runtime *rt = g_ffi_rt;
    if (jsr_on_js_thread(rt) && !call->slot->threadsafe) {
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
    call->done = ffi_waiter_create();
    jsr_post(rt, ffi_run_call, call);
    if (!ffi_waiter_wait(call->done, 3)) {
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
