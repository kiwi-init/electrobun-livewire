// Natives behind the Node shims (node/*.js) on the Livewire JSC runtime:
// synchronous fs calls, os facts, randomness, AES-GCM and fd readiness.
// Installed as globalThis.__node. Errors are thrown as Error objects carrying
// errno/syscall/path; node/errors.js turns them into Node's shape.
#include "runtime.h"
#include "node.h"

#include <CommonCrypto/CommonCryptor.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <dirent.h>
#include <unistd.h>

#define NATIVE(name) static JSValueRef name(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc)
#define UNUSED_ARGS (void)f; (void)self
#define ARG(i) ((size_t)(i) < argc ? argv[i] : JSValueMakeUndefined(ctx))

static void set_prop(JSContextRef ctx, JSObjectRef o, const char *name, JSValueRef v) {
    JSStringRef n = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(ctx, o, n, v, kJSPropertyAttributeNone, NULL);
    JSStringRelease(n);
}
static double num(JSContextRef ctx, JSValueRef v) { return JSValueToNumber(ctx, v, NULL); }

// Throws an Error with errno (negative, like Node), syscall and path.
static JSValueRef throw_errno(JSContextRef ctx, JSValueRef *exc, int err, const char *syscall, const char *path) {
    JSValueRef msg = jsr_string(ctx, strerror(err));
    JSObjectRef e = JSObjectMakeError(ctx, 1, &msg, NULL);
    set_prop(ctx, e, "errno", JSValueMakeNumber(ctx, -err));
    set_prop(ctx, e, "syscall", jsr_string(ctx, syscall));
    if (path) set_prop(ctx, e, "path", jsr_string(ctx, path));
    *exc = e;
    return NULL;
}
static JSValueRef throw_message(JSContextRef ctx, JSValueRef *exc, const char *message) {
    JSValueRef msg = jsr_string(ctx, message);
    *exc = JSObjectMakeError(ctx, 1, &msg, NULL);
    return NULL;
}

static void free_bytes(void *bytes, void *ctx) { (void)ctx; free(bytes); }
// Takes ownership of bytes.
static JSValueRef make_u8(JSContextRef ctx, void *bytes, size_t len) {
    if (!bytes) bytes = malloc(1);
    return JSObjectMakeTypedArrayWithBytesNoCopy(ctx, kJSTypedArrayTypeUint8Array, bytes, len, free_bytes, NULL, NULL);
}
// The bytes of a typed array / ArrayBuffer argument (borrowed).
static uint8_t *bytes_of(JSContextRef ctx, JSValueRef v, size_t *len) {
    *len = 0;
    if (!JSValueIsObject(ctx, v)) return NULL;
    JSObjectRef o = (JSObjectRef)v;
    JSTypedArrayType t = JSValueGetTypedArrayType(ctx, v, NULL);
    if (t == kJSTypedArrayTypeArrayBuffer) {
        *len = JSObjectGetArrayBufferByteLength(ctx, o, NULL);
        return JSObjectGetArrayBufferBytesPtr(ctx, o, NULL);
    }
    if (t == kJSTypedArrayTypeNone) return NULL;
    *len = JSObjectGetTypedArrayByteLength(ctx, o, NULL);
    uint8_t *base = JSObjectGetTypedArrayBytesPtr(ctx, o, NULL);
    return base ? base + JSObjectGetTypedArrayByteOffset(ctx, o, NULL) : NULL;
}

// --- fs ------------------------------------------------------------------------
static JSValueRef stat_object(JSContextRef ctx, const struct stat *st) {
    JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
#define S(name, value) set_prop(ctx, o, name, JSValueMakeNumber(ctx, (double)(value)))
    S("dev", st->st_dev); S("ino", st->st_ino); S("mode", st->st_mode); S("nlink", st->st_nlink);
    S("uid", st->st_uid); S("gid", st->st_gid); S("rdev", st->st_rdev); S("size", st->st_size);
    S("blksize", st->st_blksize); S("blocks", st->st_blocks);
    S("atimeMs", st->st_atimespec.tv_sec * 1e3 + st->st_atimespec.tv_nsec / 1e6);
    S("mtimeMs", st->st_mtimespec.tv_sec * 1e3 + st->st_mtimespec.tv_nsec / 1e6);
    S("ctimeMs", st->st_ctimespec.tv_sec * 1e3 + st->st_ctimespec.tv_nsec / 1e6);
    S("birthtimeMs", st->st_birthtimespec.tv_sec * 1e3 + st->st_birthtimespec.tv_nsec / 1e6);
#undef S
    return o;
}

