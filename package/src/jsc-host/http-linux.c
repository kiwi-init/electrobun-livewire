// fetch and WebSocket on libsoup 3, which WebKitGTK already ships (see
// http.h). A libsoup session belongs to the thread-default main context it
// was made in, so every call runs with the runtime's GMainContext pushed:
// libsoup's callbacks then run on the JS thread, inside its loop.
#include "http.h"
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>

typedef struct http_job {
    jsr_runtime *rt;
    long id;
    int redirect;
    bool redirected;
    bool finished;
    bool socket;
    int client_code; // a WebSocket's close(): what its close event reports
    char *client_reason;
    SoupMessage *msg;
    GCancellable *cancel;
    GInputStream *in;
    SoupWebsocketConnection *conn;
    struct http_job *next;
} http_job;

static SoupSession *session;
static http_job *jobs;

#define IN_CONTEXT(rt) for (int once_ = (g_main_context_push_thread_default(jsr_glib_context(rt)), 1); once_; \
                            once_ = (g_main_context_pop_thread_default(jsr_glib_context(rt)), 0))

static void ensure_session(jsr_runtime *rt) {
    if (session) return;
    IN_CONTEXT(rt) {
        // No cookie jar or cache (as Bun's fetch); the system's proxies.
        session = soup_session_new_with_options("timeout", 300, "max-conns", 256, "max-conns-per-host", 256, NULL);
    }
}

static http_job *find_job(long id) {
    for (http_job *j = jobs; j; j = j->next)
        if (j->id == id) return j;
    return NULL;
}
static void drop_job(http_job *job) {
    for (http_job **link = &jobs; *link; link = &(*link)->next)
        if (*link == job) {
            *link = job->next;
            break;
        }
    if (job->in) g_object_unref(job->in);
    if (job->conn) {
        g_signal_handlers_disconnect_by_data(job->conn, job);
        g_object_unref(job->conn);
    }
    if (job->msg) {
        g_signal_handlers_disconnect_by_data(job->msg, job);
        g_object_unref(job->msg);
    }
    g_object_unref(job->cancel);
    free(job->client_reason);
    free(job);
}
static http_job *new_job(jsr_runtime *rt, long id) {
    http_job *job = calloc(1, sizeof *job);
    job->rt = rt;
    job->id = id;
    job->cancel = g_cancellable_new();
    job->next = jobs;
    jobs = job;
    return job;
}

static int fail_kind(const GError *e) {
    if (e->domain == G_RESOLVER_ERROR) return HTTP_FAIL_DNS;
    if (e->domain == G_TLS_ERROR) return HTTP_FAIL_TLS;
    if (e->domain == G_IO_ERROR) {
        switch (e->code) {
        case G_IO_ERROR_CONNECTION_REFUSED: return HTTP_FAIL_REFUSED;
        case G_IO_ERROR_TIMED_OUT: return HTTP_FAIL_TIMEOUT;
        case G_IO_ERROR_CONNECTION_CLOSED:
        case G_IO_ERROR_BROKEN_PIPE: return HTTP_FAIL_RESET;
        case G_IO_ERROR_HOST_NOT_FOUND: return HTTP_FAIL_DNS;
        default: break;
        }
    }
    return HTTP_FAIL_OTHER;
}
static void emit_fail(jsr_runtime *rt, long id, http_ev_kind kind, int fail, const char *message) {
    http_ev *ev = http_ev_new(id, kind);
    ev->number = fail;
    ev->text = strdup(message ? message : "");
    http_emit(rt, ev);
}
static void finish(http_job *job, http_ev_kind kind, const GError *error) {
    if (job->finished) return;
    job->finished = true;
    if (kind == HTTP_EV_ERROR) emit_fail(job->rt, job->id, kind, error ? fail_kind(error) : HTTP_FAIL_OTHER, error ? error->message : "");
    else http_emit(job->rt, http_ev_new(job->id, kind));
    drop_job(job);
}
static bool cancelled(const GError *e) { return g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED); }

// --- requests -------------------------------------------------------------------
static void read_ready(GObject *source, GAsyncResult *result, gpointer data) {
    http_job *job = data;
    GError *error = NULL;
    GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
    if (!bytes) {
        finish(job, cancelled(error) ? HTTP_EV_ABORTED : HTTP_EV_ERROR, error);
        g_error_free(error);
        return;
    }
    gsize len = 0;
    const void *at = g_bytes_get_data(bytes, &len);
    if (len == 0) {
        g_bytes_unref(bytes);
        return finish(job, HTTP_EV_END, NULL);
    }
    http_ev *ev = http_ev_new(job->id, HTTP_EV_DATA);
    ev->bytes = malloc(len);
    memcpy(ev->bytes, at, len);
    ev->len = len;
    http_emit(job->rt, ev);
    g_bytes_unref(bytes);
    IN_CONTEXT(job->rt) {
        g_input_stream_read_bytes_async(job->in, 64 * 1024, G_PRIORITY_DEFAULT, job->cancel, read_ready, job);
    }
}

