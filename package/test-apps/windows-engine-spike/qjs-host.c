// Option B: the main process on QuickJS-ng linked into the host. The script
// gets a global "host" with echo (a native call), now and print.
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "quickjs.h"

static LARGE_INTEGER freq, start;

static double now_ms(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)(c.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
}
static JSValue js_echo(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    int32_t v = 0;
    if (argc > 0) JS_ToInt32(ctx, &v, argv[0]);
    return JS_NewInt32(ctx, v);
}
static JSValue js_now(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, now_ms());
}
static JSValue js_print(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    const char *s = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (s) { fputs(s, stdout); fputc('\n', stdout); fflush(stdout); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}
static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    if (argc < 2) { fprintf(stderr, "usage: qjs-host main.js\n"); return 2; }
    size_t len;
    char *source = read_all(argv[1], &len);
    if (!source) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue host = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, host, "echo", JS_NewCFunction(ctx, js_echo, "echo", 1));
    JS_SetPropertyStr(ctx, host, "now", JS_NewCFunction(ctx, js_now, "now", 0));
    JS_SetPropertyStr(ctx, host, "print", JS_NewCFunction(ctx, js_print, "print", 1));
    JS_SetPropertyStr(ctx, global, "host", host);
    JS_FreeValue(ctx, global);
    JSValue r = JS_Eval(ctx, source, len, argv[1], JS_EVAL_TYPE_GLOBAL);
    int code = 0;
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, e);
        fprintf(stderr, "error: %s\n", msg ? msg : "?");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, e);
        code = 1;
    }
    JS_FreeValue(ctx, r);
    JSContext *job_ctx;
    while (JS_ExecutePendingJob(rt, &job_ctx) > 0) {}
    Sleep(6000);
    return code;
}
