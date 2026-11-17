// HTTP and WebSocket clients for the Livewire JSC runtime, on the system's
// own network stack: NSURLSession on macOS (http-darwin.m), libsoup 3 on Linux
// (http-linux.c). node.c owns the JS side (callbacks, ids, loop refs); a
// backend only starts work and reports events through http_emit.
#pragma once
#include "runtime.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Why a request or a WebSocket failed, mapped to Bun's error codes in fetch.js.
enum { HTTP_FAIL_OTHER, HTTP_FAIL_REFUSED, HTTP_FAIL_DNS, HTTP_FAIL_TIMEOUT, HTTP_FAIL_RESET, HTTP_FAIL_TLS, HTTP_FAIL_REDIRECT, HTTP_FAIL_URL,
       HTTP_FAIL_CERT_EXPIRED };

typedef enum {
    HTTP_EV_RESPONSE, // status, headers, text = final URL, flag = redirected
    HTTP_EV_DATA,     // bytes
    HTTP_EV_END,      // final
    HTTP_EV_ERROR,    // final: number = HTTP_FAIL_*, text = message
    HTTP_EV_ABORTED,  // final: cancelled by http_backend_cancel
    WS_EV_OPEN,       // text = protocol
    WS_EV_MESSAGE,    // bytes (flag = binary)
    WS_EV_SENT,       // number = bytes written
    WS_EV_ERROR,      // number = HTTP_FAIL_*, text = message (a close follows)
    WS_EV_CLOSE,      // final: number = code, text = reason, flag = clean
} http_ev_kind;

typedef struct http_ev {
    long id;
    http_ev_kind kind;
    double number;
    bool flag;
    char *text;      // malloc'd UTF-8 or NULL
    uint8_t *bytes;  // malloc'd or NULL
    size_t len;
    char **headers;  // malloc'd flat name, value, name, value…; NULL-terminated
} http_ev;

// Thread-safe: hands an event (malloc'd, with malloc'd fields) to the JS thread.
void http_emit(jsr_runtime *rt, http_ev *ev);
http_ev *http_ev_new(long id, http_ev_kind kind);

// Backends: called on the JS thread. headers are flat and NULL-terminated.
// redirect: 0 follow, 1 manual (the 3xx is the response), 2 error.
void http_backend_request(jsr_runtime *rt, long id, const char *url, const char *method, char **headers,
                          const uint8_t *body, size_t body_len, int redirect);
void http_backend_cancel(jsr_runtime *rt, long id);
void http_backend_ws_open(jsr_runtime *rt, long id, const char *url, char **protocols, char **headers);
void http_backend_ws_send(jsr_runtime *rt, long id, bool binary, const uint8_t *data, size_t len);
void http_backend_ws_close(jsr_runtime *rt, long id, int code, const char *reason);
