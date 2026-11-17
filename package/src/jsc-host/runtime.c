// See runtime.h. The loop is the JS thread's CFRunLoop on macOS and its own
// GMainContext on Linux: timers are CFRunLoopTimers / GLib timeout sources,
// cross-thread work is a CFRunLoopSource / an idle source, and JSC drains
// promise jobs when each outermost API call into JS returns, so a callback that
// resolves a promise runs its continuations before the loop moves on.
#include "runtime.h"
#include "prelude.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#else
#include <glib.h>
#include <glib-unix.h>
#include <time.h>
#endif
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

extern char **environ;

typedef struct jsr_task {
    jsr_task_fn fn;
    void *data;
    struct jsr_task *next;
} jsr_task;

typedef struct jsr_timer {
    uint32_t id;
    JSObjectRef fn;
#ifdef __APPLE__
    CFRunLoopTimerRef cf;
#else
    GSource *source;
#endif
    bool repeat;
    bool refed;
    struct jsr_timer *next;
} jsr_timer;

struct jsr_runtime {
    JSGlobalContextRef ctx;
    JSObjectRef internal;      // __rt: natives the prelude wraps
    JSObjectRef dispatch_fn;   // prelude's __rt.dispatchEvent(name, ...args)
#ifdef __APPLE__
    CFRunLoopRef loop;
    CFRunLoopSourceRef source;
#else
    GMainContext *gctx;
    bool drain_scheduled;      // an idle source for drain_tasks is attached (under lock)
#endif
    pthread_t thread;
    pthread_mutex_t lock;
    jsr_task *head, *tail;
    jsr_timer *timers;
    uint32_t next_timer_id;
    int refs;                  // ref'd timers + handles
    bool stopped;
    int exit_code;
    int argc;
    const char **argv;
};

static jsr_runtime *g_runtime; // one runtime per process for now

jsr_runtime *jsr_from_context(JSContextRef ctx) { (void)ctx; return g_runtime; }
JSGlobalContextRef jsr_context(jsr_runtime *rt) { return rt->ctx; }
JSObjectRef jsr_internal(jsr_runtime *rt) { return rt->internal; }
bool jsr_on_js_thread(jsr_runtime *rt) { return pthread_equal(pthread_self(), rt->thread); }
#ifndef __APPLE__
void *jsr_glib_context(jsr_runtime *rt) { return rt->gctx; }
#endif
int jsr_exit_code(jsr_runtime *rt) { return rt->exit_code; }

// --- strings ---------------------------------------------------------------
char *jsr_to_utf8(JSContextRef ctx, JSValueRef value) {
    JSStringRef s = JSValueToStringCopy(ctx, value, NULL);
    if (!s) return strdup("");
    size_t max = JSStringGetMaximumUTF8CStringSize(s);
    char *out = malloc(max);
    JSStringGetUTF8CString(s, out, max);
    JSStringRelease(s);
    return out;
}
JSValueRef jsr_string(JSContextRef ctx, const char *utf8) {
    JSStringRef s = JSStringCreateWithUTF8CString(utf8 ? utf8 : "");
    JSValueRef v = JSValueMakeString(ctx, s);
    JSStringRelease(s);
    return v;
}
void jsr_set_function(JSContextRef ctx, JSObjectRef obj, const char *name, JSObjectCallAsFunctionCallback cb) {
    JSStringRef n = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(ctx, obj, n, JSObjectMakeFunctionWithCallback(ctx, n, cb), kJSPropertyAttributeNone, NULL);
    JSStringRelease(n);
}
static void set_value(JSContextRef ctx, JSObjectRef obj, const char *name, JSValueRef value) {
    JSStringRef n = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(ctx, obj, n, value, kJSPropertyAttributeNone, NULL);
    JSStringRelease(n);
}

// --- exit ------------------------------------------------------------------------
// process.exit() and fatal errors end the process right there, as in Node and
// Bun: 'exit' listeners run once, stdio is flushed, and nothing after the call
// runs. A process.exit() inside an 'exit' listener exits at once.
static bool g_exiting;
static void emit_exit(jsr_runtime *rt) {
    if (g_exiting || !rt->dispatch_fn) return;
    g_exiting = true;
    JSValueRef args[2] = {jsr_string(rt->ctx, "exit"), JSValueMakeNumber(rt->ctx, rt->exit_code)};
    JSObjectCallAsFunction(rt->ctx, rt->dispatch_fn, NULL, 2, args, NULL);
}
_Noreturn static void exit_now(jsr_runtime *rt, int code) {
    rt->exit_code = code;
    emit_exit(rt);
    fflush(stdout);
    fflush(stderr);
    exit(rt->exit_code);
}

