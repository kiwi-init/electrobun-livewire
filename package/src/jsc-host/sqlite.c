// bun:sqlite's natives on the system libsqlite3 (macOS ships it; on Linux
// it's libsqlite3). node/sqlite.js builds Database and Statement on these, as
// globalThis.__sqlite. Databases and statements are small integer handles.
#include "node.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static void free_with_ctx(void *bytes, void *ctx) { (void)ctx; free(bytes); }

#define NATIVE(name) static JSValueRef name(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc)
#define UNUSED_ARGS (void)f; (void)self
#define ARG(i) ((size_t)(i) < argc ? argv[i] : JSValueMakeUndefined(ctx))

// Handles: index into a growing table; 0 is never used. Each runtime (a
// Worker's too) only sees the handles it made, so no lock beyond the table's.
typedef struct { void *p; bool is_stmt; } handle;
static handle *g_handles;
static size_t g_handle_count, g_handle_cap;
static pthread_mutex_t g_handles_lock = PTHREAD_MUTEX_INITIALIZER;

static double handle_new(void *p, bool is_stmt) {
    pthread_mutex_lock(&g_handles_lock);
    size_t i = 1;
    while (i < g_handle_count && g_handles[i].p) i++;
    if (i >= g_handle_count) {
        if (g_handle_count + 1 > g_handle_cap) {
            g_handle_cap = g_handle_cap ? g_handle_cap * 2 : 64;
            g_handles = realloc(g_handles, g_handle_cap * sizeof *g_handles);
        }
        if (g_handle_count == 0) { g_handles[0].p = NULL; g_handle_count = 1; i = 1; }
        i = g_handle_count++;
    }
    g_handles[i].p = p;
    g_handles[i].is_stmt = is_stmt;
    pthread_mutex_unlock(&g_handles_lock);
    return (double)i;
}
static void *handle_get(JSContextRef ctx, JSValueRef v, bool is_stmt) {
    double d = JSValueToNumber(ctx, v, NULL);
    pthread_mutex_lock(&g_handles_lock);
    void *p = d >= 1 && (size_t)d < g_handle_count && g_handles[(size_t)d].is_stmt == is_stmt ? g_handles[(size_t)d].p : NULL;
    pthread_mutex_unlock(&g_handles_lock);
    return p;
}
static void handle_free(JSContextRef ctx, JSValueRef v) {
    double d = JSValueToNumber(ctx, v, NULL);
    pthread_mutex_lock(&g_handles_lock);
    if (d >= 1 && (size_t)d < g_handle_count) g_handles[(size_t)d].p = NULL;
    pthread_mutex_unlock(&g_handles_lock);
}

static void set_prop(JSContextRef ctx, JSObjectRef o, const char *name, JSValueRef v) {
    JSStringRef n = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(ctx, o, n, v, kJSPropertyAttributeNone, NULL);
    JSStringRelease(n);
}
static JSValueRef get_prop(JSContextRef ctx, JSObjectRef o, const char *name) {
    JSStringRef n = JSStringCreateWithUTF8CString(name);
    JSValueRef v = JSObjectGetProperty(ctx, o, n, NULL);
    JSStringRelease(n);
    return v;
}

