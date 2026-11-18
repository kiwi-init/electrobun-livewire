// Support for bun:ffi's natives (natives.c, dynamic.inc): value conversions
// with Bun's semantics, and the dispatch of a JSCallback native code calls
// into. Included once, by natives.c.
#pragma once
#include "../runtime.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// LIVEWIRE_FFI_TRACE=1 prints every native call and callback to stderr, with
// whether it ran on the JS thread: for debugging a host on a new platform.
// LIVEWIRE_FFI_TRACE=<path> appends the same to that file, each line with the
// wall-clock time in ms (performance.timeOrigin's clock), plus how long a
// native call took (when 1 ms or more) and how long a callback queued for the
// JS thread waited: for finding where startup time goes.
static int g_ffi_trace = -1;
static FILE *g_ffi_trace_out;
static pthread_mutex_t g_ffi_trace_lock = PTHREAD_MUTEX_INITIALIZER;
static jsr_runtime *g_ffi_rt;
static double ffi_trace_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}
static bool ffi_tracing(void) {
    if (g_ffi_trace < 0) {
        const char *to = getenv("LIVEWIRE_FFI_TRACE");
        g_ffi_trace = to != NULL;
        if (to && strchr(to, '/')) g_ffi_trace_out = fopen(to, "a");
    }
    return g_ffi_trace > 0;
}
__attribute__((format(printf, 1, 2))) static void ffi_trace_line(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    bool js = g_ffi_rt && jsr_on_js_thread(g_ffi_rt);
    pthread_mutex_lock(&g_ffi_trace_lock);
    if (g_ffi_trace_out) {
        fprintf(g_ffi_trace_out, "%.1f %d %s %s\n", ffi_trace_now(), (int)getpid(), js ? "js" : "other", line);
        fflush(g_ffi_trace_out);
    } else {
        fprintf(stderr, "[ffi] %s%s\n", line, js ? "" : " (other thread)");
    }
    pthread_mutex_unlock(&g_ffi_trace_lock);
}
#define FFI_TRACE(what) do { if (ffi_tracing()) ffi_trace_line("%s", what); } while (0)

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
// Each JSCallback is a libffi closure (dynamic.inc) with a slot: the JS
// function it runs, or none once closed; native code calls the closure.
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
// mutex and a condition variable, instead of a dispatch semaphore). Waits are
// measured on a monotonic clock, so a wall-clock change can't stretch them.
typedef struct ffi_waiter {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool signaled;
} ffi_waiter;
static ffi_waiter *ffi_waiter_create(void) {
    ffi_waiter *w = calloc(1, sizeof *w);
    pthread_mutex_init(&w->lock, NULL);
#ifdef __APPLE__
    pthread_cond_init(&w->cond, NULL); // waits use pthread_cond_timedwait_relative_np
#else
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&w->cond, &attr);
    pthread_condattr_destroy(&attr);
#endif
    return w;
}
static void ffi_waiter_signal(ffi_waiter *w) {
    pthread_mutex_lock(&w->lock);
    w->signaled = true;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
}
static int64_t ffi_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
// True once signaled, false after timeout_ms.
static bool ffi_waiter_wait_ms(ffi_waiter *w, int64_t timeout_ms) {
    int64_t end = ffi_now_ms() + timeout_ms;
    pthread_mutex_lock(&w->lock);
#ifdef __APPLE__
    for (int64_t left = timeout_ms; !w->signaled && left > 0; left = end - ffi_now_ms()) {
        struct timespec rel = {(time_t)(left / 1000), (long)(left % 1000) * 1000000L};
        pthread_cond_timedwait_relative_np(&w->cond, &w->lock, &rel);
    }
#else
    struct timespec deadline = {(time_t)(end / 1000), (long)(end % 1000) * 1000000L};
    int rc = 0;
    while (!w->signaled && rc != ETIMEDOUT) rc = pthread_cond_timedwait(&w->cond, &w->lock, &deadline);
#endif
    bool ok = w->signaled;
    pthread_mutex_unlock(&w->lock);
    return ok;
}

typedef struct ffi_cb_call {
    _Atomic int refs;          // the closure and the queued task; the last frees it
    _Atomic int claimed;       // 0 until a thread starts running it (exactly once)
    ffi_slot *slot;
    int argc;
    JSValueRef (*make_args)(JSContextRef ctx, struct ffi_cb_call *call, JSValueRef *out);
    union { uint32_t u32; int32_t i32; uint64_t u64; double f64; bool b; void *p; char *s; } a[8];
    bool owns_string[8];
    JSValueRef result;
    ffi_waiter *done;          // set when someone waits for the result
    bool timed_out;
    const char *name;          // for the trace: the callback's signature
    double made_at;            // for the trace: when native code called it
} ffi_cb_call;
// A callback that waited for the JS thread, in the trace.
static void ffi_trace_ran(const ffi_cb_call *call, const char *how) {
    if (ffi_tracing() && call->made_at > 0) ffi_trace_line("ran %s %s after %.1f ms", call->name ? call->name : "callback", how, ffi_trace_now() - call->made_at);
}

