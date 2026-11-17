// Workers: `new Worker(file)` runs a bundled script in its own runtime, on its
// own thread with its own loop and JavaScriptCore VM, as Bun and Node do.
// Messages are structured-cloned in JS (node/worker.js) into JSON plus the
// ArrayBuffers it refers to; here they're copied across and handed to the
// other side's loop. terminate() stops the worker's loop and, through JSC's
// watchdog, any JS it's still running.
#include "runtime.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct worker_msg {
    char *json;
    size_t count;
    void **bytes;
    size_t *lens;
    struct worker_msg *next;
} worker_msg;

typedef struct jsr_worker {
    _Atomic int refs;      // the parent's handle and the worker's thread
    int id;
    jsr_runtime *parent;
    jsr_runtime *child;    // while the worker's thread runs (under g_workers_lock)
    JSObjectRef on_event;  // the parent's handler(type, json, buffers), protected there
    char *path;
    worker_msg *init;      // workerData and name, read once by the worker
    worker_msg *pending;   // posted before the worker's runtime existed
    bool terminated;
    struct jsr_worker *next;
} jsr_worker;

static pthread_mutex_t g_workers_lock = PTHREAD_MUTEX_INITIALIZER;
static jsr_worker *g_workers;
static int g_next_worker_id;

#define NATIVE(name) static JSValueRef name(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc)
#define UNUSED_ARGS (void)f; (void)self
#define ARG(i) ((size_t)(i) < argc ? argv[i] : JSValueMakeUndefined(ctx))

static void msg_free(worker_msg *m) {
    if (!m) return;
    for (size_t i = 0; i < m->count; i++) free(m->bytes[i]);
    free(m->bytes);
    free(m->lens);
    free(m->json);
    free(m);
}

// A copy of (json, [ArrayBuffer...]) from JS.
static worker_msg *msg_capture(JSContextRef ctx, JSValueRef json, JSValueRef buffers) {
    worker_msg *m = calloc(1, sizeof *m);
    m->json = jsr_to_utf8(ctx, json);
    if (!JSValueIsObject(ctx, buffers)) return m;
    JSObjectRef list = (JSObjectRef)buffers;
    JSStringRef length = JSStringCreateWithUTF8CString("length");
    size_t n = (size_t)JSValueToNumber(ctx, JSObjectGetProperty(ctx, list, length, NULL), NULL);
    JSStringRelease(length);
    m->bytes = calloc(n ? n : 1, sizeof *m->bytes);
    m->lens = calloc(n ? n : 1, sizeof *m->lens);
    m->count = n;
    for (size_t i = 0; i < n; i++) {
        JSValueRef v = JSObjectGetPropertyAtIndex(ctx, list, (unsigned)i, NULL);
        if (JSValueGetTypedArrayType(ctx, v, NULL) != kJSTypedArrayTypeArrayBuffer) continue;
        JSObjectRef ab = (JSObjectRef)v;
        size_t len = JSObjectGetArrayBufferByteLength(ctx, ab, NULL);
        m->bytes[i] = malloc(len ? len : 1);
        if (len) memcpy(m->bytes[i], JSObjectGetArrayBufferBytesPtr(ctx, ab, NULL), len);
        m->lens[i] = len;
    }
    return m;
}

static void free_bytes(void *bytes, void *ctx) { (void)ctx; free(bytes); }
// (json, [ArrayBuffer...]) in ctx; the buffers take the message's bytes.
static void msg_values(JSContextRef ctx, worker_msg *m, JSValueRef out[2]) {
    out[0] = jsr_string(ctx, m->json);
    JSValueRef *items = calloc(m->count ? m->count : 1, sizeof *items);
    for (size_t i = 0; i < m->count; i++) {
        void *bytes = m->bytes[i] ? m->bytes[i] : malloc(1);
        items[i] = JSObjectMakeArrayBufferWithBytesNoCopy(ctx, bytes, m->lens[i], free_bytes, NULL, NULL);
        m->bytes[i] = NULL;
    }
    out[1] = JSObjectMakeArray(ctx, m->count, items, NULL);
    free(items);
}

static worker_msg *msg_text(const char *json) {
    worker_msg *m = calloc(1, sizeof *m);
    m->json = strdup(json);
    return m;
}

static void worker_unref(jsr_worker *w) {
    if (atomic_fetch_sub(&w->refs, 1) != 1) return;
    pthread_mutex_lock(&g_workers_lock);
    for (jsr_worker **link = &g_workers; *link; link = &(*link)->next)
        if (*link == w) { *link = w->next; break; }
    pthread_mutex_unlock(&g_workers_lock);
    msg_free(w->init);
    free(w->path);
    free(w);
}

static jsr_worker *worker_find(int id) {
    pthread_mutex_lock(&g_workers_lock);
    jsr_worker *w = g_workers;
    while (w && w->id != id) w = w->next;
    pthread_mutex_unlock(&g_workers_lock);
    return w;
}