// Bun names the extended code when there is one (SQLITE_CONSTRAINT_UNIQUE).
static const struct { int code; const char *name; } extended_names[] = {
    {257, "SQLITE_ERROR_MISSING_COLLSEQ"}, {513, "SQLITE_ERROR_RETRY"}, {769, "SQLITE_ERROR_SNAPSHOT"},
    {261, "SQLITE_BUSY_RECOVERY"}, {517, "SQLITE_BUSY_SNAPSHOT"}, {773, "SQLITE_BUSY_TIMEOUT"},
    {262, "SQLITE_LOCKED_SHAREDCACHE"}, {518, "SQLITE_LOCKED_VTAB"},
    {264, "SQLITE_READONLY_RECOVERY"}, {520, "SQLITE_READONLY_CANTLOCK"}, {776, "SQLITE_READONLY_ROLLBACK"},
    {1032, "SQLITE_READONLY_DBMOVED"}, {1288, "SQLITE_READONLY_CANTINIT"}, {1544, "SQLITE_READONLY_DIRECTORY"},
    {270, "SQLITE_CANTOPEN_NOTEMPDIR"}, {526, "SQLITE_CANTOPEN_ISDIR"}, {782, "SQLITE_CANTOPEN_FULLPATH"},
    {1038, "SQLITE_CANTOPEN_CONVPATH"}, {267, "SQLITE_CORRUPT_VTAB"}, {523, "SQLITE_CORRUPT_SEQUENCE"},
    {779, "SQLITE_CORRUPT_INDEX"}, {516, "SQLITE_ABORT_ROLLBACK"},
    {275, "SQLITE_CONSTRAINT_CHECK"}, {531, "SQLITE_CONSTRAINT_COMMITHOOK"}, {787, "SQLITE_CONSTRAINT_FOREIGNKEY"},
    {1043, "SQLITE_CONSTRAINT_FUNCTION"}, {1299, "SQLITE_CONSTRAINT_NOTNULL"}, {1555, "SQLITE_CONSTRAINT_PRIMARYKEY"},
    {1811, "SQLITE_CONSTRAINT_TRIGGER"}, {2067, "SQLITE_CONSTRAINT_UNIQUE"}, {2323, "SQLITE_CONSTRAINT_VTAB"},
    {2579, "SQLITE_CONSTRAINT_ROWID"}, {2835, "SQLITE_CONSTRAINT_PINNED"}, {3091, "SQLITE_CONSTRAINT_DATATYPE"},
};

static const char *code_name(int code) {
    for (size_t i = 0; i < sizeof extended_names / sizeof extended_names[0]; i++)
        if (extended_names[i].code == code) return extended_names[i].name;
    switch (code & 0xff) {
    case SQLITE_ERROR: return "SQLITE_ERROR";
    case SQLITE_INTERNAL: return "SQLITE_INTERNAL";
    case SQLITE_PERM: return "SQLITE_PERM";
    case SQLITE_ABORT: return "SQLITE_ABORT";
    case SQLITE_BUSY: return "SQLITE_BUSY";
    case SQLITE_LOCKED: return "SQLITE_LOCKED";
    case SQLITE_NOMEM: return "SQLITE_NOMEM";
    case SQLITE_READONLY: return "SQLITE_READONLY";
    case SQLITE_INTERRUPT: return "SQLITE_INTERRUPT";
    case SQLITE_IOERR: return "SQLITE_IOERR";
    case SQLITE_CORRUPT: return "SQLITE_CORRUPT";
    case SQLITE_NOTFOUND: return "SQLITE_NOTFOUND";
    case SQLITE_FULL: return "SQLITE_FULL";
    case SQLITE_CANTOPEN: return "SQLITE_CANTOPEN";
    case SQLITE_PROTOCOL: return "SQLITE_PROTOCOL";
    case SQLITE_SCHEMA: return "SQLITE_SCHEMA";
    case SQLITE_TOOBIG: return "SQLITE_TOOBIG";
    case SQLITE_CONSTRAINT: return "SQLITE_CONSTRAINT";
    case SQLITE_MISMATCH: return "SQLITE_MISMATCH";
    case SQLITE_MISUSE: return "SQLITE_MISUSE";
    case SQLITE_RANGE: return "SQLITE_RANGE";
    case SQLITE_NOTADB: return "SQLITE_NOTADB";
    default: return "SQLITE_ERROR";
    }
}

// A SQLiteError: message from SQLite, code (SQLITE_CONSTRAINT...) and errno
// (the extended result code), as Bun throws.
static JSValueRef throw_sqlite(JSContextRef ctx, JSValueRef *exc, int code, const char *message) {
    JSValueRef args[1] = {jsr_string(ctx, message ? message : sqlite3_errstr(code))};
    JSObjectRef error = JSObjectMakeError(ctx, 1, args, NULL);
    set_prop(ctx, error, "name", jsr_string(ctx, "SQLiteError"));
    set_prop(ctx, error, "code", jsr_string(ctx, code_name(code)));
    set_prop(ctx, error, "errno", JSValueMakeNumber(ctx, code));
    *exc = error;
    return NULL;
}
static JSValueRef throw_db(JSContextRef ctx, JSValueRef *exc, sqlite3 *db, int code) {
    return throw_sqlite(ctx, exc, db ? sqlite3_extended_errcode(db) : code, db ? sqlite3_errmsg(db) : NULL);
}
static JSValueRef throw_plain(JSContextRef ctx, JSValueRef *exc, const char *message) {
    JSValueRef args[1] = {jsr_string(ctx, message)};
    *exc = JSObjectMakeError(ctx, 1, args, NULL);
    return NULL;
}

