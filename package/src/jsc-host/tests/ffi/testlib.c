// A small native library for the ffi glue's tests: every argument and return
// type the SDK uses, and callbacks called on the caller's thread and on another.
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int32_t tl_add(int32_t a, int32_t b) { return a + b; }
double tl_mul(double a, double b) { return a * b; }
uint64_t tl_big(void) { return (1ULL << 40) + 5; }
uint64_t tl_u64_echo(uint64_t v) { return v; }
bool tl_not(bool v) { return !v; }
uint32_t tl_strlen(const char *s) { return s ? (uint32_t)strlen(s) : 0; }
const char *tl_greet(const char *name) {
    static char buf[256];
    snprintf(buf, sizeof buf, "hello, %s", name ? name : "(null)");
    return buf;
}
const char *tl_null_string(void) { return NULL; }
void *tl_echo_ptr(void *p) { return p; }
void tl_fill(void *buf, uint32_t n) { for (uint32_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = (uint8_t)(i * 3); }
void tl_write_pair(double *x, double *y) { *x = 1.5; *y = -2.25; }

typedef void (*event_cb)(uint32_t, const char *);
typedef uint32_t (*ret_cb)(uint32_t, const char *);
typedef const char *(*str_cb)(uint32_t);

void tl_call_sync(event_cb cb) { cb(7, "sync"); }
uint32_t tl_call_ret(ret_cb cb) { return cb(41, "x") + 1; }
const char *tl_call_str(str_cb cb) {
    static char copy[256];
    const char *s = cb(3);
    snprintf(copy, sizeof copy, "%s", s ? s : "(null)");
    return copy;
}

// What WebKit does to a views:// page: another thread asks a sync callback for
// a string and waits, while the JS thread is blocked in this very call.
typedef struct { str_cb cb; char out[256]; } str_job;
static void *run_str_job(void *arg) {
    str_job *job = arg;
    const char *s = job->cb(9);
    snprintf(job->out, sizeof job->out, "%s", s ? s : "(null)");
    return NULL;
}
const char *tl_str_via_thread(str_cb cb) {
    static str_job job;
    job.cb = cb;
    pthread_t t;
    pthread_create(&t, NULL, run_str_job, &job);
    pthread_join(t, NULL);
    return job.out;
}

// Not in tests/ffi/decls.json, so the host binds these through libffi
// (ffi/dynamic.inc), as it does an app's own library: every argument and
// return type, more arguments than registers, callbacks of signatures the
// glue has no trampolines for.
int64_t tl_dyn_mix(int8_t a, uint8_t b, int16_t c, uint16_t d, int32_t e, uint32_t f, int64_t g, float h, double i, const char *s, int8_t j, uint16_t k) {
    return a + b + c + d + e + (int64_t)f + g + (int64_t)(h * 4) + (int64_t)(i * 8) + (s ? (int64_t)strlen(s) : 0) + j * 1000 + k * 100000;
}
int8_t tl_dyn_i8(int32_t v) { return (int8_t)v; }
uint16_t tl_dyn_u16(int32_t v) { return (uint16_t)v; }
float tl_dyn_f32(float v) { return v * 2; }
int64_t tl_dyn_i64(int64_t v) { return v - 1; }
uint64_t tl_dyn_u64(uint64_t v) { return v; }
bool tl_dyn_bool(uint16_t v) { return v > 1; }

typedef double (*mix_cb)(int64_t, float, uint8_t, const char *);
double tl_dyn_call_mix(mix_cb cb) { return cb(-5, 1.5f, 200, "mix") * 2; }

typedef int8_t (*i8_cb)(int8_t);
typedef struct { i8_cb cb; int8_t out; } i8_job;
static void *run_i8_job(void *arg) {
    i8_job *job = arg;
    job->out = job->cb(-3);
    return NULL;
}
// A sync callback from another thread while JS waits in this call.
int32_t tl_dyn_i8_via_thread(i8_cb cb) {
    i8_job job = {cb, 0};
    pthread_t t;
    pthread_create(&t, NULL, run_i8_job, &job);
    pthread_join(t, NULL);
    return job.out;
}

typedef void (*pair_cb)(int16_t, double);
typedef struct { pair_cb cb; int16_t v; } pair_job;
static void *run_pair_job(void *arg) {
    pair_job *job = arg;
    usleep(10 * 1000);
    job->cb(job->v, 0.25);
    free(job);
    return NULL;
}
void tl_dyn_call_thread(pair_cb cb, int16_t v) {
    pair_job *job = malloc(sizeof *job);
    job->cb = cb;
    job->v = v;
    pthread_t t;
    pthread_create(&t, NULL, run_pair_job, job);
    pthread_detach(t);
}

typedef struct { event_cb cb; uint32_t delay_ms; } thread_job;
static void *run_job(void *arg) {
    thread_job *job = arg;
    usleep(job->delay_ms * 1000);
    char message[32];
    snprintf(message, sizeof message, "from thread %u", job->delay_ms);
    job->cb(job->delay_ms, message); // freed by the caller right after: the glue must copy it
    memset(message, 'X', sizeof message - 1);
    free(job);
    return NULL;
}
void tl_call_thread(event_cb cb, uint32_t delay_ms) {
    thread_job *job = malloc(sizeof *job);
    job->cb = cb;
    job->delay_ms = delay_ms;
    pthread_t t;
    pthread_create(&t, NULL, run_job, job);
    pthread_detach(t);
}