NATIVE(n_stat) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    bool link = JSValueToBoolean(ctx, ARG(1));
    struct stat st;
    int rc = link ? lstat(path, &st) : stat(path, &st);
    JSValueRef r = rc == 0 ? stat_object(ctx, &st) : throw_errno(ctx, exc, errno, link ? "lstat" : "stat", path);
    free(path);
    return r;
}
NATIVE(n_fstat) {
    UNUSED_ARGS;
    struct stat st;
    if (fstat((int)num(ctx, ARG(0)), &st) != 0) return throw_errno(ctx, exc, errno, "fstat", NULL);
    return stat_object(ctx, &st);
}
NATIVE(n_open) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    int fd = open(path, (int)num(ctx, ARG(1)), (mode_t)(argc > 2 ? num(ctx, ARG(2)) : 0666));
    JSValueRef r = fd >= 0 ? JSValueMakeNumber(ctx, fd) : throw_errno(ctx, exc, errno, "open", path);
    free(path);
    return r;
}
NATIVE(n_close) {
    UNUSED_ARGS;
    if (close((int)num(ctx, ARG(0))) != 0) return throw_errno(ctx, exc, errno, "close", NULL);
    return JSValueMakeUndefined(ctx);
}
NATIVE(n_fsync) {
    UNUSED_ARGS;
    if (fsync((int)num(ctx, ARG(0))) != 0) return throw_errno(ctx, exc, errno, "fsync", NULL);
    return JSValueMakeUndefined(ctx);
}
// read(fd, view, offset, length, position) / write(fd, view, offset, length, position); position < 0: current
NATIVE(n_read) {
    UNUSED_ARGS;
    size_t len;
    uint8_t *buf = bytes_of(ctx, ARG(1), &len);
    size_t offset = (size_t)num(ctx, ARG(2)), want = (size_t)num(ctx, ARG(3));
    double position = num(ctx, ARG(4));
    if (!buf || offset + want > len) return throw_message(ctx, exc, "read: buffer too small");
    ssize_t n = position >= 0 ? pread((int)num(ctx, ARG(0)), buf + offset, want, (off_t)position) : read((int)num(ctx, ARG(0)), buf + offset, want);
    return n >= 0 ? JSValueMakeNumber(ctx, (double)n) : throw_errno(ctx, exc, errno, "read", NULL);
}
NATIVE(n_write) {
    UNUSED_ARGS;
    size_t len;
    uint8_t *buf = bytes_of(ctx, ARG(1), &len);
    size_t offset = (size_t)num(ctx, ARG(2)), want = (size_t)num(ctx, ARG(3));
    double position = num(ctx, ARG(4));
    if (!buf || offset + want > len) return throw_message(ctx, exc, "write: buffer too small");
    ssize_t n = position >= 0 ? pwrite((int)num(ctx, ARG(0)), buf + offset, want, (off_t)position) : write((int)num(ctx, ARG(0)), buf + offset, want);
    return n >= 0 ? JSValueMakeNumber(ctx, (double)n) : throw_errno(ctx, exc, errno, "write", NULL);
}
NATIVE(n_read_file) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { JSValueRef r = throw_errno(ctx, exc, errno, "open", path); free(path); return r; }
    size_t cap = 65536, len = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (len == cap) buf = realloc(buf, cap *= 2);
        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR) continue;
            int err = errno;
            close(fd); free(buf);
            JSValueRef r = throw_errno(ctx, exc, err, "read", path);
            free(path);
            return r;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    free(path);
    return make_u8(ctx, buf, len);
}
// writeFile(path, view, flags, mode)
NATIVE(n_write_file) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    size_t len;
    uint8_t *buf = bytes_of(ctx, ARG(1), &len);
    int fd = open(path, (int)num(ctx, ARG(2)) | O_CLOEXEC, (mode_t)num(ctx, ARG(3)));
    if (fd < 0) { JSValueRef r = throw_errno(ctx, exc, errno, "open", path); free(path); return r; }
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            int err = errno;
            close(fd);
            JSValueRef r = throw_errno(ctx, exc, err, "write", path);
            free(path);
            return r;
        }
        done += (size_t)n;
    }
    close(fd);
    free(path);
    return JSValueMakeUndefined(ctx);
}
static int mkdir_p(char *path, mode_t mode) {
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(path, mode) != 0 && errno != EEXIST) { *p = '/'; return -1; }
        *p = '/';
    }
    if (mkdir(path, mode) != 0) {
        struct stat st;
        if (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
        return -1;
    }
    return 0;
}
// mkdir(path, mode, recursive)
NATIVE(n_mkdir) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    mode_t mode = (mode_t)num(ctx, ARG(1));
    int rc = JSValueToBoolean(ctx, ARG(2)) ? mkdir_p(path, mode) : mkdir(path, mode);
    JSValueRef r = rc == 0 ? JSValueMakeUndefined(ctx) : throw_errno(ctx, exc, errno, "mkdir", path);
    free(path);
    return r;
}
// rm(path, recursive, force)
NATIVE(n_rm) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    bool recursive = JSValueToBoolean(ctx, ARG(1)), force = JSValueToBoolean(ctx, ARG(2));
    struct stat st;
    JSValueRef r = JSValueMakeUndefined(ctx);
    if (lstat(path, &st) != 0) {
        if (!(force && errno == ENOENT)) r = throw_errno(ctx, exc, errno, "rm", path);
    } else if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) != 0) r = throw_errno(ctx, exc, errno, "rm", path);
    } else if (!recursive) {
        r = throw_errno(ctx, exc, EISDIR, "rm", path);
    } else {
        char *paths[] = {path, NULL};
        FTS *fts = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
        FTSENT *e;
        while (fts && (e = fts_read(fts))) {
            int rc = 0;
            if (e->fts_info == FTS_DP) rc = rmdir(e->fts_accpath);
            else if (e->fts_info != FTS_D) rc = unlink(e->fts_accpath);
            if (rc != 0 && errno != ENOENT) { r = throw_errno(ctx, exc, errno, "rm", e->fts_path); break; }
        }
        if (fts) fts_close(fts);
    }
    free(path);
    return r;
}
NATIVE(n_rename) {
    UNUSED_ARGS;
    char *from = jsr_to_utf8(ctx, ARG(0)), *to = jsr_to_utf8(ctx, ARG(1));
    JSValueRef r = rename(from, to) == 0 ? JSValueMakeUndefined(ctx) : throw_errno(ctx, exc, errno, "rename", from);
    free(from); free(to);
    return r;
}
NATIVE(n_unlink) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    JSValueRef r = unlink(path) == 0 ? JSValueMakeUndefined(ctx) : throw_errno(ctx, exc, errno, "unlink", path);
    free(path);
    return r;
}
NATIVE(n_rmdir) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    JSValueRef r = rmdir(path) == 0 ? JSValueMakeUndefined(ctx) : throw_errno(ctx, exc, errno, "rmdir", path);
    free(path);
    return r;
}
NATIVE(n_realpath) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    char out[PATH_MAX];
    JSValueRef r = realpath(path, out) ? jsr_string(ctx, out) : throw_errno(ctx, exc, errno, "realpath", path);
    free(path);
    return r;
}
NATIVE(n_access) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    JSValueRef r = access(path, (int)num(ctx, ARG(1))) == 0 ? JSValueMakeUndefined(ctx) : throw_errno(ctx, exc, errno, "access", path);
    free(path);
    return r;
}
NATIVE(n_chmod) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    JSValueRef r = chmod(path, (mode_t)num(ctx, ARG(1))) == 0 ? JSValueMakeUndefined(ctx) : throw_errno(ctx, exc, errno, "chmod", path);
    free(path);
    return r;
}
NATIVE(n_readdir) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    DIR *d = opendir(path);
    if (!d) { JSValueRef r = throw_errno(ctx, exc, errno, "scandir", path); free(path); return r; }
    JSObjectRef list = JSObjectMakeArray(ctx, 0, NULL, NULL);
    unsigned i = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        JSObjectSetPropertyAtIndex(ctx, list, i++, jsr_string(ctx, e->d_name), NULL);
    }
    closedir(d);
    free(path);
    return list;
}
static JSObjectRef fs_constants(JSContextRef ctx) {
    JSObjectRef c = JSObjectMake(ctx, NULL, NULL);
#define C(name) set_prop(ctx, c, #name, JSValueMakeNumber(ctx, name))
    C(O_RDONLY); C(O_WRONLY); C(O_RDWR); C(O_CREAT); C(O_EXCL); C(O_TRUNC); C(O_APPEND); C(O_NONBLOCK); C(O_CLOEXEC); C(O_NOFOLLOW); C(O_DIRECTORY); C(O_SYNC);
    C(F_OK); C(R_OK); C(W_OK); C(X_OK);
    C(S_IFMT); C(S_IFREG); C(S_IFDIR); C(S_IFLNK); C(S_IFIFO); C(S_IFSOCK); C(S_IFCHR); C(S_IFBLK);
#undef C
    return c;
}