// open(path, flags) -> db handle. flags: SQLITE_OPEN_* bits.
NATIVE(s_open) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    int flags = (int)JSValueToNumber(ctx, ARG(1), NULL);
    if (!flags) flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, flags | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_URI, NULL);
    free(path);
    if (rc != SQLITE_OK) {
        JSValueRef r = throw_db(ctx, exc, db, rc);
        sqlite3_close_v2(db);
        return r;
    }
    sqlite3_extended_result_codes(db, 1);
    return JSValueMakeNumber(ctx, handle_new(db, false));
}

NATIVE(s_close) {
    UNUSED_ARGS;
    sqlite3 *db = handle_get(ctx, ARG(0), false);
    if (!db) return JSValueMakeUndefined(ctx);
    // throwOnError: with statements still open, close fails (else it waits for them).
    if (JSValueToBoolean(ctx, ARG(1))) {
        int rc = sqlite3_close(db);
        if (rc != SQLITE_OK) return throw_db(ctx, exc, db, rc);
    } else sqlite3_close_v2(db);
    handle_free(ctx, ARG(0));
    return JSValueMakeUndefined(ctx);
}

// prepare(db, sql) -> {handle, columns, params, tail} (tail: the SQL after the
// first statement, for a multi-statement run), or null for SQL with no statement.
NATIVE(s_prepare) {
    UNUSED_ARGS;
    sqlite3 *db = handle_get(ctx, ARG(0), false);
    if (!db) return throw_plain(ctx, exc, "Cannot use a closed database");
    char *sql = jsr_to_utf8(ctx, ARG(1));
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int rc = sqlite3_prepare_v3(db, sql, -1, JSValueToBoolean(ctx, ARG(2)) ? SQLITE_PREPARE_PERSISTENT : 0, &stmt, &tail);
    if (rc != SQLITE_OK) { free(sql); return throw_db(ctx, exc, db, rc); }
    JSObjectRef out = JSObjectMake(ctx, NULL, NULL);
    while (tail && (*tail == ' ' || *tail == '\n' || *tail == '\t' || *tail == '\r' || *tail == ';')) tail++;
    set_prop(ctx, out, "tail", jsr_string(ctx, tail ? tail : ""));
    free(sql);
    if (!stmt) { set_prop(ctx, out, "handle", JSValueMakeNumber(ctx, 0)); return out; }
    int columns = sqlite3_column_count(stmt);
    JSValueRef *names = calloc((size_t)columns + 1, sizeof *names);
    for (int i = 0; i < columns; i++) names[i] = jsr_string(ctx, sqlite3_column_name(stmt, i));
    set_prop(ctx, out, "columns", JSObjectMakeArray(ctx, (size_t)columns, names, NULL));
    free(names);
    int params = sqlite3_bind_parameter_count(stmt);
    JSValueRef *pnames = calloc((size_t)params + 1, sizeof *pnames);
    for (int i = 0; i < params; i++) {
        const char *n = sqlite3_bind_parameter_name(stmt, i + 1);
        pnames[i] = n ? jsr_string(ctx, n) : JSValueMakeNull(ctx);
    }
    set_prop(ctx, out, "params", JSObjectMakeArray(ctx, (size_t)params, pnames, NULL));
    free(pnames);
    set_prop(ctx, out, "handle", JSValueMakeNumber(ctx, handle_new(stmt, true)));
    return out;
}