static void print_error(jsr_runtime *rt, const char *prefix, JSValueRef error) {
    JSStringRef stack_name = JSStringCreateWithUTF8CString("stack");
    JSValueRef stack = JSValueIsObject(rt->ctx, error)
        ? JSObjectGetProperty(rt->ctx, (JSObjectRef)error, stack_name, NULL) : NULL;
    JSStringRelease(stack_name);
    char *message = jsr_to_utf8(rt->ctx, error);
    char *trace = stack && !JSValueIsUndefined(rt->ctx, stack) ? jsr_to_utf8(rt->ctx, stack) : NULL;
    fprintf(stderr, "%s%s\n%s%s", prefix, message, trace ? trace : "", trace ? "\n" : "");
    free(message);
    free(trace);
}

// --- exceptions --------------------------------------------------------------
// With no 'uncaughtException' listener, an uncaught error ends the process
// with 1, as in Node and Bun.
void jsr_report_exception(jsr_runtime *rt, JSValueRef exception) {
    // Let process.on('uncaughtException') handle it when the prelude is up.
    if (rt->dispatch_fn) {
        JSValueRef args[2] = {jsr_string(rt->ctx, "uncaughtException"), exception};
        JSValueRef inner = NULL;
        JSValueRef handled = JSObjectCallAsFunction(rt->ctx, rt->dispatch_fn, NULL, 2, args, &inner);
        if (!inner && handled && JSValueToBoolean(rt->ctx, handled)) return;
    }
    print_error(rt, "Uncaught ", exception);
    exit_now(rt, 1);
}

JSValueRef jsr_call(jsr_runtime *rt, JSObjectRef fn, JSObjectRef self, size_t argc, const JSValueRef argv[]) {
    JSValueRef exception = NULL;
    JSValueRef result = JSObjectCallAsFunction(rt->ctx, fn, self, argc, argv, &exception);
    if (exception) {
        jsr_report_exception(rt, exception);
        return NULL;
    }
    return result;
}

bool jsr_eval(jsr_runtime *rt, const char *source, const char *url) {
    JSStringRef script = JSStringCreateWithUTF8CString(source);
    JSStringRef source_url = JSStringCreateWithUTF8CString(url);
    JSValueRef exception = NULL;
    JSEvaluateScript(rt->ctx, script, NULL, source_url, 1, &exception);
    JSStringRelease(script);
    JSStringRelease(source_url);
    if (exception) {
        jsr_report_exception(rt, exception);
        return false;
    }
    return true;
}

bool jsr_eval_file(jsr_runtime *rt, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "can't read %s\n", path); rt->exit_code = 1; return false; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = 0;
    bool ok = jsr_eval(rt, buf, path);
    free(buf);
    return ok;
}

// --- loop --------------------------------------------------------------------
void jsr_ref(jsr_runtime *rt) { rt->refs++; }
void jsr_unref(jsr_runtime *rt) { if (rt->refs > 0) rt->refs--; }

static bool queue_empty(jsr_runtime *rt) {
    pthread_mutex_lock(&rt->lock);
    bool empty = rt->head == NULL;
    pthread_mutex_unlock(&rt->lock);
    return empty;
}

static void maybe_stop_when_idle(jsr_runtime *rt);

static void drain_tasks(void *info) {
    jsr_runtime *rt = info;
    for (;;) {
        pthread_mutex_lock(&rt->lock);
#ifndef __APPLE__
        rt->drain_scheduled = false;
#endif
        jsr_task *task = rt->head;
        if (task) {
            rt->head = task->next;
            if (!rt->head) rt->tail = NULL;
        }
        pthread_mutex_unlock(&rt->lock);
        if (!task) break;
        task->fn(rt, task->data);
        free(task);
        if (rt->stopped) break;
    }
    maybe_stop_when_idle(rt);
}

#ifndef __APPLE__
static gboolean drain_tasks_source(gpointer info) {
    drain_tasks(info);
    return G_SOURCE_REMOVE;
}
#endif

