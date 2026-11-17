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