static bool bind_value(JSContextRef ctx, sqlite3_stmt *stmt, int i, JSValueRef v, JSValueRef *exc) {
    int rc;
    switch (JSValueGetType(ctx, v)) {
    case kJSTypeUndefined:
    case kJSTypeNull:
        rc = sqlite3_bind_null(stmt, i);
        break;
    case kJSTypeBoolean:
        rc = sqlite3_bind_int(stmt, i, JSValueToBoolean(ctx, v));
        break;
    case kJSTypeNumber: {
        double d = JSValueToNumber(ctx, v, NULL);
        if (d == (double)(int64_t)d && d >= -9007199254740992.0 && d <= 9007199254740992.0) rc = sqlite3_bind_int64(stmt, i, (int64_t)d);
        else rc = sqlite3_bind_double(stmt, i, d);
        break;
    }
    case kJSTypeString: {
        char *s = jsr_to_utf8(ctx, v);
        rc = sqlite3_bind_text(stmt, i, s, -1, free);
        break;
    }
    default: {
        JSTypedArrayType t = JSValueGetTypedArrayType(ctx, v, NULL);
        if (t != kJSTypedArrayTypeNone) {
            size_t len = 0;
            const void *p;
            if (t == kJSTypedArrayTypeArrayBuffer) {
                len = JSObjectGetArrayBufferByteLength(ctx, (JSObjectRef)v, NULL);
                p = JSObjectGetArrayBufferBytesPtr(ctx, (JSObjectRef)v, NULL);
            } else {
                len = JSObjectGetTypedArrayByteLength(ctx, (JSObjectRef)v, NULL);
                p = (const uint8_t *)JSObjectGetTypedArrayBytesPtr(ctx, (JSObjectRef)v, NULL) + JSObjectGetTypedArrayByteOffset(ctx, (JSObjectRef)v, NULL);
            }
            rc = sqlite3_bind_blob64(stmt, i, len ? p : "", len, SQLITE_TRANSIENT);
            break;
        }
        // A bigint (node/sqlite.js passes it as {bigint: "123"}).
        if (JSValueIsObject(ctx, v)) {
            JSValueRef big = get_prop(ctx, (JSObjectRef)v, "bigint");
            if (JSValueIsString(ctx, big)) {
                char *s = jsr_to_utf8(ctx, big);
                rc = sqlite3_bind_int64(stmt, i, strtoll(s, NULL, 10));
                free(s);
                break;
            }
        }
        throw_plain(ctx, exc, "Binding expected string, TypedArray, boolean, number, bigint or null");
        return false;
    }
    }
    if (rc != SQLITE_OK) { throw_db(ctx, exc, sqlite3_db_handle(stmt), rc); return false; }
    return true;
}

// Binds an array of values (one per parameter, in order); node/sqlite.js
// turns named parameters into that order.
static bool bind_all(JSContextRef ctx, sqlite3_stmt *stmt, JSValueRef params, JSValueRef *exc) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (!JSValueIsObject(ctx, params)) return true;
    JSObjectRef list = (JSObjectRef)params;
    int n = (int)JSValueToNumber(ctx, get_prop(ctx, list, "length"), NULL);
    int count = sqlite3_bind_parameter_count(stmt);
    if (n > count) {
        char message[128];
        snprintf(message, sizeof message, "SQLite query expected %d values, received %d", count, n);
        throw_plain(ctx, exc, message);
        return false;
    }
    for (int i = 0; i < n; i++)
        if (!bind_value(ctx, stmt, i + 1, JSObjectGetPropertyAtIndex(ctx, list, (unsigned)i, NULL), exc)) return false;
    return true;
}