void jsr_post(jsr_runtime *rt, jsr_task_fn fn, void *data) {
    jsr_task *task = calloc(1, sizeof *task);
    task->fn = fn;
    task->data = data;
    pthread_mutex_lock(&rt->lock);
    if (rt->tail) rt->tail->next = task; else rt->head = task;
    rt->tail = task;
#ifdef __APPLE__
    pthread_mutex_unlock(&rt->lock);
    CFRunLoopSourceSignal(rt->source);
    CFRunLoopWakeUp(rt->loop);
#else
    bool schedule = !rt->drain_scheduled;
    rt->drain_scheduled = true;
    pthread_mutex_unlock(&rt->lock);
    if (schedule) {
        // g_source_attach is thread-safe and wakes the context.
        GSource *idle = g_idle_source_new();
        g_source_set_callback(idle, drain_tasks_source, rt, NULL);
        g_source_attach(idle, rt->gctx);
        g_source_unref(idle);
    }
#endif
}

// Ends the current loop iteration so jsr_run re-checks whether to stop.
static void loop_wake(jsr_runtime *rt) {
#ifdef __APPLE__
    CFRunLoopStop(rt->loop);
#else
    g_main_context_wakeup(rt->gctx);
#endif
}

static bool g_until_idle;
static void maybe_stop_when_idle(jsr_runtime *rt) {
    if (g_until_idle && rt->refs == 0 && queue_empty(rt)) loop_wake(rt);
}

void jsr_run(jsr_runtime *rt, bool until_idle) {
    g_until_idle = until_idle;
    while (!rt->stopped) {
        if (until_idle && rt->refs == 0 && queue_empty(rt)) break;
#ifdef __APPLE__
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1e10, false);
#else
        g_main_context_iteration(rt->gctx, TRUE);
#endif
    }
    emit_exit(rt);
}

void jsr_stop(jsr_runtime *rt) {
    rt->stopped = true;
    loop_wake(rt);
}

// --- timers ------------------------------------------------------------------
static jsr_timer *find_timer(jsr_runtime *rt, uint32_t id, jsr_timer ***link_out) {
    jsr_timer **link = &rt->timers;
    while (*link && (*link)->id != id) link = &(*link)->next;
    if (link_out) *link_out = link;
    return *link;
}

static void free_timer(jsr_runtime *rt, uint32_t id) {
    jsr_timer **link;
    jsr_timer *t = find_timer(rt, id, &link);
    if (!t) return;
    *link = t->next;
#ifdef __APPLE__
    CFRunLoopTimerInvalidate(t->cf);
    CFRelease(t->cf);
#else
    g_source_destroy(t->source);
    g_source_unref(t->source);
#endif
    JSValueUnprotect(rt->ctx, t->fn);
    if (t->refed) jsr_unref(rt);
    free(t);
}

static void timer_fired_id(uint32_t id) {
    jsr_runtime *rt = g_runtime;
    jsr_timer *t = find_timer(rt, id, NULL);
    if (!t) return;
    JSObjectRef fn = t->fn;
    JSValueProtect(rt->ctx, fn);
    if (!t->repeat) free_timer(rt, id); // a timeout is gone before its callback runs
    jsr_call(rt, fn, NULL, 0, NULL);
    JSValueUnprotect(rt->ctx, fn);
    maybe_stop_when_idle(rt);
}
#ifdef __APPLE__
static void timer_fired(CFRunLoopTimerRef cf, void *info) {
    (void)cf;
    timer_fired_id((uint32_t)(uintptr_t)info);
}
#else
static gboolean timer_fired(gpointer info) {
    // free_timer destroys this source, so its return value is ignored then.
    timer_fired_id((uint32_t)(uintptr_t)info);
    return G_SOURCE_CONTINUE;
}
#endif

// __rt.setTimer(fn, ms, repeat, immediate) -> id. An immediate is due at once
// and, as Node's check phase, runs before timers that come due with it but
// after the timers already being run.
static JSValueRef rt_set_timer(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    jsr_runtime *rt = g_runtime;
    if (argc < 1 || !JSValueIsObject(ctx, argv[0])) return JSValueMakeNumber(ctx, 0);
    double ms = argc > 1 ? JSValueToNumber(ctx, argv[1], NULL) : 0;
    bool repeat = argc > 2 && JSValueToBoolean(ctx, argv[2]);
    bool immediate = argc > 3 && JSValueToBoolean(ctx, argv[3]);
    if (immediate) ms = 0;
    else if (!(ms >= 1)) ms = 1; // Node clamps to 1 ms
    jsr_timer *t = calloc(1, sizeof *t);
    t->id = ++rt->next_timer_id;
    t->fn = (JSObjectRef)argv[0];
    JSValueProtect(ctx, t->fn);
    t->repeat = repeat;
    t->refed = true;
#ifdef __APPLE__
    CFRunLoopTimerContext tctx = {0, (void *)(uintptr_t)t->id, NULL, NULL, NULL};
    double seconds = ms / 1000.0;
    t->cf = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + seconds, repeat ? seconds : 0, 0, 0, timer_fired, &tctx);
    CFRunLoopAddTimer(rt->loop, t->cf, kCFRunLoopCommonModes);
