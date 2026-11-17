// jsc-run: runs a script on the Livewire JSC runtime until the loop is idle,
// exiting with process.exitCode. Used by the runtime's own tests.
#include "runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Test hook: __test.postFromThread(fn, delayMs) calls fn on the JS thread from
// a foreign thread after delayMs, the way native callbacks arrive.
typedef struct { JSObjectRef fn; int delay_ms; } post_job;
static void call_job(jsr_runtime *rt, void *data) {
    post_job *job = data;
    JSValueRef args[1] = {JSValueMakeBoolean(jsr_context(rt), jsr_on_js_thread(rt))};
    jsr_call(rt, job->fn, NULL, 1, args);
    JSValueUnprotect(jsr_context(rt), job->fn);
    jsr_unref(rt);
    free(job);
}
static void *post_thread(void *arg) {
    post_job *job = arg;
    usleep((useconds_t)job->delay_ms * 1000);
    jsr_runtime *rt = jsr_from_context(NULL);
    jsr_post(rt, call_job, job);
    return NULL;
}
static JSValueRef t_post_from_thread(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    if (argc < 1 || !JSValueIsObject(ctx, argv[0])) return JSValueMakeUndefined(ctx);
    post_job *job = calloc(1, sizeof *job);
    job->fn = (JSObjectRef)argv[0];
    job->delay_ms = argc > 1 ? (int)JSValueToNumber(ctx, argv[1], NULL) : 0;
    JSValueProtect(ctx, job->fn);
    jsr_ref(jsr_from_context(ctx)); // the pending callback keeps the loop alive
    pthread_t t;
    pthread_create(&t, NULL, post_thread, job);
    pthread_detach(t);
    return JSValueMakeUndefined(ctx);
}

int main(int argc, const char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: jsc-run <script.js> [args...]\n");
        return 2;
    }
    jsr_runtime *rt = jsr_create(argc, argv);
    JSContextRef ctx = jsr_context(rt);
    JSObjectRef test = JSObjectMake(ctx, NULL, NULL);
    jsr_set_function(ctx, test, "postFromThread", t_post_from_thread);
    JSStringRef name = JSStringCreateWithUTF8CString("__test");
    JSObjectSetProperty(ctx, JSContextGetGlobalObject(ctx), name, test, kJSPropertyAttributeNone, NULL);
    JSStringRelease(name);
    if (jsr_eval_file(rt, argv[1])) jsr_run(rt, true);
    return jsr_exit_code(rt);
}