// --- os --------------------------------------------------------------------------
NATIVE(n_os_info) {
    UNUSED_ARGS; (void)argc; (void)argv; (void)exc;
    JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
    const char *home = getenv("HOME");
    struct passwd *pw = getpwuid(getuid());
    if (!home && pw) home = pw->pw_dir;
    set_prop(ctx, o, "homedir", jsr_string(ctx, home ? home : "/"));
    set_prop(ctx, o, "username", jsr_string(ctx, pw ? pw->pw_name : ""));
    char tmp[PATH_MAX];
    const char *t = getenv("TMPDIR");
    if (!t && confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp) > 0) t = tmp;
    char trimmed[PATH_MAX];
    snprintf(trimmed, sizeof trimmed, "%s", t ? t : "/tmp");
    size_t n = strlen(trimmed);
    if (n > 1 && trimmed[n - 1] == '/') trimmed[n - 1] = 0;
    set_prop(ctx, o, "tmpdir", jsr_string(ctx, trimmed));
    char host[256] = "";
    gethostname(host, sizeof host);
    set_prop(ctx, o, "hostname", jsr_string(ctx, host));
    int64_t mem = 0; size_t len = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
    set_prop(ctx, o, "totalmem", JSValueMakeNumber(ctx, (double)mem));
    int cpus = 0; len = sizeof cpus;
    sysctlbyname("hw.ncpu", &cpus, &len, NULL, 0);
    set_prop(ctx, o, "cpus", JSValueMakeNumber(ctx, cpus));
    char release[64] = ""; len = sizeof release;
    sysctlbyname("kern.osrelease", release, &len, NULL, 0);
    set_prop(ctx, o, "release", jsr_string(ctx, release));
    set_prop(ctx, o, "uid", JSValueMakeNumber(ctx, getuid()));
    set_prop(ctx, o, "gid", JSValueMakeNumber(ctx, getgid()));
    return o;
}

