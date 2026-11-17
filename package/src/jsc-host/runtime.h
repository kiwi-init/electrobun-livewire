// Livewire JSC runtime: the event loop and core globals a main process needs on
// the system JavaScriptCore.framework (macOS). One JS thread owns the context;
// other threads hand it work through jsr_post().
#pragma once
#include <JavaScriptCore/JavaScriptCore.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct jsr_runtime jsr_runtime;

// Creates the context and installs globals on the calling thread, which
// becomes the JS thread. argv is exposed as process.argv.
jsr_runtime *jsr_create(int argc, const char **argv);
JSGlobalContextRef jsr_context(jsr_runtime *rt);

// Evaluates a classic script. Returns false (and reports) on an uncaught error.
bool jsr_eval(jsr_runtime *rt, const char *source, const char *url);
bool jsr_eval_file(jsr_runtime *rt, const char *path);

// Runs the JS thread's run loop. With until_idle, returns once nothing keeps
// the loop alive (no ref'd timers or handles, no queued work); otherwise runs
// until process.exit() or jsr_stop().
void jsr_run(jsr_runtime *rt, bool until_idle);
void jsr_stop(jsr_runtime *rt);
int jsr_exit_code(jsr_runtime *rt);

// Thread-safe: runs fn(rt, data) on the JS thread, inside the loop.
typedef void (*jsr_task_fn)(jsr_runtime *rt, void *data);
void jsr_post(jsr_runtime *rt, jsr_task_fn fn, void *data);
bool jsr_on_js_thread(jsr_runtime *rt);

// Handles that keep the loop alive while ref'd (sockets, child processes).
void jsr_ref(jsr_runtime *rt);
void jsr_unref(jsr_runtime *rt);

// Calls a JS function on the JS thread and reports an uncaught exception the
// way Node does (process 'uncaughtException' listeners, else print).
JSValueRef jsr_call(jsr_runtime *rt, JSObjectRef fn, JSObjectRef self, size_t argc, const JSValueRef argv[]);
void jsr_report_exception(jsr_runtime *rt, JSValueRef exception);

// Native helpers on the internal __rt object.
void jsr_set_function(JSContextRef ctx, JSObjectRef obj, const char *name, JSObjectCallAsFunctionCallback cb);
JSObjectRef jsr_internal(jsr_runtime *rt);
char *jsr_to_utf8(JSContextRef ctx, JSValueRef value);
JSValueRef jsr_string(JSContextRef ctx, const char *utf8);
jsr_runtime *jsr_from_context(JSContextRef ctx);
