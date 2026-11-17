// Livewire JSC runtime: the event loop and core globals a main process needs on
// the system JavaScriptCore (JavaScriptCore.framework on macOS, WebKitGTK's
// libjavascriptcoregtk on Linux). One JS thread owns the context;
// other threads hand it work through jsr_post().
#pragma once
#include <JavaScriptCore/JavaScript.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct jsr_runtime jsr_runtime;

// Creates the context and installs globals on the calling thread, which
// becomes the JS thread. argv is exposed as process.argv. The first runtime
// created is the process's main one; each Worker gets another (worker.c).
jsr_runtime *jsr_create(int argc, const char **argv);
// What a host installs on every runtime it doesn't create itself (a Worker's):
// its natives (ffi_install, node_install).
typedef void (*jsr_init_fn)(jsr_runtime *rt);
void jsr_set_runtime_init(jsr_init_fn fn);
jsr_runtime *jsr_main_runtime(void);
// The calling thread's runtime, else the main one.
jsr_runtime *jsr_current(void);
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
#ifndef __APPLE__
// The runtime's own GMainContext (a GMainContext *): sources that must run on
// the JS thread attach to it. It is not the thread's default context.
void *jsr_glib_context(jsr_runtime *rt);
#endif

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
// The runtime a context belongs to (NULL: jsr_current()).
jsr_runtime *jsr_from_context(JSContextRef ctx);

// Workers (worker.c). A Worker's runtime runs on its own thread with its own
// loop and JavaScriptCore VM; messages are copied between runtimes.
struct jsr_worker;
jsr_runtime *jsr_create_worker(int argc, const char **argv, struct jsr_worker *worker);
void jsr_destroy(jsr_runtime *rt);
struct jsr_worker *jsr_worker_of(jsr_runtime *rt); // NULL in the main runtime
void jsr_worker_terminating(jsr_runtime *rt);       // thread-safe: the watchdog ends its JS
void jsr_worker_error(jsr_runtime *rt, JSValueRef exception);
int jsr_argc(jsr_runtime *rt);
const char **jsr_argv(jsr_runtime *rt);
// The Worker natives on __rt; hosts call it on every runtime.
void worker_install(jsr_runtime *rt);