// --- crypto ------------------------------------------------------------------------
NATIVE(n_random_fill) {
    UNUSED_ARGS; (void)exc;
    size_t len;
    uint8_t *buf = bytes_of(ctx, ARG(0), &len);
    if (buf) arc4random_buf(buf, len); // a CSPRNG on macOS
    return ARG(0);
}

// AES-GCM from public CommonCrypto: AES-ECB for the block cipher (keystream
// and H), GHASH in C (NIST SP 800-38D).
typedef struct { uint64_t hi, lo; } u128;
static u128 load128(const uint8_t *b) {
    u128 r = {0, 0};
    for (int i = 0; i < 8; i++) { r.hi = (r.hi << 8) | b[i]; r.lo = (r.lo << 8) | b[8 + i]; }
    return r;
}
static void store128(uint8_t *b, u128 v) {
    for (int i = 7; i >= 0; i--) { b[i] = (uint8_t)v.hi; v.hi >>= 8; b[8 + i] = (uint8_t)v.lo; v.lo >>= 8; }
}
static u128 gf_mul(u128 x, u128 y) {
    u128 z = {0, 0}, v = y;
    for (int i = 0; i < 128; i++) {
        uint64_t bit = i < 64 ? (x.hi >> (63 - i)) & 1 : (x.lo >> (127 - i)) & 1;
        if (bit) { z.hi ^= v.hi; z.lo ^= v.lo; }
        uint64_t lsb = v.lo & 1;
        v.lo = (v.lo >> 1) | (v.hi << 63);
        v.hi >>= 1;
        if (lsb) v.hi ^= 0xe100000000000000ULL;
    }
    return z;
}
static void ghash_update(u128 *y, u128 h, const uint8_t *data, size_t len) {
    uint8_t block[16];
    for (size_t off = 0; off < len; off += 16) {
        size_t n = len - off < 16 ? len - off : 16;
        memset(block, 0, 16);
        memcpy(block, data + off, n);
        u128 b = load128(block);
        y->hi ^= b.hi; y->lo ^= b.lo;
        *y = gf_mul(*y, h);
    }
}
static bool aes_ecb(const uint8_t *key, size_t key_len, const uint8_t *in, uint8_t *out, size_t len) {
    size_t moved = 0;
    return CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, key_len, NULL, in, len, out, len, &moved) == kCCSuccess && moved == len;
}
// gcm(encrypt, key, iv, data, aad, tag?) -> { data, tag } (encrypt) or data (decrypt; throws on a bad tag)
NATIVE(n_aes_gcm) {
    UNUSED_ARGS;
    bool encrypt = JSValueToBoolean(ctx, ARG(0));
    size_t key_len, iv_len, len, aad_len, tag_len;
    const uint8_t *key = bytes_of(ctx, ARG(1), &key_len), *iv = bytes_of(ctx, ARG(2), &iv_len);
    const uint8_t *data = bytes_of(ctx, ARG(3), &len), *aad = bytes_of(ctx, ARG(4), &aad_len);
    const uint8_t *want_tag = encrypt ? NULL : bytes_of(ctx, ARG(5), &tag_len);
    if (!key || (key_len != 16 && key_len != 24 && key_len != 32)) return throw_message(ctx, exc, "Invalid key length");
    if (!iv || iv_len == 0) return throw_message(ctx, exc, "Invalid initialization vector");
    if (!encrypt && (!want_tag || tag_len < 4 || tag_len > 16)) return throw_message(ctx, exc, "Invalid authentication tag length");
    uint8_t zero[16] = {0}, hb[16];
    if (!aes_ecb(key, key_len, zero, hb, 16)) return throw_message(ctx, exc, "AES failed");
    u128 h = load128(hb);
    uint8_t j0[16];
    if (iv_len == 12) { memcpy(j0, iv, 12); j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1; }
    else {
        u128 y = {0, 0};
        ghash_update(&y, h, iv, iv_len);
        uint8_t lens[16] = {0};
        uint64_t bits = (uint64_t)iv_len * 8;
        for (int i = 0; i < 8; i++) lens[15 - i] = (uint8_t)(bits >> (8 * i));
        ghash_update(&y, h, lens, 16);
        store128(j0, y);
    }
    // Keystream: E(inc32^k(J0)) for k = 1..blocks, one ECB call.
    size_t blocks = (len + 15) / 16;
    uint8_t *counters = malloc(blocks * 16 + 16), *stream = malloc(blocks * 16 + 16);
    uint32_t c0 = (uint32_t)j0[12] << 24 | (uint32_t)j0[13] << 16 | (uint32_t)j0[14] << 8 | j0[15];
    for (size_t b = 0; b < blocks; b++) {
        memcpy(counters + b * 16, j0, 12);
        uint32_t c = c0 + 1 + (uint32_t)b;
        counters[b * 16 + 12] = (uint8_t)(c >> 24); counters[b * 16 + 13] = (uint8_t)(c >> 16);
        counters[b * 16 + 14] = (uint8_t)(c >> 8); counters[b * 16 + 15] = (uint8_t)c;
    }
    if (blocks && !aes_ecb(key, key_len, counters, stream, blocks * 16)) { free(counters); free(stream); return throw_message(ctx, exc, "AES failed"); }
    uint8_t *out = malloc(len ? len : 1);
    for (size_t i = 0; i < len; i++) out[i] = data[i] ^ stream[i];
    free(counters); free(stream);
    const uint8_t *ciphertext = encrypt ? out : data;
    u128 y = {0, 0};
    ghash_update(&y, h, aad, aad_len);
    ghash_update(&y, h, ciphertext, len);
    uint8_t lens[16];
    uint64_t abits = (uint64_t)aad_len * 8, cbits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) { lens[7 - i] = (uint8_t)(abits >> (8 * i)); lens[15 - i] = (uint8_t)(cbits >> (8 * i)); }
    ghash_update(&y, h, lens, 16);
    uint8_t s[16], ekj0[16], tag[16];
    store128(s, y);
    aes_ecb(key, key_len, j0, ekj0, 16);
    for (int i = 0; i < 16; i++) tag[i] = s[i] ^ ekj0[i];
    if (!encrypt) {
        uint8_t diff = 0;
        for (size_t i = 0; i < tag_len; i++) diff |= tag[i] ^ want_tag[i];
        if (diff) { free(out); return throw_message(ctx, exc, "Unsupported state or unable to authenticate data"); }
        return make_u8(ctx, out, len);
    }
    JSObjectRef result = JSObjectMake(ctx, NULL, NULL);
    set_prop(ctx, result, "data", make_u8(ctx, out, len));
    uint8_t *tag_copy = malloc(16);
    memcpy(tag_copy, tag, 16);
    set_prop(ctx, result, "tag", make_u8(ctx, tag_copy, 16));
    return result;
}