static void send_ready(GObject *source, GAsyncResult *result, gpointer data) {
    http_job *job = data;
    GError *error = NULL;
    job->in = soup_session_send_finish(SOUP_SESSION(source), result, &error);
    if (!job->in) {
        GTlsCertificateFlags flags = soup_message_get_tls_peer_certificate_errors(job->msg);
        if (error->domain == G_TLS_ERROR && (flags & (G_TLS_CERTIFICATE_EXPIRED | G_TLS_CERTIFICATE_NOT_ACTIVATED))) {
            job->finished = true;
            emit_fail(job->rt, job->id, HTTP_EV_ERROR, HTTP_FAIL_CERT_EXPIRED, error->message);
            drop_job(job);
        } else finish(job, cancelled(error) ? HTTP_EV_ABORTED : HTTP_EV_ERROR, error);
        g_error_free(error);
        return;
    }
    guint status = soup_message_get_status(job->msg);
    SoupMessageHeaders *headers = soup_message_get_response_headers(job->msg);
    if (job->redirect == 2 && SOUP_STATUS_IS_REDIRECTION(status) && soup_message_headers_get_one(headers, "Location")) {
        job->finished = true;
        emit_fail(job->rt, job->id, HTTP_EV_ERROR, HTTP_FAIL_REDIRECT, "unexpected redirect");
        return drop_job(job);
    }
    http_ev *ev = http_ev_new(job->id, HTTP_EV_RESPONSE);
    ev->number = status;
    ev->flag = job->redirected;
    char *uri = g_uri_to_string(soup_message_get_uri(job->msg));
    ev->text = strdup(uri);
    g_free(uri);
    size_t count = 0, i = 0;
    SoupMessageHeadersIter iter;
    const char *name, *value;
    soup_message_headers_iter_init(&iter, headers);
    while (soup_message_headers_iter_next(&iter, &name, &value)) count++;
    ev->headers = calloc(count * 2 + 1, sizeof(char *));
    soup_message_headers_iter_init(&iter, headers);
    while (soup_message_headers_iter_next(&iter, &name, &value)) {
        ev->headers[i++] = strdup(name);
        ev->headers[i++] = strdup(value);
    }
    http_emit(job->rt, ev);
    IN_CONTEXT(job->rt) {
        g_input_stream_read_bytes_async(job->in, 64 * 1024, G_PRIORITY_DEFAULT, job->cancel, read_ready, job);
    }
}

static void restarted(SoupMessage *msg, gpointer data) {
    (void)msg;
    ((http_job *)data)->redirected = true;
}

void http_backend_request(jsr_runtime *rt, long id, const char *url, const char *method, char **headers,
                          const uint8_t *body, size_t body_len, int redirect) {
    ensure_session(rt);
    IN_CONTEXT(rt) {
        SoupMessage *msg = soup_message_new(method, url);
        if (!msg) {
            emit_fail(rt, id, HTTP_EV_ERROR, HTTP_FAIL_URL, "invalid URL");
            continue;
        }
        http_job *job = new_job(rt, id);
        job->msg = msg;
        job->redirect = redirect;
        SoupMessageHeaders *h = soup_message_get_request_headers(msg);
        for (size_t i = 0; headers && headers[i] && headers[i + 1]; i += 2) soup_message_headers_append(h, headers[i], headers[i + 1]);
        if (body) {
            GBytes *bytes = g_bytes_new(body, body_len);
            soup_message_set_request_body_from_bytes(msg, NULL, bytes); // NULL keeps the request's own content type
            g_bytes_unref(bytes);
        }
        if (redirect != 0) soup_message_add_flags(msg, SOUP_MESSAGE_NO_REDIRECT);
        g_signal_connect(msg, "restarted", G_CALLBACK(restarted), job);
        soup_session_send_async(session, msg, G_PRIORITY_DEFAULT, job->cancel, send_ready, job);
    }
}

void http_backend_cancel(jsr_runtime *rt, long id) {
    (void)rt;
    http_job *job = find_job(id);
    if (job) g_cancellable_cancel(job->cancel);
}

