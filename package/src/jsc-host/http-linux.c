// fetch and WebSocket on libsoup 3, which WebKitGTK already ships (see
// http.h). libsoup and the GIO under it schedule each step on the thread's
// default main context at that moment, and the JS thread deliberately has
// none (runtime.c), so libsoup runs on a network thread of its own, with its
// own context as that thread's default. Calls from the JS thread hop onto it;
// events go back through http_emit. As the macOS backend's delegate queue.
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

static SoupSession *session; // network thread only, as is everything below but the entry points
static http_job *jobs;
static GMainContext *net_context;

static gpointer net_main(gpointer data) {
    (void)data;
    g_main_context_push_thread_default(net_context);
    // No cookie jar or cache (as Bun's fetch); the system's proxies.
    session = soup_session_new_with_options("timeout", 300, "max-conns", 256, "max-conns-per-host", 256, NULL);
    GMainLoop *loop = g_main_loop_new(net_context, FALSE);
    g_main_loop_run(loop);
    return NULL;
}
// Runs fn(data) on the network thread, starting it the first time.
static void on_net(GSourceFunc fn, gpointer data) {
    static gsize started; // once across JS threads (Workers fetch too)
    if (g_once_init_enter(&started)) {
        net_context = g_main_context_new();
        g_thread_unref(g_thread_new("livewire-net", net_main, NULL));
        g_once_init_leave(&started, 1);
    }
    g_main_context_invoke(net_context, fn, data);
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
        case G_IO_ERROR_CONNECTION_CLOSED: return HTTP_FAIL_RESET; // also G_IO_ERROR_BROKEN_PIPE
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
    g_input_stream_read_bytes_async(job->in, 64 * 1024, G_PRIORITY_DEFAULT, job->cancel, read_ready, job);
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
    g_input_stream_read_bytes_async(job->in, 64 * 1024, G_PRIORITY_DEFAULT, job->cancel, read_ready, job);
}

static void restarted(SoupMessage *msg, gpointer data) {
    (void)msg;
    ((http_job *)data)->redirected = true;
}

// A call from the JS thread, with copies of its arguments.
typedef struct net_call {
    jsr_runtime *rt;
    long id;
    char *url, *method, *reason;
    char **headers, **protocols;
    GBytes *body;
    int redirect, code;
    bool binary;
} net_call;
static char **copy_list(char **list) { return list ? g_strdupv(list) : NULL; }
static void free_call(net_call *c) {
    g_free(c->url);
    g_free(c->method);
    g_free(c->reason);
    g_strfreev(c->headers);
    g_strfreev(c->protocols);
    if (c->body) g_bytes_unref(c->body);
    g_free(c);
}

static gboolean start_request(gpointer data) {
    net_call *c = data;
    SoupMessage *msg = soup_message_new(c->method, c->url);
    if (!msg) {
        emit_fail(c->rt, c->id, HTTP_EV_ERROR, HTTP_FAIL_URL, "invalid URL");
        free_call(c);
        return G_SOURCE_REMOVE;
    }
    http_job *job = new_job(c->rt, c->id);
    job->msg = msg;
    job->redirect = c->redirect;
    SoupMessageHeaders *h = soup_message_get_request_headers(msg);
    for (size_t i = 0; c->headers && c->headers[i] && c->headers[i + 1]; i += 2) soup_message_headers_append(h, c->headers[i], c->headers[i + 1]);
    if (c->body) soup_message_set_request_body_from_bytes(msg, NULL, c->body); // NULL keeps the request's own content type
    if (c->redirect != 0) soup_message_add_flags(msg, SOUP_MESSAGE_NO_REDIRECT);
    g_signal_connect(msg, "restarted", G_CALLBACK(restarted), job);
    soup_session_send_async(session, msg, G_PRIORITY_DEFAULT, job->cancel, send_ready, job);
    free_call(c);
    return G_SOURCE_REMOVE;
}
void http_backend_request(jsr_runtime *rt, long id, const char *url, const char *method, char **headers,
                          const uint8_t *body, size_t body_len, int redirect) {
    net_call *c = g_new0(net_call, 1);
    c->rt = rt;
    c->id = id;
    c->url = g_strdup(url);
    c->method = g_strdup(method);
    c->headers = copy_list(headers);
    c->body = body ? g_bytes_new(body, body_len) : NULL;
    c->redirect = redirect;
    on_net(start_request, c);
}