// --- fd readiness (net.Socket({fd})) ---------------------------------------------
// watchFd(fd, fn): fn(Uint8Array) per read, fn(null) at EOF, fn(errno) on error.
// Reads happen on a dispatch queue; delivery is on the JS thread.
typedef struct fd_watch {
    int id, fd;
    JSObjectRef fn;
    dispatch_source_t source;
    bool closed;
    struct fd_watch *next;
} fd_watch;
static fd_watch *g_watches;
static int g_next_watch;

typedef struct { int id; uint8_t *bytes; ssize_t len; int err; } fd_event;
static fd_watch *find_watch(int id) {
    for (fd_watch *w = g_watches; w; w = w->next) if (w->id == id) return w;
    return NULL;
}
static void stop_watch(jsr_runtime *rt, fd_watch *w, bool close_fd) {
    if (w->closed) return;
    w->closed = true;
    dispatch_source_cancel(w->source);
    if (close_fd) close(w->fd);
    jsr_unref(rt);
}
static void deliver_fd_event(jsr_runtime *rt, void *data) {
    fd_event *ev = data;
    fd_watch *w = find_watch(ev->id);
    if (w && !w->closed) {
        JSContextRef ctx = jsr_context(rt);
        JSValueRef arg;
        if (ev->err) arg = JSValueMakeNumber(ctx, -ev->err);
        else if (ev->len == 0) arg = JSValueMakeNull(ctx);
        else { arg = make_u8(ctx, ev->bytes, (size_t)ev->len); ev->bytes = NULL; }
        if (ev->err || ev->len == 0) stop_watch(rt, w, false);
        jsr_call(rt, w->fn, NULL, 1, &arg);
    }
    free(ev->bytes);
    free(ev);
}
NATIVE(n_watch_fd) {
    UNUSED_ARGS;
    int fd = (int)num(ctx, ARG(0));
    if (fd < 0 || !JSValueIsObject(ctx, ARG(1))) return throw_message(ctx, exc, "watchFd(fd, fn)");
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fd_watch *w = calloc(1, sizeof *w);
    w->id = ++g_next_watch;
    w->fd = fd;
    w->fn = (JSObjectRef)ARG(1);
    JSValueProtect(ctx, w->fn);
    w->next = g_watches;
    g_watches = w;
    jsr_runtime *rt = jsr_from_context(ctx);
    jsr_ref(rt);
    int id = w->id;
    w->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, dispatch_get_global_queue(0, 0));
    dispatch_source_set_event_handler(w->source, ^{
        uint8_t *buf = malloc(65536);
        ssize_t n = read(fd, buf, 65536);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) { free(buf); return; }
        fd_event *ev = calloc(1, sizeof *ev);
        ev->id = id;
        ev->len = n;
        ev->err = n < 0 ? errno : 0;
        ev->bytes = n > 0 ? buf : NULL;
        if (n <= 0) { free(buf); dispatch_source_cancel(w->source); } // EOF or error: no more events
        jsr_post(rt, deliver_fd_event, ev);
    });
    dispatch_resume(w->source);
    return JSValueMakeNumber(ctx, id);
}
// unwatchFd(id, closeFd)
NATIVE(n_unwatch_fd) {
    UNUSED_ARGS; (void)exc;
    fd_watch *w = find_watch((int)num(ctx, ARG(0)));
    if (w) stop_watch(jsr_from_context(ctx), w, JSValueToBoolean(ctx, ARG(1)));
    return JSValueMakeUndefined(ctx);
}
// refFd(id, bool)
NATIVE(n_ref_fd) {
    UNUSED_ARGS; (void)exc;
    fd_watch *w = find_watch((int)num(ctx, ARG(0)));
    if (w && !w->closed) {
        if (JSValueToBoolean(ctx, ARG(1))) jsr_ref(jsr_from_context(ctx)); else jsr_unref(jsr_from_context(ctx));
    }
    return JSValueMakeUndefined(ctx);
}

