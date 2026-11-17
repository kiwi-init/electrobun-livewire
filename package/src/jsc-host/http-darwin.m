// fetch and WebSocket on NSURLSession (see http.h). All state lives on the
// session's serial delegate queue; calls from the JS thread hop onto it.
#import <Foundation/Foundation.h>
#include "http.h"
#include <stdlib.h>
#include <string.h>

@interface LWTask : NSObject
@property(nonatomic) long ident;
@property(nonatomic) int redirect;
@property(nonatomic) BOOL redirected;
@property(nonatomic) BOOL redirectFailed;
@property(nonatomic) BOOL isSocket;
@property(nonatomic) BOOL closed;
@property(nonatomic) NSInteger clientCode;
@property(nonatomic, strong) NSData *clientReason;
@property(nonatomic, strong) NSURLSessionTask *task;
@end
@implementation LWTask
@end

@interface LWDelegate : NSObject <NSURLSessionDataDelegate, NSURLSessionWebSocketDelegate>
@property(nonatomic) jsr_runtime *rt;
@end

static NSOperationQueue *queue;
static NSURLSession *session;
static LWDelegate *delegate;
static NSMutableDictionary<NSNumber *, LWTask *> *byTask; // taskIdentifier → task
static NSMutableDictionary<NSNumber *, LWTask *> *byId;   // JS id → task

static char *dup_string(NSString *s) { return s ? strdup(s.UTF8String ?: "") : NULL; }
static uint8_t *dup_data(NSData *d, size_t *len) {
    *len = d.length;
    uint8_t *out = malloc(d.length ? d.length : 1);
    [d getBytes:out length:d.length];
    return out;
}

static int fail_kind(NSError *e) {
    if (![e.domain isEqualToString:NSURLErrorDomain]) return HTTP_FAIL_OTHER;
    switch (e.code) {
    case NSURLErrorCannotConnectToHost: return HTTP_FAIL_REFUSED;
    case NSURLErrorCannotFindHost:
    case NSURLErrorDNSLookupFailed: return HTTP_FAIL_DNS;
    case NSURLErrorTimedOut: return HTTP_FAIL_TIMEOUT;
    case NSURLErrorNetworkConnectionLost: return HTTP_FAIL_RESET;
    case NSURLErrorUnsupportedURL:
    case NSURLErrorBadURL: return HTTP_FAIL_URL;
    case NSURLErrorServerCertificateHasBadDate:
    case NSURLErrorServerCertificateNotYetValid: return HTTP_FAIL_CERT_EXPIRED;
    case NSURLErrorSecureConnectionFailed:
    case NSURLErrorServerCertificateUntrusted:
    case NSURLErrorServerCertificateHasUnknownRoot:
    case NSURLErrorClientCertificateRejected:
    case NSURLErrorClientCertificateRequired: return HTTP_FAIL_TLS;
    default: return HTTP_FAIL_OTHER;
    }
}

static void emit_fail(jsr_runtime *rt, long ident, http_ev_kind kind, int fail, NSString *message) {
    http_ev *ev = http_ev_new(ident, kind);
    ev->number = fail;
    ev->text = dup_string(message);
    http_emit(rt, ev);
}

static void forget(LWTask *t) {
    [byTask removeObjectForKey:@(t.task.taskIdentifier)];
    [byId removeObjectForKey:@(t.ident)];
}

static void finish_socket(jsr_runtime *rt, LWTask *t, NSInteger code, NSData *reason, BOOL clean) {
    if (t.closed) return;
    t.closed = YES;
    http_ev *ev = http_ev_new(t.ident, WS_EV_CLOSE);
    ev->number = (double)code;
    ev->flag = clean;
    ev->text = reason.length ? strndup(reason.bytes, reason.length) : strdup("");
    http_emit(rt, ev);
    forget(t);
}

static void receive(jsr_runtime *rt, LWTask *t) {
    NSURLSessionWebSocketTask *task = (NSURLSessionWebSocketTask *)t.task;
    [task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *message, NSError *error) {
      if (error || t.closed) return; // didCompleteWithError reports how it ended
      http_ev *ev = http_ev_new(t.ident, WS_EV_MESSAGE);
      if (message.type == NSURLSessionWebSocketMessageTypeString) {
          NSData *d = [message.string dataUsingEncoding:NSUTF8StringEncoding];
          ev->bytes = dup_data(d, &ev->len);
      } else {
          ev->flag = true;
          ev->bytes = dup_data(message.data, &ev->len);
      }
      http_emit(rt, ev);
      receive(rt, t);
    }];
}

