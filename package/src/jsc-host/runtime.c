// See runtime.h. The loop is the JS thread's CFRunLoop: timers are
// CFRunLoopTimers, cross-thread work is a CFRunLoopSource, and JSC drains
// promise jobs when each outermost API call into JS returns, so a callback that
// resolves a promise runs its continuations before the loop moves on.
#include "runtime.h"
#include "prelude.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

typedef struct jsr_task {
    jsr_task_fn fn;
    void *data;
    struct jsr_task *next;
} jsr_task;

typedef struct jsr_timer {
    uint32_t id;
    JSObjectRef fn;
    CFRunLoopTimerRef cf;
    bool repeat;
    bool refed;
    struct jsr_timer *next;
} jsr_timer;

struct jsr_runtime {
    JSGlobalContextRef ctx;
    JSObjectRef internal;      // __rt: natives the prelude wraps
    JSObjectRef dispatch_fn;   // prelude's __rt.dispatchEvent(name, ...args)
    CFRunLoopRef loop;
    CFRunLoopSourceRef source;
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

// --- exceptions --------------------------------------------------------------
void jsr_report_exception(jsr_runtime *rt, JSValueRef exception) {
    // Let process.on('uncaughtException') handle it when the prelude is up.
    if (rt->dispatch_fn) {
        JSValueRef args[2] = {jsr_string(rt->ctx, "uncaughtException"), exception};
        JSValueRef inner = NULL;
        JSValueRef handled = JSObjectCallAsFunction(rt->ctx, rt->dispatch_fn, NULL, 2, args, &inner);
        if (!inner && handled && JSValueToBoolean(rt->ctx, handled)) return;
    }
    JSStringRef stack_name = JSStringCreateWithUTF8CString("stack");
    JSValueRef stack = JSValueIsObject(rt->ctx, exception)
        ? JSObjectGetProperty(rt->ctx, (JSObjectRef)exception, stack_name, NULL) : NULL;
    JSStringRelease(stack_name);
    char *message = jsr_to_utf8(rt->ctx, exception);
    char *trace = stack && !JSValueIsUndefined(rt->ctx, stack) ? jsr_to_utf8(rt->ctx, stack) : NULL;
    fprintf(stderr, "Uncaught %s\n%s%s", message, trace ? trace : "", trace ? "\n" : "");
    free(message);
    free(trace);
    if (rt->exit_code == 0) rt->exit_code = 1;
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

void jsr_post(jsr_runtime *rt, jsr_task_fn fn, void *data) {
    jsr_task *task = calloc(1, sizeof *task);
    task->fn = fn;
    task->data = data;
    pthread_mutex_lock(&rt->lock);
    if (rt->tail) rt->tail->next = task; else rt->head = task;
    rt->tail = task;
    pthread_mutex_unlock(&rt->lock);
    CFRunLoopSourceSignal(rt->source);
    CFRunLoopWakeUp(rt->loop);
}

static bool g_until_idle;
static void maybe_stop_when_idle(jsr_runtime *rt) {
    if (g_until_idle && rt->refs == 0 && queue_empty(rt)) CFRunLoopStop(rt->loop);
}

void jsr_run(jsr_runtime *rt, bool until_idle) {
    g_until_idle = until_idle;
    while (!rt->stopped) {
        if (until_idle && rt->refs == 0 && queue_empty(rt)) break;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1e10, false);
    }
    if (rt->dispatch_fn) {
        JSValueRef args[2] = {jsr_string(rt->ctx, "exit"), JSValueMakeNumber(rt->ctx, rt->exit_code)};
        JSObjectCallAsFunction(rt->ctx, rt->dispatch_fn, NULL, 2, args, NULL);
    }
}

void jsr_stop(jsr_runtime *rt) {
    rt->stopped = true;
    CFRunLoopStop(rt->loop);
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
    CFRunLoopTimerInvalidate(t->cf);
    CFRelease(t->cf);
    JSValueUnprotect(rt->ctx, t->fn);
    if (t->refed) jsr_unref(rt);
    free(t);
}

static void timer_fired(CFRunLoopTimerRef cf, void *info) {
    (void)cf;
    jsr_runtime *rt = g_runtime;
    uint32_t id = (uint32_t)(uintptr_t)info;
    jsr_timer *t = find_timer(rt, id, NULL);
    if (!t) return;
    JSObjectRef fn = t->fn;
    JSValueProtect(rt->ctx, fn);
    if (!t->repeat) free_timer(rt, id); // a timeout is gone before its callback runs
    jsr_call(rt, fn, NULL, 0, NULL);
    JSValueUnprotect(rt->ctx, fn);
    maybe_stop_when_idle(rt);
}

// __rt.setTimer(fn, ms, repeat) -> id
static JSValueRef rt_set_timer(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    jsr_runtime *rt = g_runtime;
    if (argc < 1 || !JSValueIsObject(ctx, argv[0])) return JSValueMakeNumber(ctx, 0);
    double ms = argc > 1 ? JSValueToNumber(ctx, argv[1], NULL) : 0;
    if (!(ms >= 1)) ms = 1; // Node clamps to 1 ms
    bool repeat = argc > 2 && JSValueToBoolean(ctx, argv[2]);
    jsr_timer *t = calloc(1, sizeof *t);
    t->id = ++rt->next_timer_id;
    t->fn = (JSObjectRef)argv[0];
    JSValueProtect(ctx, t->fn);
    t->repeat = repeat;
    t->refed = true;
    CFRunLoopTimerContext tctx = {0, (void *)(uintptr_t)t->id, NULL, NULL, NULL};
    double seconds = ms / 1000.0;
    t->cf = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + seconds, repeat ? seconds : 0, 0, 0, timer_fired, &tctx);
    CFRunLoopAddTimer(rt->loop, t->cf, kCFRunLoopCommonModes);
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
    rt->exit_code = argc > 0 ? (int)JSValueToNumber(ctx, argv[0], NULL) : rt->exit_code;
    jsr_stop(rt);
    return JSValueMakeUndefined(ctx);
}
static JSValueRef rt_set_exit_code(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    if (argc > 0) g_runtime->exit_code = (int)JSValueToNumber(ctx, argv[0], NULL);
    return JSValueMakeUndefined(ctx);
}
static JSValueRef rt_now(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)argc; (void)argv; (void)exc;
    static mach_timebase_info_data_t tb;
    static uint64_t start;
    if (!tb.denom) { mach_timebase_info(&tb); start = mach_absolute_time(); }
    double ns = (double)(mach_absolute_time() - start) * tb.numer / tb.denom;
    return JSValueMakeNumber(ctx, ns / 1e6);
}

