// The app host for mainProcess "jsc": the app's MacOS/main binary on macOS.
// The main thread runs Livewire's core (AppKit) the way Bun's launcher does;
// a JS thread runs the app's bundled main process (Resources/app/bun/index.js)
// on the system JavaScriptCore, with the runtime, the Node shims and the
// generated bun:ffi glue installed.
#include "runtime.h"
#include "node.h"
#include "ffi/ffi.h"

#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_exe_dir[PATH_MAX];
static int g_argc;
static const char **g_argv;

static void *js_main(void *arg) {
    (void)arg;
    jsr_runtime *rt = jsr_create(g_argc, g_argv);
    ffi_install(rt);
    node_install(rt);
    char app[PATH_MAX];
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

int main(int argc, const char **argv) {
    g_argc = argc;
    g_argv = argv;
    char exe[PATH_MAX], real[PATH_MAX];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0 || !realpath(exe, real)) return 1;
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", real);
    *strrchr(g_exe_dir, '/') = 0;
    // The SDK reads ../Resources/... relative to the working directory, as
    // under Bun's launcher.
    if (chdir(g_exe_dir) != 0) return 1;

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/../Resources/version.json", g_exe_dir);
    FILE *f = fopen(path, "rb");
    char json[16384] = "{}";
    if (f) { size_t n = fread(json, 1, sizeof json - 1, f); json[n] = 0; fclose(f); }
    JSGlobalContextRef ctx = JSGlobalContextCreate(NULL);
    JSStringRef js = JSStringCreateWithUTF8CString(json);
    JSValueRef version = JSValueMakeFromJSONString(ctx, js);
    JSStringRelease(js);
    char *identifier = version_field(ctx, version, "identifier");
    char *name = version_field(ctx, version, "name");
    char *channel = version_field(ctx, version, "channel");
    JSGlobalContextRelease(ctx);

    snprintf(path, sizeof path, "%s/libElectrobunCore.dylib", g_exe_dir);
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