// --- to the parent -------------------------------------------------------------
typedef struct { jsr_worker *w; const char *type; worker_msg *m; } parent_event;

static void deliver_to_parent(jsr_runtime *rt, void *data) {
    parent_event *e = data;
    JSContextRef ctx = jsr_context(rt);
    JSValueRef args[3];
    args[0] = jsr_string(ctx, e->type);
    msg_values(ctx, e->m, args + 1);
    jsr_call(rt, e->w->on_event, NULL, 3, args);
    if (!strcmp(e->type, "exit")) {
        JSValueUnprotect(ctx, e->w->on_event);
        worker_unref(e->w); // the parent's handle
    }
    msg_free(e->m);
    free(e);
}

static void post_to_parent(jsr_worker *w, const char *type, worker_msg *m) {
    parent_event *e = malloc(sizeof *e);
    e->w = w;
    e->type = type;
    e->m = m;
    jsr_post(w->parent, deliver_to_parent, e);
}

// An uncaught error in the worker, as {name, message, stack} JSON.
void jsr_worker_error(jsr_runtime *rt, JSValueRef exception) {
    JSContextRef ctx = jsr_context(rt);
    JSObjectRef info = JSObjectMake(ctx, NULL, NULL);
    const char *keys[] = {"name", "message", "stack"};
    for (int i = 0; i < 3; i++) {
        JSStringRef k = JSStringCreateWithUTF8CString(keys[i]);
        JSValueRef v = JSValueIsObject(ctx, exception) ? JSObjectGetProperty(ctx, (JSObjectRef)exception, k, NULL) : NULL;
        if (i == 1 && (!v || JSValueIsUndefined(ctx, v))) v = exception; // a thrown non-Error
        if (v && !JSValueIsUndefined(ctx, v)) {
            char *s = jsr_to_utf8(ctx, v);
            JSObjectSetProperty(ctx, info, k, jsr_string(ctx, s), kJSPropertyAttributeNone, NULL);
            free(s);
        }
        JSStringRelease(k);
    }
    JSStringRef json = JSValueCreateJSONString(ctx, info, 0, NULL);
    size_t max = json ? JSStringGetMaximumUTF8CStringSize(json) : 3;
    char *text = malloc(max);
    if (json) { JSStringGetUTF8CString(json, text, max); JSStringRelease(json); } else strcpy(text, "{}");
    post_to_parent(jsr_worker_of(rt), "error", msg_text(text));
    free(text);
}

// --- to the worker -------------------------------------------------------------
static void deliver_to_child(jsr_runtime *rt, void *data) {
    worker_msg *m = data;
    JSContextRef ctx = jsr_context(rt);
    JSStringRef name = JSStringCreateWithUTF8CString("onParentMessage");
    JSValueRef fn = JSObjectGetProperty(ctx, jsr_internal(rt), name, NULL);
    JSStringRelease(name);
    if (JSValueIsObject(ctx, fn)) {
        JSValueRef args[2];
        msg_values(ctx, m, args);
        jsr_call(rt, (JSObjectRef)fn, NULL, 2, args);
    }
    msg_free(m);
}

static void stop_task(jsr_runtime *rt, void *data) { (void)data; jsr_stop(rt); }

static void terminate(jsr_worker *w) {
    pthread_mutex_lock(&g_workers_lock);
    w->terminated = true;
    jsr_runtime *child = w->child;
    if (child) {
        jsr_worker_terminating(child);
        jsr_post(child, stop_task, NULL);
    }
    pthread_mutex_unlock(&g_workers_lock);
}

// --- the worker's thread ---------------------------------------------------------
static void *worker_main(void *arg) {
    jsr_worker *w = arg;
    jsr_runtime *parent = w->parent;
    jsr_runtime *rt = jsr_create_worker(jsr_argc(parent), jsr_argv(parent), w);
    // From here the parent posts straight to this runtime; what it sent before
    // is queued first. Nothing runs until the script has been evaluated.
    pthread_mutex_lock(&g_workers_lock);
    bool terminated = w->terminated;
    if (!terminated) w->child = rt;
    worker_msg *m = w->pending;
    w->pending = NULL;
    pthread_mutex_unlock(&g_workers_lock);
    while (m) {
        worker_msg *next = m->next;
        if (terminated) msg_free(m); else jsr_post(rt, deliver_to_child, m);
        m = next;
    }
    if (!terminated && jsr_eval_file(rt, w->path)) jsr_run(rt, true);
    else jsr_stop(rt);
    pthread_mutex_lock(&g_workers_lock);
    w->child = NULL;
    // Workers this one started end with it.
    jsr_worker *kids[64];
    int n = 0;
    for (jsr_worker *k = g_workers; k && n < 64; k = k->next)
        if (k->parent == rt) kids[n++] = k;
    pthread_mutex_unlock(&g_workers_lock);
    for (int i = 0; i < n; i++) terminate(kids[i]);
    char code[16];
    snprintf(code, sizeof code, "%d", jsr_exit_code(rt));
    jsr_destroy(rt);
    post_to_parent(w, "exit", msg_text(code));
    worker_unref(w); // the thread's
    return NULL;
}