@implementation LWDelegate
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)task didReceiveResponse:(NSURLResponse *)response
    completionHandler:(void (^)(NSURLSessionResponseDisposition))done {
    (void)s;
    LWTask *t = byTask[@(task.taskIdentifier)];
    if (t && [response isKindOfClass:[NSHTTPURLResponse class]]) {
        NSHTTPURLResponse *r = (NSHTTPURLResponse *)response;
        http_ev *ev = http_ev_new(t.ident, HTTP_EV_RESPONSE);
        ev->number = (double)r.statusCode;
        ev->flag = t.redirected;
        ev->text = dup_string(r.URL.absoluteString);
        NSDictionary *fields = r.allHeaderFields;
        ev->headers = calloc(fields.count * 2 + 1, sizeof(char *));
        size_t i = 0;
        for (NSString *name in fields) {
            ev->headers[i++] = dup_string(name);
            ev->headers[i++] = dup_string([fields[name] description]);
        }
        http_emit(self.rt, ev);
    }
    done(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    (void)s;
    LWTask *t = byTask[@(task.taskIdentifier)];
    if (!t) return;
    http_ev *ev = http_ev_new(t.ident, HTTP_EV_DATA);
    ev->bytes = dup_data(data, &ev->len);
    http_emit(self.rt, ev);
}
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)response
        newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest *))done {
    (void)s;
    (void)response;
    LWTask *t = byTask[@(task.taskIdentifier)];
    if (!t || t.redirect == 0) {
        t.redirected = YES;
        done(request);
    } else if (t.redirect == 1) {
        done(nil); // the 3xx becomes the response
    } else {
        t.redirectFailed = YES;
        done(nil);
        [task cancel];
    }
}
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    (void)s;
    LWTask *t = byTask[@(task.taskIdentifier)];
    if (!t) return;
    if (t.isSocket) {
        NSURLSessionWebSocketTask *ws = (NSURLSessionWebSocketTask *)task;
        if (t.clientCode) return finish_socket(self.rt, t, t.clientCode, t.clientReason, YES);
        if (ws.closeCode != NSURLSessionWebSocketCloseCodeInvalid) return finish_socket(self.rt, t, ws.closeCode, ws.closeReason, YES);
        if (error) emit_fail(self.rt, t.ident, WS_EV_ERROR, fail_kind(error), error.localizedDescription);
        return finish_socket(self.rt, t, 1006, nil, NO);
    }
    if (t.redirectFailed) emit_fail(self.rt, t.ident, HTTP_EV_ERROR, HTTP_FAIL_REDIRECT, @"unexpected redirect");
    else if (error && [error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled)
        http_emit(self.rt, http_ev_new(t.ident, HTTP_EV_ABORTED));
    else if (error) emit_fail(self.rt, t.ident, HTTP_EV_ERROR, fail_kind(error), error.localizedDescription);
    else http_emit(self.rt, http_ev_new(t.ident, HTTP_EV_END));
    forget(t);
}
- (void)URLSession:(NSURLSession *)s webSocketTask:(NSURLSessionWebSocketTask *)task didOpenWithProtocol:(NSString *)protocol {
    (void)s;
    LWTask *t = byTask[@(task.taskIdentifier)];
    if (!t) return;
    http_ev *ev = http_ev_new(t.ident, WS_EV_OPEN);
    ev->text = dup_string(protocol ?: @"");
    http_emit(self.rt, ev);
}
- (void)URLSession:(NSURLSession *)s webSocketTask:(NSURLSessionWebSocketTask *)task
    didCloseWithCode:(NSURLSessionWebSocketCloseCode)code reason:(NSData *)reason {
    (void)s;
    LWTask *t = byTask[@(task.taskIdentifier)];
    if (t) finish_socket(self.rt, t, t.clientCode ?: code, t.clientCode ? t.clientReason : reason, YES);
}
@end

// Lazily, on the JS thread: one session per process, no cookies or cache
// (as Bun's fetch), and the system's proxies and trust store.
static void ensure_session(jsr_runtime *rt) {
    if (session) return;
    queue = [NSOperationQueue new];
    queue.maxConcurrentOperationCount = 1;
    delegate = [LWDelegate new];
    delegate.rt = rt;
    byTask = [NSMutableDictionary new];
    byId = [NSMutableDictionary new];
    NSURLSessionConfiguration *config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    config.HTTPCookieStorage = nil;
    config.HTTPShouldSetCookies = NO;
    config.URLCache = nil;
    config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    config.timeoutIntervalForRequest = 300; // idle, as Bun's
    config.HTTPMaximumConnectionsPerHost = 256;
    session = [NSURLSession sessionWithConfiguration:config delegate:delegate delegateQueue:queue];
}