static JSValueRef column_value(JSContextRef ctx, sqlite3_stmt *stmt, int i, bool safe_integers) {
    switch (sqlite3_column_type(stmt, i)) {
    case SQLITE_INTEGER: {
        sqlite3_int64 v = sqlite3_column_int64(stmt, i);
        if (safe_integers) {
            char buf[32];
            snprintf(buf, sizeof buf, "%lld", (long long)v);
            JSObjectRef box = JSObjectMake(ctx, NULL, NULL);
            set_prop(ctx, box, "bigint", jsr_string(ctx, buf));
            return box;
        }
        return JSValueMakeNumber(ctx, (double)v);
    }
    case SQLITE_FLOAT:
        return JSValueMakeNumber(ctx, sqlite3_column_double(stmt, i));
    case SQLITE_TEXT: {
        const unsigned char *s = sqlite3_column_text(stmt, i);
        return jsr_string(ctx, (const char *)s);
    }
    case SQLITE_BLOB: {
        int len = sqlite3_column_bytes(stmt, i);
        void *copy = malloc(len > 0 ? (size_t)len : 1);
        if (len > 0) memcpy(copy, sqlite3_column_blob(stmt, i), (size_t)len);
        return JSObjectMakeTypedArrayWithBytesNoCopy(ctx, kJSTypedArrayTypeUint8Array, copy, (size_t)len, free_with_ctx, NULL, NULL);
    }
    default:
        return JSValueMakeNull(ctx);
    }
}

// step(stmt, params, mode, safeIntegers): mode 0 run -> {changes, lastInsertRowid};
// 1 get -> first row as an object (null if none); 2 all -> rows as objects;
// 3 values -> rows as arrays.
NATIVE(s_step) {
    UNUSED_ARGS;
    sqlite3_stmt *stmt = handle_get(ctx, ARG(0), true);
    if (!stmt) return throw_plain(ctx, exc, "Statement has been finalized");
    int mode = (int)JSValueToNumber(ctx, ARG(2), NULL);
    bool safe = JSValueToBoolean(ctx, ARG(3));
    if (!bind_all(ctx, stmt, ARG(1), exc)) return NULL;
    sqlite3 *db = sqlite3_db_handle(stmt);
    int columns = sqlite3_column_count(stmt);
    JSStringRef *names = calloc((size_t)columns + 1, sizeof *names);
    for (int i = 0; i < columns; i++) names[i] = JSStringCreateWithUTF8CString(sqlite3_column_name(stmt, i));
    size_t cap = 16, count = 0;
    JSValueRef *rows = mode >= 2 ? malloc(cap * sizeof *rows) : NULL;
    JSValueRef first = JSValueMakeNull(ctx);
    JSValueRef result = NULL;
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) { throw_db(ctx, exc, db, rc); goto done; }
        if (mode == 0) continue;
        JSValueRef row;
        if (mode == 3) {
            JSValueRef *cells = calloc((size_t)columns + 1, sizeof *cells);
            for (int i = 0; i < columns; i++) cells[i] = column_value(ctx, stmt, i, safe);
            row = JSObjectMakeArray(ctx, (size_t)columns, cells, NULL);
            free(cells);
        } else {
            JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
            for (int i = 0; i < columns; i++) JSObjectSetProperty(ctx, o, names[i], column_value(ctx, stmt, i, safe), kJSPropertyAttributeNone, NULL);
            row = o;
        }
        if (mode == 1) { first = row; break; }
        if (count == cap) { cap *= 2; rows = realloc(rows, cap * sizeof *rows); }
        rows[count++] = row;
    }
    if (mode == 0) {
        JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
        set_prop(ctx, o, "changes", JSValueMakeNumber(ctx, (double)sqlite3_changes64(db)));
        set_prop(ctx, o, "lastInsertRowid", JSValueMakeNumber(ctx, (double)sqlite3_last_insert_rowid(db)));
        result = o;
    } else if (mode == 1) result = first;
    else result = JSObjectMakeArray(ctx, count, rows, NULL);
done:
    sqlite3_reset(stmt);
    for (int i = 0; i < columns; i++) JSStringRelease(names[i]);
    free(names);
    free(rows);
    return result;
}

NATIVE(s_finalize) {
    UNUSED_ARGS; (void)exc;
    sqlite3_stmt *stmt = handle_get(ctx, ARG(0), true);
    if (stmt) { sqlite3_finalize(stmt); handle_free(ctx, ARG(0)); }
    return JSValueMakeUndefined(ctx);
}

// The statement's SQL with its current bindings filled in.
NATIVE(s_expanded) {
    UNUSED_ARGS; (void)exc;
    sqlite3_stmt *stmt = handle_get(ctx, ARG(0), true);
    char *s = stmt ? sqlite3_expanded_sql(stmt) : NULL;
    JSValueRef v = jsr_string(ctx, s ? s : "");
    sqlite3_free(s);
    return v;
}

