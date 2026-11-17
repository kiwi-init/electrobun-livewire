// Livewire JSC host (spike, T109): runs an app's bundled TS main process on the
// JavaScriptCore.framework that ships with macOS, instead of a bundled Bun.
//
// The launcher starts this as the app's "main" binary (build.json mainProcess
// "zig" path for now). It loads libElectrobunCore.dylib, exposes the few core
// symbols the hello-world needs as host functions on globalThis.__host (the
// glue a real build would generate from the SDK's FFI declarations), evaluates
// Resources/app.js on a JS thread and runs AppKit's loop on the main thread.

#include <JavaScriptCore/JavaScriptCore.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef uint32_t (*DecideNavigationFn)(uint32_t, const char *);
typedef void (*WebviewEventFn)(uint32_t, const char *, const char *);
typedef void (*WebviewPostMessageFn)(uint32_t, const char *);

static const char *(*core_last_error)(void);
static int (*core_run_main_thread)(const char *, const char *, const char *, int);
static bool (*core_configure_webview_runtime)(uint32_t, const char *, const char *);
static uint32_t (*core_get_window_style)(bool, bool, bool, bool, bool, bool, bool, bool, bool, bool, bool, bool);
static uint32_t (*core_create_window)(double, double, double, double, uint32_t, const char *, bool, const char *, bool, bool, bool, double, double,
                                      void *, void *, void *, void *, void *, void *, void *);
static void (*core_set_next_webview_allowed_protocols)(bool, bool);
static uint32_t (*core_create_webview)(uint32_t, uint32_t, const char *, const char *, double, double, double, double, bool, const char *,
                                       DecideNavigationFn, WebviewEventFn, WebviewPostMessageFn, WebviewPostMessageFn, WebviewPostMessageFn,
                                       const char *, const char *, const char *, bool, bool, bool, bool);

static char g_exe_dir[PATH_MAX];
static char g_resources_dir[PATH_MAX];
static JSGlobalContextRef g_ctx;

static uint32_t allow_all_navigation(uint32_t id, const char *url) { (void)id; (void)url; return 1; }
static void noop_event(uint32_t id, const char *a, const char *b) { (void)id; (void)a; (void)b; }
static void noop_message(uint32_t id, const char *m) { (void)id; (void)m; }

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(buf); return NULL; }
    fclose(f);
    buf[len] = 0;
    if (len_out) *len_out = (size_t)len;
    return buf;
}

// --- value helpers ---------------------------------------------------------
static char *arg_string(JSContextRef ctx, size_t argc, const JSValueRef argv[], size_t i) {
    if (i >= argc) return strdup("");
    JSStringRef s = JSValueToStringCopy(ctx, argv[i], NULL);
    size_t max = JSStringGetMaximumUTF8CStringSize(s);
    char *out = malloc(max);
    JSStringGetUTF8CString(s, out, max);
    JSStringRelease(s);
    return out;
}
static double arg_number(JSContextRef ctx, size_t argc, const JSValueRef argv[], size_t i) {
    return i < argc ? JSValueToNumber(ctx, argv[i], NULL) : 0;
}
static bool arg_bool(JSContextRef ctx, size_t argc, const JSValueRef argv[], size_t i) {
    return i < argc ? JSValueToBoolean(ctx, argv[i]) : false;
}
static JSValueRef make_string(JSContextRef ctx, const char *s) {
    JSStringRef js = JSStringCreateWithUTF8CString(s ? s : "");
    JSValueRef v = JSValueMakeString(ctx, js);
    JSStringRelease(js);
    return v;
}
#define HOST_FN(name) static JSValueRef name(JSContextRef ctx, JSObjectRef fn, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc)
#define UNUSED (void)fn; (void)self; (void)exc