// __rt.workerSpawn(path, onEvent, initJson, initBuffers) -> id
NATIVE(w_spawn) {
    UNUSED_ARGS;
    if (!JSValueIsObject(ctx, ARG(1))) { *exc = jsr_string(ctx, "workerSpawn: onEvent must be a function"); return NULL; }
    jsr_worker *w = calloc(1, sizeof *w);
    atomic_store(&w->refs, 2);
    w->parent = jsr_from_context(ctx);
    w->path = jsr_to_utf8(ctx, ARG(0));
    w->on_event = (JSObjectRef)ARG(1);
    JSValueProtect(ctx, w->on_event);
    w->init = msg_capture(ctx, ARG(2), ARG(3));
    pthread_mutex_lock(&g_workers_lock);
    w->id = ++g_next_worker_id;
    w->next = g_workers;
    g_workers = w;
    pthread_mutex_unlock(&g_workers_lock);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024); // as the app host's JS thread
    pthread_t thread;
    int failed = pthread_create(&thread, &attr, worker_main, w);
    pthread_attr_destroy(&attr);
    if (failed) {
        JSValueUnprotect(ctx, w->on_event);
        atomic_store(&w->refs, 1);
        worker_unref(w);
        *exc = jsr_string(ctx, "Worker: couldn't start a thread");
        return NULL;
    }
    pthread_detach(thread);
    return JSValueMakeNumber(ctx, w->id);
}

// __rt.workerPost(id, json, buffers)
NATIVE(w_post) {
    UNUSED_ARGS; (void)exc;
    jsr_worker *w = worker_find((int)JSValueToNumber(ctx, ARG(0), NULL));
    if (!w) return JSValueMakeBoolean(ctx, false);
    worker_msg *m = msg_capture(ctx, ARG(1), ARG(2));
    pthread_mutex_lock(&g_workers_lock);
    // Before the worker's runtime exists, held in pending (worker_main queues them).
    jsr_runtime *child = w->terminated ? NULL : w->child;
    if (child) jsr_post(child, deliver_to_child, m);
    else if (!w->terminated) {
        m->next = NULL;
        worker_msg **link = &w->pending;
        while (*link) link = &(*link)->next;
        *link = m;
        m = NULL;
    }
    pthread_mutex_unlock(&g_workers_lock);
    if (!child && m) msg_free(m);
    return JSValueMakeBoolean(ctx, true);
}

// __rt.workerTerminate(id)
NATIVE(w_terminate) {
    UNUSED_ARGS; (void)exc;
    jsr_worker *w = worker_find((int)JSValueToNumber(ctx, ARG(0), NULL));
    if (w) terminate(w);
    return JSValueMakeUndefined(ctx);
}

// In a worker: __rt.workerInit() -> [json, buffers] once (workerData, name), then null.
NATIVE(w_init) {
    UNUSED_ARGS; (void)exc; (void)argc; (void)argv;
    jsr_worker *w = jsr_worker_of(jsr_from_context(ctx));
    if (!w || !w->init) return JSValueMakeNull(ctx);
    JSValueRef pair[2];
    msg_values(ctx, w->init, pair);
    msg_free(w->init);
    w->init = NULL;
    return JSObjectMakeArray(ctx, 2, pair, NULL);
}

// In a worker: __rt.parentPost(json, buffers)
NATIVE(w_parent_post) {
    UNUSED_ARGS; (void)exc;
    jsr_worker *w = jsr_worker_of(jsr_from_context(ctx));
    if (!w) return JSValueMakeBoolean(ctx, false);
    post_to_parent(w, "message", msg_capture(ctx, ARG(0), ARG(1)));
    return JSValueMakeBoolean(ctx, true);
}

void worker_install(jsr_runtime *rt) {
    JSContextRef ctx = jsr_context(rt);
    JSObjectRef internal = jsr_internal(rt);
    jsr_set_function(ctx, internal, "workerSpawn", w_spawn);
    jsr_set_function(ctx, internal, "workerPost", w_post);
    jsr_set_function(ctx, internal, "workerTerminate", w_terminate);
    jsr_worker *w = jsr_worker_of(rt);
    if (!w) return;
    jsr_set_function(ctx, internal, "workerInit", w_init);
    jsr_set_function(ctx, internal, "parentPost", w_parent_post);
    JSStringRef id = JSStringCreateWithUTF8CString("workerId");
    JSObjectSetProperty(ctx, internal, id, JSValueMakeNumber(ctx, w->id), kJSPropertyAttributeNone, NULL);
    JSStringRelease(id);
}