// --- WebSockets -----------------------------------------------------------------
static void ws_finish(http_job *job, int code, const char *reason, bool clean) {
    if (job->finished) return;
    job->finished = true;
    http_ev *ev = http_ev_new(job->id, WS_EV_CLOSE);
    if (job->client_code) {
        code = job->client_code;
        reason = job->client_reason;
        clean = true;
    }
    ev->number = code;
    ev->flag = clean;
    ev->text = strdup(reason ? reason : "");
    http_emit(job->rt, ev);
    drop_job(job);
}
static void ws_message(SoupWebsocketConnection *conn, gint type, GBytes *message, gpointer data) {
    (void)conn;
    http_job *job = data;
    gsize len = 0;
    const void *at = g_bytes_get_data(message, &len);
    http_ev *ev = http_ev_new(job->id, WS_EV_MESSAGE);
    ev->flag = type == SOUP_WEBSOCKET_DATA_BINARY;
    ev->bytes = malloc(len ? len : 1);
    memcpy(ev->bytes, at, len);
    ev->len = len;
    http_emit(job->rt, ev);
}
static void ws_error(SoupWebsocketConnection *conn, GError *error, gpointer data) {
    (void)conn;
    http_job *job = data;
    emit_fail(job->rt, job->id, WS_EV_ERROR, fail_kind(error), error->message);
}
static void ws_closed(SoupWebsocketConnection *conn, gpointer data) {
    gushort code = soup_websocket_connection_get_close_code(conn);
    ws_finish(data, code ? code : 1006, soup_websocket_connection_get_close_data(conn), code != 0);
}
static void ws_ready(GObject *source, GAsyncResult *result, gpointer data) {
    http_job *job = data;
    GError *error = NULL;
    job->conn = soup_session_websocket_connect_finish(SOUP_SESSION(source), result, &error);
    if (!job->conn) {
        if (!cancelled(error)) emit_fail(job->rt, job->id, WS_EV_ERROR, fail_kind(error), error->message);
        g_error_free(error);
        return ws_finish(job, 1006, "", false);
    }
    // libsoup's default limit is 128 KB; Bun takes far bigger messages.
    soup_websocket_connection_set_max_incoming_payload_size(job->conn, 128 * 1024 * 1024);
    soup_websocket_connection_set_keepalive_interval(job->conn, 0);
    g_signal_connect(job->conn, "message", G_CALLBACK(ws_message), job);
    g_signal_connect(job->conn, "error", G_CALLBACK(ws_error), job);
    g_signal_connect(job->conn, "closed", G_CALLBACK(ws_closed), job);
    http_ev *ev = http_ev_new(job->id, WS_EV_OPEN);
    const char *protocol = soup_websocket_connection_get_protocol(job->conn);
    ev->text = strdup(protocol ? protocol : "");
    http_emit(job->rt, ev);
    if (job->client_code) soup_websocket_connection_close(job->conn, (gushort)job->client_code, job->client_reason);
}

void http_backend_ws_open(jsr_runtime *rt, long id, const char *url, char **protocols, char **headers) {
    ensure_session(rt);
    // libsoup speaks the handshake over http(s) URLs.
    char *http_url = NULL;
    if (!strncmp(url, "ws:", 3)) http_url = g_strconcat("http:", url + 3, NULL);
    else if (!strncmp(url, "wss:", 4)) http_url = g_strconcat("https:", url + 4, NULL);
    else http_url = g_strdup(url);
    IN_CONTEXT(rt) {
        SoupMessage *msg = soup_message_new("GET", http_url);
        if (!msg) {
            emit_fail(rt, id, WS_EV_ERROR, HTTP_FAIL_URL, "invalid URL");
            http_ev *ev = http_ev_new(id, WS_EV_CLOSE);
            ev->number = 1006;
            ev->text = strdup("");
            http_emit(rt, ev);
            continue;
        }
        http_job *job = new_job(rt, id);
        job->socket = true;
        job->msg = msg;
        SoupMessageHeaders *h = soup_message_get_request_headers(msg);
        for (size_t i = 0; headers && headers[i] && headers[i + 1]; i += 2) soup_message_headers_append(h, headers[i], headers[i + 1]);
        soup_session_websocket_connect_async(session, msg, NULL, protocols && protocols[0] ? protocols : NULL, G_PRIORITY_DEFAULT,
                                             job->cancel, ws_ready, job);
    }
    g_free(http_url);
}

void http_backend_ws_send(jsr_runtime *rt, long id, bool binary, const uint8_t *data, size_t len) {
    http_job *job = find_job(id);
    if (!job || !job->conn || soup_websocket_connection_get_state(job->conn) != SOUP_WEBSOCKET_STATE_OPEN) return;
    IN_CONTEXT(rt) {
        if (binary) soup_websocket_connection_send_binary(job->conn, data, len);
        else {
            char *text = strndup((const char *)data, len);
            soup_websocket_connection_send_text(job->conn, text);
            free(text);
        }
    }
    // libsoup queues the frame; count it written now.
    http_ev *ev = http_ev_new(id, WS_EV_SENT);
    ev->number = (double)len;
    http_emit(rt, ev);
}

void http_backend_ws_close(jsr_runtime *rt, long id, int code, const char *reason) {
    http_job *job = find_job(id);
    if (!job || job->client_code) return;
    // 0: close() without a code, a close frame with no status (1005 here)
    job->client_code = code ? code : SOUP_WEBSOCKET_CLOSE_NO_STATUS;
    job->client_reason = strdup(reason);
    IN_CONTEXT(rt) {
        if (job->conn) soup_websocket_connection_close(job->conn, code ? (gushort)code : SOUP_WEBSOCKET_CLOSE_NO_STATUS, code ? reason : NULL);
        else g_cancellable_cancel(job->cancel);
    }
}
