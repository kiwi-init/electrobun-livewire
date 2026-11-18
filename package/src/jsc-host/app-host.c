// The app host for mainProcess "jsc": the app's MacOS/main binary on macOS,
// bin/main on Linux. The main thread runs Livewire's core (AppKit or GTK) the
// way Bun's launcher does;
// a JS thread runs the app's bundled main process (Resources/app/bun/index.js)
// on the system JavaScriptCore, with the runtime, the Node shims and the
// generated bun:ffi glue installed.
// Run as `main [--bun-flag ...] script.js [args...]`, it is a plain script
// runner instead, as the app's Bun is for scripts an app runs with
// process.execPath (Channels' kiwi CLI): no core, no windows, the caller's
// working directory, and Bun's own flags before the script ignored.
#include "runtime.h"
#include "node.h"
#include "ffi/glue.h"

#include <dlfcn.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#define CORE_LIB "libElectrobunCore.dylib"
#else
#define CORE_LIB "libElectrobunCore.so"
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_exe_dir[PATH_MAX];
static int g_argc;
static const char **g_argv;

// Natives every runtime gets: this one and each Worker's.
static void install_natives(jsr_runtime *rt) {
    ffi_install(rt);
    node_install(rt);
    sqlite_install(rt);
    worker_install(rt);
}

static void *js_main(void *arg) {
    (void)arg;
    jsr_set_runtime_init(install_natives);
    jsr_runtime *rt = jsr_create(g_argc, g_argv);
    install_natives(rt);
    char app[PATH_MAX + 64];
    snprintf(app, sizeof app, "%s/../Resources/app/bun/index.js", g_exe_dir);
    if (jsr_eval_file(rt, app)) jsr_run(rt, false);
    // process.exit(), or an app that failed to load: the whole app ends, as
    // when Bun's worker exits.
    exit(jsr_exit_code(rt));
}

// identifier/name/channel from Resources/version.json, read with JSC's JSON parser.
static char *version_field(JSGlobalContextRef ctx, JSValueRef version, const char *key) {
    if (!version || !JSValueIsObject(ctx, version)) return strdup("");
    JSStringRef k = JSStringCreateWithUTF8CString(key);
    JSValueRef v = JSObjectGetProperty(ctx, (JSObjectRef)version, k, NULL);
    JSStringRelease(k);
    return JSValueIsString(ctx, v) ? jsr_to_utf8(ctx, v) : strdup("");
}

typedef struct { char *identifier, *name, *channel; } version_info;
static void *read_version(void *arg) {
    version_info *info = arg;
    char path[PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/../Resources/version.json", g_exe_dir);
    FILE *f = fopen(path, "rb");
    char json[16384] = "{}";
    if (f) { size_t n = fread(json, 1, sizeof json - 1, f); json[n] = 0; fclose(f); }
    JSGlobalContextRef ctx = JSGlobalContextCreate(NULL);
    JSStringRef js = JSStringCreateWithUTF8CString(json);
    JSValueRef version = JSValueMakeFromJSONString(ctx, js);
    JSStringRelease(js);
    info->identifier = version_field(ctx, version, "identifier");
    info->name = version_field(ctx, version, "name");
    info->channel = version_field(ctx, version, "channel");
    JSGlobalContextRelease(ctx);
    return NULL;
}

// The script in `main [--flags] script.js [args]`, or 0 when this is the app.
static int script_index(int argc, const char **argv) {
    int i = 1;
    while (i < argc && strncmp(argv[i], "--", 2) == 0 && strcmp(argv[i], "--") != 0) i++;
    if (i < argc && strcmp(argv[i], "--") == 0) i++;
    if (i >= argc) return 0;
    size_t len = strlen(argv[i]);
    bool js = (len > 3 && strcmp(argv[i] + len - 3, ".js") == 0) || (len > 4 && strcmp(argv[i] + len - 4, ".mjs") == 0);
    return js && access(argv[i], R_OK) == 0 ? i : 0;
}

static int run_script(int argc, const char **argv, int at) {
    // process.argv is [execPath, script, args...], as under Bun.
    const char **args = calloc((size_t)(argc - at + 2), sizeof *args);
    int n = 0;
    args[n++] = argv[0];
    for (int i = at; i < argc; i++) args[n++] = argv[i];
    jsr_set_runtime_init(install_natives);
    jsr_runtime *rt = jsr_create(n, args);
    install_natives(rt);
    if (jsr_eval_file(rt, args[1])) jsr_run(rt, true);
    return jsr_exit_code(rt);
}

int main(int argc, const char **argv) {
    int script = script_index(argc, argv);
    if (script) return run_script(argc, argv, script);
    g_argc = argc;
    g_argv = argv;
    char real[PATH_MAX];
#ifdef __APPLE__
    char exe[PATH_MAX];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0 || !realpath(exe, real)) return 1;
#else
    if (!realpath("/proc/self/exe", real)) return 1;
#endif
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", real);
    *strrchr(g_exe_dir, '/') = 0;
    // The SDK reads ../Resources/... relative to the working directory, as
    // under Bun's launcher.
    if (chdir(g_exe_dir) != 0) return 1;

    // identifier/name/channel from version.json, parsed on a short-lived
    // thread: the main thread must not touch JavaScriptCore before WebKit
    // does. On Linux, WebKitGTK shares this process's JSC, and a VM created
    // on the main thread first gives it a private GMainContext that becomes
    // WebKit's main run loop. GTK never iterates that context, so pages never load.
    version_info info = {0};
    pthread_t reader;
    if (pthread_create(&reader, NULL, read_version, &info) != 0) return 1;
    pthread_join(reader, NULL);
    char *identifier = info.identifier, *name = info.name, *channel = info.channel;
    char path[PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/" CORE_LIB, g_exe_dir);
    void *core = dlopen(path, RTLD_NOW);
    if (!core) { fprintf(stderr, "[jsc-host] %s\n", dlerror()); return 1; }
    int (*run_main_thread)(const char *, const char *, const char *, int) = dlsym(core, "electrobun_core_run_main_thread");
    const char *(*last_error)(void) = dlsym(core, "electrobun_core_last_error");
    if (!run_main_thread || !last_error) { fprintf(stderr, "[jsc-host] core symbols missing\n"); return 1; }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024); // JS recursion needs more than 512 KB
    pthread_t thread;
    pthread_create(&thread, &attr, js_main, NULL);
    pthread_detach(thread);

    int status = run_main_thread(identifier, name, channel, 0);
    if (status != 0) fprintf(stderr, "[jsc-host] ElectrobunCore failed: %s\n", last_error());
    return status;
}