void node_install(jsr_runtime *rt) {
    JSContextRef ctx = jsr_context(rt);
    JSObjectRef node = JSObjectMake(ctx, NULL, NULL);
    JSObjectRef fs = JSObjectMake(ctx, NULL, NULL);
    jsr_set_function(ctx, fs, "stat", n_stat);
    jsr_set_function(ctx, fs, "fstat", n_fstat);
    jsr_set_function(ctx, fs, "open", n_open);
    jsr_set_function(ctx, fs, "close", n_close);
    jsr_set_function(ctx, fs, "fsync", n_fsync);
    jsr_set_function(ctx, fs, "read", n_read);
    jsr_set_function(ctx, fs, "write", n_write);
    jsr_set_function(ctx, fs, "readFile", n_read_file);
    jsr_set_function(ctx, fs, "writeFile", n_write_file);
    jsr_set_function(ctx, fs, "mkdir", n_mkdir);
    jsr_set_function(ctx, fs, "rm", n_rm);
    jsr_set_function(ctx, fs, "rename", n_rename);
    jsr_set_function(ctx, fs, "unlink", n_unlink);
    jsr_set_function(ctx, fs, "rmdir", n_rmdir);
    jsr_set_function(ctx, fs, "realpath", n_realpath);
    jsr_set_function(ctx, fs, "access", n_access);
    jsr_set_function(ctx, fs, "chmod", n_chmod);
    jsr_set_function(ctx, fs, "readdir", n_readdir);
    set_prop(ctx, fs, "constants", fs_constants(ctx));
    set_prop(ctx, node, "fs", fs);
    jsr_set_function(ctx, node, "osInfo", n_os_info);
    jsr_set_function(ctx, node, "randomFill", n_random_fill);
    jsr_set_function(ctx, node, "aesGcm", n_aes_gcm);
    jsr_set_function(ctx, node, "watchFd", n_watch_fd);
    jsr_set_function(ctx, node, "unwatchFd", n_unwatch_fd);
    jsr_set_function(ctx, node, "refFd", n_ref_fd);
    set_prop(ctx, JSContextGetGlobalObject(ctx), "__node", node);
}