#else
    t->source = g_timeout_source_new((guint)(ms > (double)G_MAXUINT ? G_MAXUINT : ms));
    if (immediate) g_source_set_priority(t->source, G_PRIORITY_DEFAULT - 1);
    g_source_set_callback(t->source, timer_fired, (void *)(uintptr_t)t->id, NULL);
    g_source_attach(t->source, rt->gctx);
#endif
    t->next = rt->timers;
    rt->timers = t;
    jsr_ref(rt);
    return JSValueMakeNumber(ctx, t->id);
}
static JSValueRef rt_clear_timer(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    if (argc > 0) free_timer(g_runtime, (uint32_t)JSValueToNumber(ctx, argv[0], NULL));
    return JSValueMakeUndefined(ctx);
}
// __rt.refTimer(id, bool)
static JSValueRef rt_ref_timer(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    jsr_runtime *rt = g_runtime;
    jsr_timer *t = argc > 1 ? find_timer(rt, (uint32_t)JSValueToNumber(ctx, argv[0], NULL), NULL) : NULL;
    if (t) {
        bool want = JSValueToBoolean(ctx, argv[1]);
        if (want && !t->refed) jsr_ref(rt);
        if (!want && t->refed) jsr_unref(rt);
        t->refed = want;
    }
    return JSValueMakeUndefined(ctx);
}

// --- process -----------------------------------------------------------------
static JSValueRef rt_write(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    if (argc < 2) return JSValueMakeUndefined(ctx);
    int fd = (int)JSValueToNumber(ctx, argv[0], NULL);
    char *s = jsr_to_utf8(ctx, argv[1]);
    FILE *out = fd == 2 ? stderr : stdout;
    fputs(s, out);
    fflush(out);
    free(s);
    return JSValueMakeUndefined(ctx);
}
static JSValueRef rt_exit(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    jsr_runtime *rt = g_runtime;
    int code = argc > 0 ? (int)JSValueToNumber(ctx, argv[0], NULL) : rt->exit_code;
    if (g_exiting) { fflush(stdout); fflush(stderr); exit(code); }
    exit_now(rt, code);
}
// __rt.fatal(prefix, error): print it and exit 1 (an unhandled rejection).
static JSValueRef rt_fatal(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    char *prefix = argc > 0 ? jsr_to_utf8(ctx, argv[0]) : strdup("");
    print_error(g_runtime, prefix, argc > 1 ? argv[1] : JSValueMakeUndefined(ctx));
    free(prefix);
    exit_now(g_runtime, 1);
}
static JSValueRef rt_set_exit_code(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    if (argc > 0) g_runtime->exit_code = (int)JSValueToNumber(ctx, argv[0], NULL);
    return JSValueMakeUndefined(ctx);
}
static JSValueRef rt_cwd(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)argc; (void)argv; (void)exc;
    char buf[PATH_MAX];
    return getcwd(buf, sizeof buf) ? jsr_string(ctx, buf) : jsr_string(ctx, "/");
}
static JSValueRef rt_chdir(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self;
    char *path = argc > 0 ? jsr_to_utf8(ctx, argv[0]) : NULL;
    int failed = !path || chdir(path) != 0;
    int err = errno;
    if (failed) {
        // A Node-shaped error: code, errno, syscall, path.
        char message[PATH_MAX + 64];
        snprintf(message, sizeof message, "%s: %s, chdir '%s'", err == ENOENT ? "ENOENT" : err == ENOTDIR ? "ENOTDIR" : err == EACCES ? "EACCES" : "EINVAL", strerror(err), path ? path : "");
        JSValueRef args[1] = {jsr_string(ctx, message)};
        JSObjectRef error = JSObjectMakeError(ctx, 1, args, NULL);
        set_value(ctx, error, "code", jsr_string(ctx, err == ENOENT ? "ENOENT" : err == ENOTDIR ? "ENOTDIR" : err == EACCES ? "EACCES" : "EINVAL"));
        set_value(ctx, error, "errno", JSValueMakeNumber(ctx, -err));
        set_value(ctx, error, "syscall", jsr_string(ctx, "chdir"));
        if (path) set_value(ctx, error, "path", jsr_string(ctx, path));
        *exc = error;
    }
    free(path);
    return JSValueMakeUndefined(ctx);
}
static JSValueRef rt_now(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)argc; (void)argv; (void)exc;
#ifdef __APPLE__
    static mach_timebase_info_data_t tb;
    static uint64_t start;
    if (!tb.denom) { mach_timebase_info(&tb); start = mach_absolute_time(); }
    double ns = (double)(mach_absolute_time() - start) * tb.numer / tb.denom;