// Signals: a dispatch source posts the signal to the JS thread, where the
// prelude emits it on process. Installed when JS first listens.
typedef struct { int signo; } signal_task;
static void deliver_signal(jsr_runtime *rt, void *data) {
    signal_task *task = data;
    const char *name = task->signo == SIGINT ? "SIGINT" : task->signo == SIGTERM ? "SIGTERM" : "SIGHUP";
    JSValueRef args[1] = {jsr_string(rt->ctx, name)};
    jsr_call(rt, rt->dispatch_fn, NULL, 1, args);
    free(task);
}
static JSValueRef rt_watch_signal(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    char *name = argc > 0 ? jsr_to_utf8(ctx, argv[0]) : strdup("");
    int signo = !strcmp(name, "SIGINT") ? SIGINT : !strcmp(name, "SIGTERM") ? SIGTERM : !strcmp(name, "SIGHUP") ? SIGHUP : 0;
    free(name);
    if (!signo) return JSValueMakeBoolean(ctx, false);
    signal(signo, SIG_IGN);
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)signo, 0, dispatch_get_global_queue(0, 0));
    dispatch_source_set_event_handler(src, ^{
        signal_task *task = malloc(sizeof *task);
        task->signo = signo;
        jsr_post(g_runtime, deliver_signal, task);
    });
    dispatch_resume(src); // lives for the process
    return JSValueMakeBoolean(ctx, true);
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
    rt->loop = CFRunLoopGetCurrent();
    pthread_mutex_init(&rt->lock, NULL);
    CFRunLoopSourceContext sctx = {0};
    sctx.info = rt;
    sctx.perform = drain_tasks;
    rt->source = CFRunLoopSourceCreate(NULL, 0, &sctx);
    CFRunLoopAddSource(rt->loop, rt->source, kCFRunLoopCommonModes);

    rt->ctx = JSGlobalContextCreate(NULL);
    JSContextRef ctx = rt->ctx;
    rt->internal = JSObjectMake(ctx, NULL, NULL);
    JSValueProtect(ctx, rt->internal);
    jsr_set_function(ctx, rt->internal, "setTimer", rt_set_timer);
    jsr_set_function(ctx, rt->internal, "clearTimer", rt_clear_timer);
    jsr_set_function(ctx, rt->internal, "refTimer", rt_ref_timer);
    jsr_set_function(ctx, rt->internal, "write", rt_write);
    jsr_set_function(ctx, rt->internal, "exit", rt_exit);
    jsr_set_function(ctx, rt->internal, "setExitCode", rt_set_exit_code);
    jsr_set_function(ctx, rt->internal, "now", rt_now);
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
    return rt;
}