// --- native calls and borrowed JS --------------------------------------------
// While the JS thread is inside a native call, JavaScriptCore has dropped its
// lock, so another thread can run JS. A sync callback that arrives then (WebKit
// asking for a views:// page's MIME type on the main thread while JS waits on
// the main thread in createWebview) runs on the calling thread, as Bun's
// JSCallback does, instead of waiting for a JS thread that's waiting for it.
// One borrower at a time; a thread that can't take the borrow lock (another
// borrower, possibly one blocked on this thread) queues and waits instead.
static _Atomic int g_ffi_in_native;
static _Thread_local int t_ffi_borrowed; // this thread is running JS it borrowed
static pthread_mutex_t g_ffi_borrow_lock = PTHREAD_MUTEX_INITIALIZER;
// Only the main runtime's JS can be borrowed; a Worker's native calls don't count.
static inline void ffi_enter_native(void) { if (jsr_on_js_thread(g_ffi_rt)) atomic_fetch_add(&g_ffi_in_native, 1); }
static inline void ffi_leave_native(void) { if (jsr_on_js_thread(g_ffi_rt)) atomic_fetch_sub(&g_ffi_in_native, 1); }
static inline bool ffi_runs_js_here(void) { return t_ffi_borrowed > 0 || jsr_on_js_thread(g_ffi_rt); }

static void ffi_free_call(ffi_cb_call *call) {
    for (int i = 0; i < call->argc; i++)
        if (call->owns_string[i]) free(call->a[i].s);
    if (call->done) {
        pthread_mutex_destroy(&call->done->lock);
        pthread_cond_destroy(&call->done->cond);
        free(call->done);
    }
    free(call);
}
static void ffi_call_unref(ffi_cb_call *call) {
    if (atomic_fetch_sub(&call->refs, 1) == 1) ffi_free_call(call);
}

// Runs a call with the borrow lock held (the caller took it).
static void ffi_run_borrowed(ffi_cb_call *call) {
    ffi_trace_ran(call, "borrowed");
    t_ffi_borrowed++;
    JSContextRef ctx = jsr_context(g_ffi_rt);
    JSValueRef args[8];
    call->make_args(ctx, call, args);
    call->result = call->slot->fn ? jsr_call(g_ffi_rt, call->slot->fn, NULL, (size_t)call->argc, args) : NULL;
    if (call->result) JSValueProtect(ctx, call->result); // released by the closure
    t_ffi_borrowed--;
    pthread_mutex_unlock(&g_ffi_borrow_lock);
}

static void ffi_run_call(jsr_runtime *rt, void *data) {
    ffi_cb_call *call = data;
    if (call->done && atomic_exchange(&call->claimed, 1)) { // the waiter ran it
        ffi_call_unref(call);
        return;
    }
    ffi_trace_ran(call, "queued");
    JSContextRef ctx = jsr_context(rt);
    JSValueRef args[8];
    call->make_args(ctx, call, args);
    call->result = call->slot->fn ? jsr_call(rt, call->slot->fn, NULL, (size_t)call->argc, args) : NULL;
    if (call->done) {
        if (call->result) JSValueProtect(ctx, call->result);
        ffi_waiter_signal(call->done);
        ffi_call_unref(call);
    } else {
        ffi_free_call(call);
    }
}

// Runs the call on the JS thread; returns the JS result (protected when it
// came from another thread, see ffi_release_result) or NULL.
static JSValueRef ffi_dispatch(ffi_cb_call *call, bool wants_result) {
    jsr_runtime *rt = g_ffi_rt;
    if (ffi_runs_js_here() && !call->slot->threadsafe) {
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
    // JS parked in a native call: run it here.
    call->done = NULL;
    call->refs = 1;
    if (atomic_load(&g_ffi_in_native) > 0 && pthread_mutex_trylock(&g_ffi_borrow_lock) == 0) {
        call->claimed = 1;
        ffi_run_borrowed(call);
        return call->result;
    }
    // Otherwise queue it for the JS thread, and take it back if the JS thread
    // enters a native call before running it. The queued task holds a
    // reference: it may run (and find the call claimed) after we return.
    call->done = ffi_waiter_create();
    call->refs = 2;
    jsr_post(rt, ffi_run_call, call);
    for (int64_t waited_ms = 0; waited_ms < 3000; waited_ms += 2) {
        if (ffi_waiter_wait_ms(call->done, 2)) return call->result;
        if (atomic_load(&g_ffi_in_native) > 0 && pthread_mutex_trylock(&g_ffi_borrow_lock) == 0) {
            if (!atomic_exchange(&call->claimed, 1)) {
                ffi_run_borrowed(call);
                return call->result;
            }
            pthread_mutex_unlock(&g_ffi_borrow_lock); // the JS thread is running it
        }
    }
    if (ffi_waiter_wait_ms(call->done, 0)) return call->result;
    fprintf(stderr, "[jsc-ffi] a non-threadsafe callback timed out waiting for the JS thread\n");
    call->timed_out = true;
    return NULL; // the call still runs later; its result stays protected
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