static NSMutableURLRequest *make_request(const char *url, char **headers) {
    NSURL *u = [NSURL URLWithString:@(url)];
    if (!u) return nil;
    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:u];
    for (size_t i = 0; headers && headers[i] && headers[i + 1]; i += 2)
        [req addValue:@(headers[i + 1]) forHTTPHeaderField:@(headers[i])];
    return req;
}

void http_backend_request(jsr_runtime *rt, long ident, const char *url, const char *method, char **headers,
                          const uint8_t *body, size_t body_len, int redirect) {
    ensure_session(rt);
    NSMutableURLRequest *req = make_request(url, headers);
    if (!req) return emit_fail(rt, ident, HTTP_EV_ERROR, HTTP_FAIL_URL, @"invalid URL");
    req.HTTPMethod = @(method);
    // CFNetwork gives a POST body without a content type a form one; fetch.js
    // passes a truthful type for those instead.
    if (body) req.HTTPBody = [NSData dataWithBytes:body length:body_len];
    [queue addOperationWithBlock:^{
      LWTask *t = [LWTask new];
      t.ident = ident;
      t.redirect = redirect;
      t.task = [session dataTaskWithRequest:req];
      byTask[@(t.task.taskIdentifier)] = t;
      byId[@(ident)] = t;
      [t.task resume];
    }];
}

void http_backend_cancel(jsr_runtime *rt, long ident) {
    (void)rt;
    if (!queue) return;
    [queue addOperationWithBlock:^{
      [byId[@(ident)].task cancel];
    }];
}

void http_backend_ws_open(jsr_runtime *rt, long ident, const char *url, char **protocols, char **headers) {
    ensure_session(rt);
    NSMutableURLRequest *req = make_request(url, headers);
    if (!req) {
        emit_fail(rt, ident, WS_EV_ERROR, HTTP_FAIL_URL, @"invalid URL");
        http_ev *ev = http_ev_new(ident, WS_EV_CLOSE);
        ev->number = 1006;
        ev->text = strdup("");
        return http_emit(rt, ev);
    }
    NSMutableArray *list = [NSMutableArray new];
    for (size_t i = 0; protocols && protocols[i]; i++) [list addObject:@(protocols[i])];
    if (list.count) [req setValue:[list componentsJoinedByString:@", "] forHTTPHeaderField:@"Sec-WebSocket-Protocol"];
    [queue addOperationWithBlock:^{
      LWTask *t = [LWTask new];
      t.ident = ident;
      t.isSocket = YES;
      NSURLSessionWebSocketTask *task = [session webSocketTaskWithRequest:req];
      task.maximumMessageSize = 128 * 1024 * 1024;
      t.task = task;
      byTask[@(task.taskIdentifier)] = t;
      byId[@(ident)] = t;
      [task resume];
      receive(rt, t);
    }];
}

void http_backend_ws_send(jsr_runtime *rt, long ident, bool binary, const uint8_t *data, size_t len) {
    NSData *bytes = [NSData dataWithBytes:data length:len];
    [queue addOperationWithBlock:^{
      LWTask *t = byId[@(ident)];
      if (!t || t.closed) return;
      NSURLSessionWebSocketMessage *message =
          binary ? [[NSURLSessionWebSocketMessage alloc] initWithData:bytes]
                 : [[NSURLSessionWebSocketMessage alloc] initWithString:[[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding] ?: @""];
      [(NSURLSessionWebSocketTask *)t.task sendMessage:message completionHandler:^(NSError *error) {
        (void)error; // a failed send ends the socket; its close reports it
        http_ev *ev = http_ev_new(ident, WS_EV_SENT);
        ev->number = (double)len;
        http_emit(rt, ev);
      }];
    }];
}

void http_backend_ws_close(jsr_runtime *rt, long ident, int code, const char *reason) {
    (void)rt;
    NSData *why = [NSData dataWithBytes:reason length:strlen(reason)];
    [queue addOperationWithBlock:^{
      LWTask *t = byId[@(ident)];
      if (!t || t.closed || t.clientCode) return;
      // 0: close() without a code, a close frame with no status (1005 here)
      t.clientCode = code ? code : NSURLSessionWebSocketCloseCodeNoStatusReceived;
      t.clientReason = why;
      [(NSURLSessionWebSocketTask *)t.task cancelWithCloseCode:t.clientCode reason:why];
    }];
}