static gboolean cancel_job(gpointer data) {
    net_call *c = data;
    http_job *job = find_job(c->id);
    if (job) g_cancellable_cancel(job->cancel);
    free_call(c);
    return G_SOURCE_REMOVE;
}
void http_backend_cancel(jsr_runtime *rt, long id) {
    net_call *c = g_new0(net_call, 1);
    c->rt = rt;
    c->id = id;
    on_net(cancel_job, c);
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

static gboolean start_socket(gpointer data) {
    net_call *c = data;
    // libsoup speaks the handshake over http(s) URLs.
    char *url = !strncmp(c->url, "ws:", 3) ? g_strconcat("http:", c->url + 3, NULL)
              : !strncmp(c->url, "wss:", 4) ? g_strconcat("https:", c->url + 4, NULL)
              : g_strdup(c->url);
    SoupMessage *msg = soup_message_new("GET", url);
    g_free(url);
    if (!msg) {
        emit_fail(c->rt, c->id, WS_EV_ERROR, HTTP_FAIL_URL, "invalid URL");
        http_ev *ev = http_ev_new(c->id, WS_EV_CLOSE);
        ev->number = 1006;
        ev->text = strdup("");
        http_emit(c->rt, ev);
        free_call(c);
        return G_SOURCE_REMOVE;
    }
    http_job *job = new_job(c->rt, c->id);
    job->socket = true;
    job->msg = msg;
    SoupMessageHeaders *h = soup_message_get_request_headers(msg);
    for (size_t i = 0; c->headers && c->headers[i] && c->headers[i + 1]; i += 2) soup_message_headers_append(h, c->headers[i], c->headers[i + 1]);
    soup_session_websocket_connect_async(session, msg, NULL, c->protocols && c->protocols[0] ? c->protocols : NULL, G_PRIORITY_DEFAULT,
                                         job->cancel, ws_ready, job);
    free_call(c);
    return G_SOURCE_REMOVE;
}
void http_backend_ws_open(jsr_runtime *rt, long id, const char *url, char **protocols, char **headers) {
    net_call *c = g_new0(net_call, 1);
    c->rt = rt;
    c->id = id;
    c->url = g_strdup(url);
    c->protocols = copy_list(protocols);
    c->headers = copy_list(headers);
    on_net(start_socket, c);
}

static gboolean send_frame(gpointer data) {
    net_call *c = data;
    http_job *job = find_job(c->id);
    if (job && job->conn && soup_websocket_connection_get_state(job->conn) == SOUP_WEBSOCKET_STATE_OPEN) {
        gsize len = 0;
        const void *bytes = g_bytes_get_data(c->body, &len);
        if (c->binary) soup_websocket_connection_send_binary(job->conn, bytes, len);
        else soup_websocket_connection_send_message(job->conn, SOUP_WEBSOCKET_DATA_TEXT, c->body); // keeps NULs, unlike send_text
        // libsoup queues the frame; count it written now.
        http_ev *ev = http_ev_new(c->id, WS_EV_SENT);
        ev->number = (double)len;
        http_emit(c->rt, ev);
    }
    free_call(c);
    return G_SOURCE_REMOVE;
}
void http_backend_ws_send(jsr_runtime *rt, long id, bool binary, const uint8_t *data, size_t len) {
    net_call *c = g_new0(net_call, 1);
    c->rt = rt;
    c->id = id;
    c->binary = binary;
    c->body = g_bytes_new(data, len);
    on_net(send_frame, c);
}

static gboolean close_socket(gpointer data) {
    net_call *c = data;
    http_job *job = find_job(c->id);
    if (job && !job->client_code) {
        // 0: close() without a code, a close frame with no status (1005 here)
        job->client_code = c->code ? c->code : SOUP_WEBSOCKET_CLOSE_NO_STATUS;
        job->client_reason = strdup(c->reason);
        if (job->conn) soup_websocket_connection_close(job->conn, c->code ? (gushort)c->code : SOUP_WEBSOCKET_CLOSE_NO_STATUS, c->code ? c->reason : NULL);
        else g_cancellable_cancel(job->cancel);
    }
    free_call(c);
    return G_SOURCE_REMOVE;
}
void http_backend_ws_close(jsr_runtime *rt, long id, int code, const char *reason) {
    net_call *c = g_new0(net_call, 1);
    c->rt = rt;
    c->id = id;
    c->code = code;
    c->reason = g_strdup(reason);
    on_net(close_socket, c);
}