NATIVE(s_in_transaction) {
    UNUSED_ARGS; (void)exc;
    sqlite3 *db = handle_get(ctx, ARG(0), false);
    return JSValueMakeBoolean(ctx, db && !sqlite3_get_autocommit(db));
}

// serialize(db) -> Uint8Array of the main database.
NATIVE(s_serialize) {
    UNUSED_ARGS;
    sqlite3 *db = handle_get(ctx, ARG(0), false);
    if (!db) return throw_plain(ctx, exc, "Cannot use a closed database");
    sqlite3_int64 size = 0;
    unsigned char *bytes = sqlite3_serialize(db, "main", &size, 0);
    if (!bytes) return throw_db(ctx, exc, db, SQLITE_NOMEM);
    void *copy = malloc(size > 0 ? (size_t)size : 1);
    if (size > 0) memcpy(copy, bytes, (size_t)size);
    sqlite3_free(bytes);
    return JSObjectMakeTypedArrayWithBytesNoCopy(ctx, kJSTypedArrayTypeUint8Array, copy, (size_t)size, free_with_ctx, NULL, NULL);
}

// deserialize(bytes, readonly) -> db handle: an in-memory database holding a copy.
NATIVE(s_deserialize) {
    UNUSED_ARGS;
    JSValueRef v = ARG(0);
    JSTypedArrayType t = JSValueGetTypedArrayType(ctx, v, NULL);
    if (t == kJSTypedArrayTypeNone) return throw_plain(ctx, exc, "Expected a Uint8Array or ArrayBuffer");
    size_t len;
    const uint8_t *p;
    if (t == kJSTypedArrayTypeArrayBuffer) {
        len = JSObjectGetArrayBufferByteLength(ctx, (JSObjectRef)v, NULL);
        p = JSObjectGetArrayBufferBytesPtr(ctx, (JSObjectRef)v, NULL);
    } else {
        len = JSObjectGetTypedArrayByteLength(ctx, (JSObjectRef)v, NULL);
        p = (const uint8_t *)JSObjectGetTypedArrayBytesPtr(ctx, (JSObjectRef)v, NULL) + JSObjectGetTypedArrayByteOffset(ctx, (JSObjectRef)v, NULL);
    }
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) { JSValueRef r = throw_db(ctx, exc, db, rc); sqlite3_close_v2(db); return r; }
    unsigned char *copy = sqlite3_malloc64(len ? len : 1);
    if (len) memcpy(copy, p, len);
    bool readonly = JSValueToBoolean(ctx, ARG(1));
    rc = sqlite3_deserialize(db, "main", copy, (sqlite3_int64)len, (sqlite3_int64)len,
                             SQLITE_DESERIALIZE_FREEONCLOSE | (readonly ? SQLITE_DESERIALIZE_READONLY : SQLITE_DESERIALIZE_RESIZEABLE));
    if (rc != SQLITE_OK) { JSValueRef r = throw_db(ctx, exc, db, rc); sqlite3_close_v2(db); return r; }
    sqlite3_extended_result_codes(db, 1);
    return JSValueMakeNumber(ctx, handle_new(db, false));
}

void sqlite_install(jsr_runtime *rt) {
    JSContextRef ctx = jsr_context(rt);
    JSObjectRef s = JSObjectMake(ctx, NULL, NULL);
    jsr_set_function(ctx, s, "open", s_open);
    jsr_set_function(ctx, s, "close", s_close);
    jsr_set_function(ctx, s, "prepare", s_prepare);
    jsr_set_function(ctx, s, "step", s_step);
    jsr_set_function(ctx, s, "finalize", s_finalize);
    jsr_set_function(ctx, s, "expanded", s_expanded);
    jsr_set_function(ctx, s, "inTransaction", s_in_transaction);
    jsr_set_function(ctx, s, "serialize", s_serialize);
    jsr_set_function(ctx, s, "deserialize", s_deserialize);
    set_prop(ctx, s, "version", jsr_string(ctx, sqlite3_libversion()));
    set_prop(ctx, JSContextGetGlobalObject(ctx), "__sqlite", s);
}