// --- host functions --------------------------------------------------------
HOST_FN(h_print) {
    UNUSED;
    char *s = arg_string(ctx, argc, argv, 0);
    fprintf(stderr, "%s\n", s);
    free(s);
    return JSValueMakeUndefined(ctx);
}
HOST_FN(h_read_file) {
    UNUSED;
    char *path = arg_string(ctx, argc, argv, 0);
    char *data = read_file(path, NULL);
    free(path);
    if (!data) return JSValueMakeNull(ctx);
    JSValueRef v = make_string(ctx, data);
    free(data);
    return v;
}
// Comma-separated random bytes, the format the core expects for webview keys.
HOST_FN(h_random_bytes) {
    UNUSED;
    size_t n = (size_t)arg_number(ctx, argc, argv, 0);
    if (n == 0 || n > 4096) return JSValueMakeNull(ctx);
    uint8_t *bytes = malloc(n);
    arc4random_buf(bytes, n);
    char *out = malloc(n * 4 + 1), *p = out;
    for (size_t i = 0; i < n; i++) p += sprintf(p, i ? ",%u" : "%u", bytes[i]);
    JSValueRef v = make_string(ctx, out);
    free(bytes); free(out);
    return v;
}
HOST_FN(h_last_error) { UNUSED; (void)argc; (void)argv; return make_string(ctx, core_last_error()); }
HOST_FN(h_configure_webview_runtime) {
    UNUSED;
    char *full = arg_string(ctx, argc, argv, 1), *sandboxed = arg_string(ctx, argc, argv, 2);
    bool ok = core_configure_webview_runtime((uint32_t)arg_number(ctx, argc, argv, 0), full, sandboxed);
    free(full); free(sandboxed);
    return JSValueMakeBoolean(ctx, ok);
}
HOST_FN(h_get_window_style) {
    UNUSED;
    bool b[12];
    for (size_t i = 0; i < 12; i++) b[i] = arg_bool(ctx, argc, argv, i);
    return JSValueMakeNumber(ctx, core_get_window_style(b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]));
}
HOST_FN(h_create_window) {
    UNUSED;
    char *title_bar_style = arg_string(ctx, argc, argv, 5), *title = arg_string(ctx, argc, argv, 7);
    uint32_t id = core_create_window(arg_number(ctx, argc, argv, 0), arg_number(ctx, argc, argv, 1), arg_number(ctx, argc, argv, 2),
                                     arg_number(ctx, argc, argv, 3), (uint32_t)arg_number(ctx, argc, argv, 4), title_bar_style,
                                     arg_bool(ctx, argc, argv, 6), title, arg_bool(ctx, argc, argv, 8), arg_bool(ctx, argc, argv, 9),
                                     arg_bool(ctx, argc, argv, 10), arg_number(ctx, argc, argv, 11), arg_number(ctx, argc, argv, 12),
                                     NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    free(title_bar_style); free(title);
    return JSValueMakeNumber(ctx, id);
}
HOST_FN(h_set_next_webview_allowed_protocols) {
    UNUSED;
    core_set_next_webview_allowed_protocols(arg_bool(ctx, argc, argv, 0), arg_bool(ctx, argc, argv, 1));
    return JSValueMakeUndefined(ctx);
}
HOST_FN(h_create_webview) {
    UNUSED;
    char *renderer = arg_string(ctx, argc, argv, 2), *url = arg_string(ctx, argc, argv, 3), *partition = arg_string(ctx, argc, argv, 9),
         *secret = arg_string(ctx, argc, argv, 10), *preload = arg_string(ctx, argc, argv, 11), *views_root = arg_string(ctx, argc, argv, 12);
    uint32_t id = core_create_webview((uint32_t)arg_number(ctx, argc, argv, 0), (uint32_t)arg_number(ctx, argc, argv, 1), renderer, url,
                                      arg_number(ctx, argc, argv, 4), arg_number(ctx, argc, argv, 5), arg_number(ctx, argc, argv, 6),
                                      arg_number(ctx, argc, argv, 7), arg_bool(ctx, argc, argv, 8), partition, allow_all_navigation, noop_event,
                                      noop_message, noop_message, noop_message, secret, preload, views_root, arg_bool(ctx, argc, argv, 13),
                                      arg_bool(ctx, argc, argv, 14), arg_bool(ctx, argc, argv, 15), arg_bool(ctx, argc, argv, 16));
    free(renderer); free(url); free(partition); free(secret); free(preload); free(views_root);
    return JSValueMakeNumber(ctx, id);
}

static void set_fn(JSContextRef ctx, JSObjectRef obj, const char *name, JSObjectCallAsFunctionCallback cb) {
    JSStringRef n = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(ctx, obj, n, JSObjectMakeFunctionWithCallback(ctx, n, cb), kJSPropertyAttributeNone, NULL);
    JSStringRelease(n);
}
static void set_str(JSContextRef ctx, JSObjectRef obj, const char *name, const char *value) {
    JSStringRef n = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(ctx, obj, n, make_string(ctx, value), kJSPropertyAttributeNone, NULL);
    JSStringRelease(n);
}

static void report_exception(JSContextRef ctx, JSValueRef exc) {
    JSStringRef s = JSValueToStringCopy(ctx, exc, NULL);
    size_t max = JSStringGetMaximumUTF8CStringSize(s);
    char *out = malloc(max);
    JSStringGetUTF8CString(s, out, max);
    fprintf(stderr, "[jsc-host] uncaught: %s\n", out);
    free(out);
    JSStringRelease(s);
}

static void *js_thread(void *arg) {
    (void)arg;
    usleep(150 * 1000); // like the Zig SDK: let the main thread enter the run loop first
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/app.js", g_resources_dir);
    char *src = read_file(path, NULL);
    if (!src) { fprintf(stderr, "[jsc-host] can't read %s\n", path); return NULL; }
    JSStringRef script = JSStringCreateWithUTF8CString(src);
    JSStringRef url = JSStringCreateWithUTF8CString("app.js");
    JSValueRef exc = NULL;
    JSEvaluateScript(g_ctx, script, NULL, url, 1, &exc);
    if (exc) report_exception(g_ctx, exc);
    JSStringRelease(script); JSStringRelease(url);
    free(src);
    return NULL;
}

static void *must(void *lib, const char *name) {
    void *sym = dlsym(lib, name);
    if (!sym) { fprintf(stderr, "[jsc-host] missing core symbol %s\n", name); exit(1); }
    return sym;
}

// version.json fields, read with JSC's own JSON parser.
static char *json_field(JSContextRef ctx, JSValueRef obj, const char *key) {
    JSStringRef k = JSStringCreateWithUTF8CString(key);
    JSValueRef v = JSObjectGetProperty(ctx, (JSObjectRef)obj, k, NULL);
    JSStringRelease(k);
    const JSValueRef args[1] = {v};
    return arg_string(ctx, 1, args, 0);
}

int main(void) {
    char exe[PATH_MAX], real[PATH_MAX];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0 || !realpath(exe, real)) return 1;
    strncpy(g_exe_dir, real, sizeof g_exe_dir);
    *strrchr(g_exe_dir, '/') = 0;
    snprintf(g_resources_dir, sizeof g_resources_dir, "%s/../Resources", g_exe_dir);

    char lib_path[PATH_MAX];
    snprintf(lib_path, sizeof lib_path, "%s/libElectrobunCore.dylib", g_exe_dir);
    void *lib = dlopen(lib_path, RTLD_NOW);
    if (!lib) { fprintf(stderr, "[jsc-host] %s\n", dlerror()); return 1; }
    core_last_error = must(lib, "electrobun_core_last_error");
    core_run_main_thread = must(lib, "electrobun_core_run_main_thread");
    core_configure_webview_runtime = must(lib, "configureWebviewRuntime");
    core_get_window_style = must(lib, "getWindowStyle");
    core_create_window = must(lib, "createWindow");
    core_set_next_webview_allowed_protocols = must(lib, "setNextWebviewAllowedProtocols");
    core_create_webview = must(lib, "createWebview");

    g_ctx = JSGlobalContextCreate(NULL);
    JSObjectRef host = JSObjectMake(g_ctx, NULL, NULL);
    set_fn(g_ctx, host, "print", h_print);
    set_fn(g_ctx, host, "readFile", h_read_file);
    set_fn(g_ctx, host, "lastError", h_last_error);
    set_fn(g_ctx, host, "randomBytes", h_random_bytes);
    set_fn(g_ctx, host, "configureWebviewRuntime", h_configure_webview_runtime);
    set_fn(g_ctx, host, "getWindowStyle", h_get_window_style);
    set_fn(g_ctx, host, "createWindow", h_create_window);
    set_fn(g_ctx, host, "setNextWebviewAllowedProtocols", h_set_next_webview_allowed_protocols);
    set_fn(g_ctx, host, "createWebview", h_create_webview);
    set_str(g_ctx, host, "resourcesDir", g_resources_dir);
    JSStringRef host_name = JSStringCreateWithUTF8CString("__host");
    JSObjectSetProperty(g_ctx, JSContextGetGlobalObject(g_ctx), host_name, host, kJSPropertyAttributeNone, NULL);
    JSStringRelease(host_name);

    char version_path[PATH_MAX];
    snprintf(version_path, sizeof version_path, "%s/version.json", g_resources_dir);
    char *version_src = read_file(version_path, NULL);
    if (!version_src) { fprintf(stderr, "[jsc-host] can't read version.json\n"); return 1; }
    JSStringRef version_js = JSStringCreateWithUTF8CString(version_src);
    JSValueRef version = JSValueMakeFromJSONString(g_ctx, version_js);
    JSStringRelease(version_js);
    free(version_src);
    char *identifier = json_field(g_ctx, version, "identifier");
    char *name = json_field(g_ctx, version, "name");
    char *channel = json_field(g_ctx, version, "channel");

    pthread_t thread;
    pthread_create(&thread, NULL, js_thread, NULL);
    pthread_detach(thread);

    int status = core_run_main_thread(identifier, name, channel, 0);
    if (status != 0) fprintf(stderr, "[jsc-host] run_main_thread: %s\n", core_last_error());
    return status;
}