#else
    static double start = -1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now_ns = (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
    if (start < 0) start = now_ns;
    double ns = now_ns - start;
#endif
    return JSValueMakeNumber(ctx, ns / 1e6);
}

// Signals: a dispatch source (macOS) posts the signal to the JS thread, or a
// GLib unix signal source on the JS thread's context (Linux) delivers it there;
// the prelude emits it on process. Installed when JS first listens.
typedef struct { int signo; } signal_task;
static void deliver_signal(jsr_runtime *rt, void *data) {
    signal_task *task = data;
    const char *name = task->signo == SIGINT ? "SIGINT" : task->signo == SIGTERM ? "SIGTERM" : "SIGHUP";
    // (no listeners left: the prelude exits with 128 + the signal's number)
    JSValueRef args[1] = {jsr_string(rt->ctx, name)};
    jsr_call(rt, rt->dispatch_fn, NULL, 1, args);
    free(task);
}
#ifndef __APPLE__
static gboolean signal_fired(gpointer data) {
    signal_task *task = malloc(sizeof *task);
    task->signo = (int)(intptr_t)data;
    deliver_signal(g_runtime, task);
    return G_SOURCE_CONTINUE;
}
#endif
static JSValueRef rt_watch_signal(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    char *name = argc > 0 ? jsr_to_utf8(ctx, argv[0]) : strdup("");
    int signo = !strcmp(name, "SIGINT") ? SIGINT : !strcmp(name, "SIGTERM") ? SIGTERM : !strcmp(name, "SIGHUP") ? SIGHUP : 0;
    free(name);
    if (!signo) return JSValueMakeBoolean(ctx, false);
#ifndef __APPLE__
    GSource *src = g_unix_signal_source_new(signo);
    g_source_set_callback(src, signal_fired, (void *)(intptr_t)signo, NULL);
    g_source_attach(src, g_runtime->gctx); // lives for the process
    g_source_unref(src);
    return JSValueMakeBoolean(ctx, true);
#else
    signal(signo, SIG_IGN);
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)signo, 0, dispatch_get_global_queue(0, 0));
    dispatch_source_set_event_handler(src, ^{
        signal_task *task = malloc(sizeof *task);
        task->signo = signo;
        jsr_post(g_runtime, deliver_signal, task);
    });
    dispatch_resume(src); // lives for the process
    return JSValueMakeBoolean(ctx, true);
#endif
}

static JSObjectRef make_env(JSContextRef ctx) {
    JSObjectRef env = JSObjectMake(ctx, NULL, NULL);
    for (char **e = environ; *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        char *key = strndup(*e, (size_t)(eq - *e));
        set_value(ctx, env, key, jsr_string(ctx, eq + 1));
        free(key);
    }
    return env;
}

jsr_runtime *jsr_create(int argc, const char **argv) {
    jsr_runtime *rt = calloc(1, sizeof *rt);
    g_runtime = rt;
    rt->argc = argc;
    rt->argv = argv;
    rt->thread = pthread_self();
    pthread_mutex_init(&rt->lock, NULL);
#ifdef __APPLE__
    rt->loop = CFRunLoopGetCurrent();
    CFRunLoopSourceContext sctx = {0};
    sctx.info = rt;
    sctx.perform = drain_tasks;
    rt->source = CFRunLoopSourceCreate(NULL, 0, &sctx);
    CFRunLoopAddSource(rt->loop, rt->source, kCFRunLoopCommonModes);
#else
    // The JS thread's own context: the main thread's default context belongs
    // to GTK in an app host.
    // Not pushed as this thread's default context: WebKitGTK and other GLib
    // code the core runs from this thread would bind to it instead of GTK's
    // loop (under Bun this thread has no default context). Every runtime
    // source is attached to rt->gctx explicitly.
    rt->gctx = g_main_context_new();
#endif

    rt->ctx = JSGlobalContextCreate(NULL);
    JSContextRef ctx = rt->ctx;
    rt->internal = JSObjectMake(ctx, NULL, NULL);
    JSValueProtect(ctx, rt->internal);
    jsr_set_function(ctx, rt->internal, "setTimer", rt_set_timer);
    jsr_set_function(ctx, rt->internal, "clearTimer", rt_clear_timer);
    jsr_set_function(ctx, rt->internal, "refTimer", rt_ref_timer);
    jsr_set_function(ctx, rt->internal, "write", rt_write);
    jsr_set_function(ctx, rt->internal, "exit", rt_exit);
    jsr_set_function(ctx, rt->internal, "fatal", rt_fatal);
    jsr_set_function(ctx, rt->internal, "setExitCode", rt_set_exit_code);
    jsr_set_function(ctx, rt->internal, "now", rt_now);
    {
        struct utsname u;
        set_value(ctx, rt->internal, "arch", jsr_string(ctx, uname(&u) == 0 && !strcmp(u.machine, "x86_64") ? "x64" : "arm64"));
    }
    jsr_set_function(ctx, rt->internal, "cwd", rt_cwd);
    jsr_set_function(ctx, rt->internal, "chdir", rt_chdir);
    {
        char real[PATH_MAX];
#ifdef __APPLE__
        char exe[PATH_MAX];
        uint32_t size = sizeof exe;
        if (_NSGetExecutablePath(exe, &size) == 0 && realpath(exe, real))
            set_value(ctx, rt->internal, "execPath", jsr_string(ctx, real));
#else
        if (realpath("/proc/self/exe", real)) set_value(ctx, rt->internal, "execPath", jsr_string(ctx, real));
#endif
    }
#ifdef __APPLE__
    set_value(ctx, rt->internal, "platform", jsr_string(ctx, "darwin"));
#elif defined(__linux__)
    set_value(ctx, rt->internal, "platform", jsr_string(ctx, "linux"));
#endif
    jsr_set_function(ctx, rt->internal, "watchSignal", rt_watch_signal);
    set_value(ctx, rt->internal, "env", make_env(ctx));
    JSValueRef *args = calloc((size_t)argc + 1, sizeof *args);
    for (int i = 0; i < argc; i++) args[i] = jsr_string(ctx, argv[i]);
    set_value(ctx, rt->internal, "argv", JSObjectMakeArray(ctx, (size_t)argc, args, NULL));
    free(args);
    set_value(ctx, rt->internal, "pid", JSValueMakeNumber(ctx, getpid()));
    set_value(ctx, JSContextGetGlobalObject(ctx), "__rt", rt->internal);

    jsr_eval(rt, (const char *)jsr_prelude_js, "livewire:prelude");
    JSStringRef dispatch_name = JSStringCreateWithUTF8CString("dispatchEvent");
    JSValueRef dispatch = JSObjectGetProperty(ctx, rt->internal, dispatch_name, NULL);
    JSStringRelease(dispatch_name);
    if (JSValueIsObject(ctx, dispatch)) {
        rt->dispatch_fn = (JSObjectRef)dispatch;
        JSValueProtect(ctx, dispatch);
    }
    // Unhandled rejections reach __rt.onUnhandledRejection(promise, reason).
    // JavaScriptCore exports the setter (macOS 10.15.4+, WebKitGTK) without a
    // public header, so it's looked up rather than linked.
    typedef void (*set_rejection_fn)(JSGlobalContextRef, JSObjectRef, JSValueRef *);
    set_rejection_fn set_rejection = (set_rejection_fn)dlsym(RTLD_DEFAULT, "JSGlobalContextSetUnhandledRejectionCallback");
    JSStringRef on_name = JSStringCreateWithUTF8CString("onUnhandledRejection");
    JSValueRef on_rejection = JSObjectGetProperty(ctx, rt->internal, on_name, NULL);
    JSStringRelease(on_name);
    if (set_rejection && JSValueIsObject(ctx, on_rejection)) set_rejection(rt->ctx, (JSObjectRef)on_rejection, NULL);
    return rt;
}
