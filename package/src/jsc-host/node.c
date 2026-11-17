// Natives behind the Node shims (node/*.js) on the Livewire JSC runtime:
// synchronous fs calls, os facts, randomness, AES-GCM, fd readiness, child
// processes, sockets, zlib and the OS secret store. Installed as
// globalThis.__node. Errors are thrown as Error objects carrying
// errno/syscall/path; node/errors.js turns them into Node's shape.
#ifndef __APPLE__
#define _GNU_SOURCE // pipe2, posix_spawn's SETSID, addchdir_np and addclosefrom_np
#endif
#include "runtime.h"
#include "node.h"

#ifdef __APPLE__
#include <CommonCrypto/CommonCryptor.h>
#include <Security/Security.h>
#include <dispatch/dispatch.h>
#include <sys/sysctl.h>
#else
#include <gcrypt.h>
#include <glib.h>
#include <glib-unix.h>
#include <sys/random.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <zlib.h>

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
#ifdef __APPLE__
    S("atimeMs", st->st_atimespec.tv_sec * 1e3 + st->st_atimespec.tv_nsec / 1e6);
    S("mtimeMs", st->st_mtimespec.tv_sec * 1e3 + st->st_mtimespec.tv_nsec / 1e6);
    S("ctimeMs", st->st_ctimespec.tv_sec * 1e3 + st->st_ctimespec.tv_nsec / 1e6);
    S("birthtimeMs", st->st_birthtimespec.tv_sec * 1e3 + st->st_birthtimespec.tv_nsec / 1e6);
#else
    S("atimeMs", st->st_atim.tv_sec * 1e3 + st->st_atim.tv_nsec / 1e6);
    S("mtimeMs", st->st_mtim.tv_sec * 1e3 + st->st_mtim.tv_nsec / 1e6);
    S("ctimeMs", st->st_ctim.tv_sec * 1e3 + st->st_ctim.tv_nsec / 1e6);
    S("birthtimeMs", 0); // struct stat has no birth time on Linux (only statx)
#endif
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
            JSValueRef r = throw_errno(ctx, exc, err, "read", NULL);
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
NATIVE(n_symlink) {
    UNUSED_ARGS;
    char *target = jsr_to_utf8(ctx, ARG(0)), *path = jsr_to_utf8(ctx, ARG(1));
    JSValueRef r = symlink(target, path) == 0 ? JSValueMakeUndefined(ctx) : throw_errno(ctx, exc, errno, "symlink", target);
    free(target); free(path);
    return r;
}
NATIVE(n_link) {
    UNUSED_ARGS;
    char *from = jsr_to_utf8(ctx, ARG(0)), *to = jsr_to_utf8(ctx, ARG(1));
    JSValueRef r = link(from, to) == 0 ? JSValueMakeUndefined(ctx) : throw_errno(ctx, exc, errno, "link", from);
    free(from); free(to);
    return r;
}
NATIVE(n_readlink) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    char out[PATH_MAX];
    ssize_t n = readlink(path, out, sizeof out - 1);
    JSValueRef r;
    if (n >= 0) { out[n] = 0; r = jsr_string(ctx, out); }
    else r = throw_errno(ctx, exc, errno, "readlink", path);
    free(path);
    return r;
}
NATIVE(n_utimes) {
    UNUSED_ARGS;
    char *path = jsr_to_utf8(ctx, ARG(0));
    double at = num(ctx, ARG(1)), mt = num(ctx, ARG(2));
    time_t as = (time_t)at, ms = (time_t)mt;
    if ((double)as > at) as--;
    if ((double)ms > mt) ms--;
    struct timespec ts[2] = {{as, (long)((at - (double)as) * 1e9)}, {ms, (long)((mt - (double)ms) * 1e9)}};
    JSValueRef r = utimensat(AT_FDCWD, path, ts, 0) == 0 ? JSValueMakeUndefined(ctx) : throw_errno(ctx, exc, errno, "utime", path);
    free(path);
    return r;
}
static JSObjectRef errno_names(JSContextRef ctx) {
    JSObjectRef c = JSObjectMake(ctx, NULL, NULL);
#define E(name) set_prop(ctx, c, #name, JSValueMakeNumber(ctx, name))
    E(EPERM); E(ENOENT); E(ESRCH); E(EINTR); E(EIO); E(ENXIO); E(E2BIG); E(ENOEXEC); E(EBADF); E(ECHILD); E(EAGAIN); E(ENOMEM);
    E(EACCES); E(EFAULT); E(EBUSY); E(EEXIST); E(EXDEV); E(ENODEV); E(ENOTDIR); E(EISDIR); E(EINVAL); E(ENFILE); E(EMFILE);
    E(ENOTTY); E(EFBIG); E(ENOSPC); E(ESPIPE); E(EROFS); E(EMLINK); E(EPIPE); E(ELOOP); E(ENAMETOOLONG); E(ENOTEMPTY);
    E(ENOSYS); E(ETIMEDOUT); E(ECONNREFUSED); E(ECONNRESET); E(EADDRINUSE); E(ENOTSUP); E(ENOTCONN); E(ECANCELED); E(ENOBUFS);
#undef E
    return c;
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
#ifdef __APPLE__
    if (!t && confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp) > 0) t = tmp;
#else
    (void)tmp;
#endif
    char trimmed[PATH_MAX];
    snprintf(trimmed, sizeof trimmed, "%s", t ? t : "/tmp");
    size_t n = strlen(trimmed);
    if (n > 1 && trimmed[n - 1] == '/') trimmed[n - 1] = 0;
    set_prop(ctx, o, "tmpdir", jsr_string(ctx, trimmed));
    char host[256] = "";
    gethostname(host, sizeof host);
    set_prop(ctx, o, "hostname", jsr_string(ctx, host));
#ifdef __APPLE__
    int64_t mem = 0; size_t len = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
    int cpus = 0; len = sizeof cpus;
    sysctlbyname("hw.ncpu", &cpus, &len, NULL, 0);
    char release[64] = ""; len = sizeof release;
    sysctlbyname("kern.osrelease", release, &len, NULL, 0);
#else
    struct sysinfo si;
    double mem = sysinfo(&si) == 0 ? (double)si.totalram * si.mem_unit : 0;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    struct utsname un;
    char release[sizeof un.release] = "";
    if (uname(&un) == 0) snprintf(release, sizeof release, "%s", un.release);
#endif
    set_prop(ctx, o, "totalmem", JSValueMakeNumber(ctx, (double)mem));
    set_prop(ctx, o, "cpus", JSValueMakeNumber(ctx, (double)cpus));
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
#ifdef __APPLE__
    if (buf) arc4random_buf(buf, len); // a CSPRNG on macOS
#else
    // getrandom(2): the kernel CSPRNG; it returns at most 32 MB per call.
    for (size_t done = 0; buf && done < len;) {
        ssize_t n = getrandom(buf + done, len - done, 0);
        if (n < 0) { if (errno == EINTR) continue; return throw_message(ctx, exc, "getrandom failed"); }
        done += (size_t)n;
    }
#endif
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
#ifdef __APPLE__
    size_t moved = 0;
    return CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, key_len, NULL, in, len, out, len, &moved) == kCCSuccess && moved == len;
#else
    // libgcrypt: WebKitGTK already depends on it, so it costs the app nothing.
    int algo = key_len == 16 ? GCRY_CIPHER_AES128 : key_len == 24 ? GCRY_CIPHER_AES192 : key_len == 32 ? GCRY_CIPHER_AES256 : 0;
    if (!algo) return false;
    static bool initialized;
    if (!initialized) { gcry_check_version(NULL); gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0); initialized = true; }
    gcry_cipher_hd_t hd;
    if (gcry_cipher_open(&hd, algo, GCRY_CIPHER_MODE_ECB, 0)) return false;
    bool ok = !gcry_cipher_setkey(hd, key, key_len) && !gcry_cipher_encrypt(hd, out, len, in, len);
    gcry_cipher_close(hd);
    return ok;
#endif
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
#ifdef __APPLE__
    dispatch_source_t source;
#else
    GSource *source; // on the JS thread's context: reads happen on the JS thread
#endif
    bool closed;
    struct fd_watch *next;
} fd_watch;
// The fd, child and writable lists are shared by every runtime's JS thread
// (a Worker's too), so their links change under g_lists_lock; each entry is
// used only by the runtime that made it.
static pthread_mutex_t g_lists_lock = PTHREAD_MUTEX_INITIALIZER;
static fd_watch *g_watches;
static int g_next_watch;

typedef struct { int id; uint8_t *bytes; ssize_t len; int err; } fd_event;
static fd_watch *find_watch(int id) {
    pthread_mutex_lock(&g_lists_lock);
    fd_watch *w = g_watches;
    while (w && w->id != id) w = w->next;
    pthread_mutex_unlock(&g_lists_lock);
    return w;
}
static void stop_watch(jsr_runtime *rt, fd_watch *w, bool close_fd) {
    // Stopped already (at EOF): only the fd may be left to close.
    if (w->closed) { if (close_fd && w->fd >= 0) { close(w->fd); w->fd = -1; } return; }
    w->closed = true;
#ifdef __APPLE__
    dispatch_source_cancel(w->source);
#else
    if (w->source) { g_source_destroy(w->source); g_source_unref(w->source); w->source = NULL; }
#endif
    if (close_fd) { close(w->fd); w->fd = -1; }
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
#ifndef __APPLE__
// Linux: the fd source runs on the JS thread, so it reads and delivers in place.
static gboolean fd_readable(gint fd, GIOCondition cond, gpointer data) {
    (void)cond;
    fd_watch *w = data;
    jsr_runtime *rt = jsr_from_context(NULL);
    uint8_t *buf = malloc(65536);
    ssize_t n = read(fd, buf, 65536);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) { free(buf); return G_SOURCE_CONTINUE; }
    fd_event *ev = calloc(1, sizeof *ev);
    ev->id = w->id;
    ev->len = n;
    ev->err = n < 0 ? errno : 0;
    ev->bytes = n > 0 ? buf : NULL;
    if (n <= 0) free(buf);
    deliver_fd_event(rt, ev); // may stop_watch(), which destroys this source
    return G_SOURCE_CONTINUE;
}
#endif
NATIVE(n_watch_fd) {
    UNUSED_ARGS;
    int fd = (int)num(ctx, ARG(0));
    if (fd < 0 || !JSValueIsObject(ctx, ARG(1))) return throw_message(ctx, exc, "watchFd(fd, fn)");
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fd_watch *w = calloc(1, sizeof *w);
    w->fd = fd;
    w->fn = (JSObjectRef)ARG(1);
    JSValueProtect(ctx, w->fn);
    pthread_mutex_lock(&g_lists_lock);
    w->id = ++g_next_watch;
    w->next = g_watches;
    g_watches = w;
    pthread_mutex_unlock(&g_lists_lock);
    jsr_runtime *rt = jsr_from_context(ctx);
    jsr_ref(rt);
    int id = w->id;
#ifdef __APPLE__
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
#else
    (void)id;
    w->source = g_unix_fd_source_new(fd, G_IO_IN | G_IO_HUP | G_IO_ERR);
    g_source_set_callback(w->source, (GSourceFunc)(void (*)(void))fd_readable, w, NULL);
    g_source_attach(w->source, (GMainContext *)jsr_glib_context(rt));
#endif
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


// --- child processes (child_process, Bun.spawn) -----------------------------
// posix_spawn with stdio as pipes, /dev/null, the parent's fds or given fds.
// Async children report their exit on the JS thread: a dispatch proc source
// (macOS) or a GLib child watch on the JS thread's context (Linux). Sync ones
// poll their pipes on the calling thread.
#ifdef __APPLE__
extern char **environ;
#endif
#define SIGNAL_LIST(X) X(SIGHUP) X(SIGINT) X(SIGQUIT) X(SIGILL) X(SIGTRAP) X(SIGABRT) X(SIGBUS) X(SIGFPE) \
    X(SIGKILL) X(SIGUSR1) X(SIGSEGV) X(SIGUSR2) X(SIGPIPE) X(SIGALRM) X(SIGTERM) X(SIGCHLD) X(SIGCONT) \
    X(SIGSTOP) X(SIGTSTP) X(SIGTTIN) X(SIGTTOU) X(SIGURG) X(SIGXCPU) X(SIGXFSZ) X(SIGVTALRM) X(SIGPROF) \
    X(SIGWINCH) X(SIGIO) X(SIGSYS)
static JSObjectRef signal_names(JSContextRef ctx) {
    JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
#define X(s) set_prop(ctx, o, #s, JSValueMakeNumber(ctx, s));
    SIGNAL_LIST(X)
#undef X
    return o;
}

// An array of strings (or null) as a NULL-terminated char *[]; NULL for null.
static char **string_list(JSContextRef ctx, JSValueRef v) {
    if (!JSValueIsObject(ctx, v)) return NULL;
    JSObjectRef a = (JSObjectRef)v;
    JSStringRef len_name = JSStringCreateWithUTF8CString("length");
    size_t n = (size_t)num(ctx, JSObjectGetProperty(ctx, a, len_name, NULL));
    JSStringRelease(len_name);
    char **list = calloc(n + 1, sizeof *list);
    for (size_t i = 0; i < n; i++) list[i] = jsr_to_utf8(ctx, JSObjectGetPropertyAtIndex(ctx, a, (unsigned)i, NULL));
    return list;
}
static void free_list(char **list) {
    if (!list) return;
    for (char **p = list; *p; p++) free(*p);
    free(list);
}
static char *optional_string(JSContextRef ctx, JSValueRef v) {
    return JSValueIsString(ctx, v) ? jsr_to_utf8(ctx, v) : NULL;
}

// The program to run: a name with a slash as given, else the first
// executable regular file of that name on search_path (the child's PATH).
static char *resolve_program(const char *file, const char *search_path) {
    if (strchr(file, '/')) return strdup(file);
    if (!search_path) search_path = "/usr/bin:/bin";
    const char *p = search_path;
    while (true) {
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *candidate = malloc(len + strlen(file) + 3);
        if (len == 0) sprintf(candidate, "./%s", file);
        else sprintf(candidate, "%.*s/%s", (int)len, p, file);
        struct stat st;
        if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode) && access(candidate, X_OK) == 0) return candidate;
        free(candidate);
        if (!end) return NULL;
        p = end + 1;
    }
}

typedef struct { int kind, fd; } stdio_spec; // kind: 0 inherit, 1 pipe, 2 ignore, 3 the fd
static stdio_spec stdio_of(JSContextRef ctx, JSValueRef list, unsigned i) {
    JSValueRef v = JSValueIsObject(ctx, list) ? JSObjectGetPropertyAtIndex(ctx, (JSObjectRef)list, i, NULL) : JSValueMakeUndefined(ctx);
    stdio_spec s = {1, -1};
    if (JSValueIsNumber(ctx, v)) {
        s.fd = (int)num(ctx, v);
        s.kind = s.fd == (int)i ? 0 : 3;
    } else if (JSValueIsString(ctx, v)) {
        char *k = jsr_to_utf8(ctx, v);
        s.kind = !strcmp(k, "inherit") ? 0 : !strcmp(k, "ignore") ? 2 : 1;
        free(k);
    }
    return s;
}
static int cloexec_pipe(int fds[2]) {
#ifdef __APPLE__
    if (pipe(fds) != 0) return errno;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
#else
    return pipe2(fds, O_CLOEXEC) == 0 ? 0 : errno;
#endif
}

// Starts the child. parent_fds[i] is the parent's end of stdio i's pipe, or
// -1. Returns 0 or an errno (ENOENT for a program not found).
static int start_child(const char *file, char **args, char **env, const char *cwd, const char *search_path,
                       const stdio_spec stdio[3], bool detached, pid_t *pid, int parent_fds[3]) {
    int pipes[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
    for (int i = 0; i < 3; i++) parent_fds[i] = -1;
    char *program = resolve_program(file, search_path);
    if (!program) return ENOENT;
    int err = 0;
    for (int i = 0; i < 3 && !err; i++)
        if (stdio[i].kind == 1) err = cloexec_pipe(pipes[i]);
    if (err) {
        for (int i = 0; i < 3; i++) { if (pipes[i][0] >= 0) close(pipes[i][0]); if (pipes[i][1] >= 0) close(pipes[i][1]); }
        free(program);
        return err;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
    for (int i = 0; i < 3; i++) {
        switch (stdio[i].kind) {
        case 1: posix_spawn_file_actions_adddup2(&actions, i == 0 ? pipes[i][0] : pipes[i][1], i); break;
        case 2: posix_spawn_file_actions_addopen(&actions, i, "/dev/null", i == 0 ? O_RDONLY : O_WRONLY, 0); break;
        case 3: posix_spawn_file_actions_adddup2(&actions, stdio[i].fd, i); break;
        default:
#ifdef __APPLE__
            posix_spawn_file_actions_addinherit_np(&actions, i);
#endif
            break;
        }
    }
    if (cwd) {
#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        // The _np name works back to macOS 10.15; its POSIX name needs macOS 26.
        posix_spawn_file_actions_addchdir_np(&actions, cwd);
#pragma clang diagnostic pop
#else
        posix_spawn_file_actions_addchdir_np(&actions, cwd);
#endif
    }
#if !defined(__APPLE__) && defined(__GLIBC__) && (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 34)
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
    // Default dispositions and an empty mask: the runtime ignores SIGPIPE and
    // the signals the app listens to, and children must not inherit that.
    sigset_t all, none;
    sigfillset(&all);
    sigemptyset(&none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setsigmask(&attr, &none);
    short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT; // only the fds set up above reach the child
#endif
    if (detached) flags |= POSIX_SPAWN_SETSID;
    posix_spawnattr_setflags(&attr, flags);
    err = posix_spawn(pid, program, &actions, &attr, args, env ? env : environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    free(program);
    for (int i = 0; i < 3; i++) {
        if (pipes[i][0] < 0) continue;
        int child_end = i == 0 ? pipes[i][0] : pipes[i][1], parent_end = i == 0 ? pipes[i][1] : pipes[i][0];
        close(child_end);
        if (err) close(parent_end);
        else {
            fcntl(parent_end, F_SETFL, fcntl(parent_end, F_GETFL) | O_NONBLOCK);
            parent_fds[i] = parent_end;
        }
    }
    return err;
}

// Exit watches for async children.
typedef struct child_watch {
    pid_t pid;
    JSObjectRef fn;
    bool reffed;
    struct child_watch *next;
} child_watch;
static child_watch *g_children;
typedef struct { pid_t pid; int status; } child_exit;
static void deliver_child_exit(jsr_runtime *rt, void *data) {
    child_exit *ev = data;
    child_watch **link = &g_children, *w = NULL;
    pthread_mutex_lock(&g_lists_lock);
    while (*link && (*link)->pid != ev->pid) link = &(*link)->next;
    if ((w = *link)) *link = w->next;
    pthread_mutex_unlock(&g_lists_lock);
    if (w) {
        if (w->reffed) jsr_unref(rt);
        JSContextRef ctx = jsr_context(rt);
        JSValueRef args[2];
        if (WIFSIGNALED(ev->status)) {
            args[0] = JSValueMakeNull(ctx);
            args[1] = JSValueMakeNumber(ctx, WTERMSIG(ev->status));
        } else {
            args[0] = JSValueMakeNumber(ctx, WEXITSTATUS(ev->status));
            args[1] = JSValueMakeNumber(ctx, 0);
        }
        JSObjectRef fn = w->fn;
        free(w);
        jsr_call(rt, fn, NULL, 2, args);
        JSValueUnprotect(ctx, fn);
    }
    free(ev);
}
#ifdef __APPLE__
static dispatch_queue_t g_child_queue;
// On g_child_queue: reaps pid if it has exited, once.
static bool reap_child(jsr_runtime *rt, pid_t pid, dispatch_source_t source) {
    int status;
    pid_t r;
    do r = waitpid(pid, &status, WNOHANG); while (r < 0 && errno == EINTR);
    if (r != pid) return false;
    dispatch_source_cancel(source);
    child_exit *ev = malloc(sizeof *ev);
    ev->pid = pid;
    ev->status = status;
    jsr_post(rt, deliver_child_exit, ev);
    return true;
}
#else
static void child_exited(GPid pid, gint status, gpointer data) {
    (void)data;
    child_exit *ev = malloc(sizeof *ev);
    ev->pid = pid;
    ev->status = status;
    deliver_child_exit(jsr_from_context(NULL), ev);
}
#endif
static void watch_child(jsr_runtime *rt, JSContextRef ctx, pid_t pid, JSObjectRef fn) {
    child_watch *w = calloc(1, sizeof *w);
    w->pid = pid;
    w->fn = fn;
    w->reffed = true;
    JSValueProtect(ctx, fn);
    pthread_mutex_lock(&g_lists_lock);
    w->next = g_children;
    g_children = w;
#ifdef __APPLE__
    if (!g_child_queue) g_child_queue = dispatch_queue_create("livewire.children", DISPATCH_QUEUE_SERIAL);
#endif
    pthread_mutex_unlock(&g_lists_lock);
    jsr_ref(rt);
#ifdef __APPLE__
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid, DISPATCH_PROC_EXIT, g_child_queue);
    dispatch_source_set_event_handler(source, ^{ reap_child(rt, pid, source); });
    dispatch_source_set_cancel_handler(source, ^{ dispatch_release(source); });
    dispatch_resume(source);
    // A child that exited before the source was armed: reap it now.
    dispatch_retain(source);
    dispatch_async(g_child_queue, ^{
        if (!dispatch_source_testcancel(source)) reap_child(rt, pid, source);
        dispatch_release(source);
    });
#else
    GSource *source = g_child_watch_source_new(pid);
    g_source_set_callback(source, (GSourceFunc)(void (*)(void))child_exited, NULL, NULL);
    g_source_attach(source, (GMainContext *)jsr_glib_context(rt));
    g_source_unref(source);
#endif
}

// spawn(file, args, env | null, cwd | null, stdio[3], detached, searchPath | null, onExit(code | null, signal))
// -> [pid, stdinFd, stdoutFd, stderrFd] (-1 where not a pipe), or a negative errno.
NATIVE(n_spawn) {
    UNUSED_ARGS;
    if (!JSValueIsObject(ctx, ARG(7))) return throw_message(ctx, exc, "spawn: onExit must be a function");
    char *file = jsr_to_utf8(ctx, ARG(0)), *cwd = optional_string(ctx, ARG(3)), *search = optional_string(ctx, ARG(6));
    char **args = string_list(ctx, ARG(1)), **env = string_list(ctx, ARG(2));
    stdio_spec stdio[3] = {stdio_of(ctx, ARG(4), 0), stdio_of(ctx, ARG(4), 1), stdio_of(ctx, ARG(4), 2)};
    pid_t pid = 0;
    int fds[3];
    int err = start_child(file, args, env, cwd, search, stdio, JSValueToBoolean(ctx, ARG(5)), &pid, fds);
    free(file); free(cwd); free(search); free_list(args); free_list(env);
    if (err) return JSValueMakeNumber(ctx, -err);
    watch_child(jsr_from_context(ctx), ctx, pid, (JSObjectRef)ARG(7));
    JSValueRef items[4] = {JSValueMakeNumber(ctx, pid), JSValueMakeNumber(ctx, fds[0]), JSValueMakeNumber(ctx, fds[1]), JSValueMakeNumber(ctx, fds[2])};
    return JSObjectMakeArray(ctx, 4, items, NULL);
}
// childRef(pid, bool): whether a running child keeps the loop alive.
NATIVE(n_child_ref) {
    UNUSED_ARGS; (void)exc;
    pid_t pid = (pid_t)num(ctx, ARG(0));
    bool ref = JSValueToBoolean(ctx, ARG(1));
    pthread_mutex_lock(&g_lists_lock);
    for (child_watch *w = g_children; w; w = w->next) {
        if (w->pid != pid || w->reffed == ref) continue;
        w->reffed = ref;
        if (ref) jsr_ref(jsr_from_context(ctx)); else jsr_unref(jsr_from_context(ctx));
    }
    pthread_mutex_unlock(&g_lists_lock);
    return JSValueMakeUndefined(ctx);
}
// kill(pid, signal) -> 0 or a negative errno.
NATIVE(n_kill) {
    UNUSED_ARGS; (void)exc;
    return JSValueMakeNumber(ctx, kill((pid_t)num(ctx, ARG(0)), (int)num(ctx, ARG(1))) == 0 ? 0 : -errno);
}

static double monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
typedef struct { uint8_t *bytes; size_t len, cap; } byte_buf;
static void buf_add(byte_buf *b, const uint8_t *data, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2;
        b->bytes = realloc(b->bytes, b->cap);
    }
    memcpy(b->bytes + b->len, data, n);
    b->len += n;
}
// spawnSync(file, args, env, cwd, stdio, detached, searchPath, input | null, timeoutMs, killSignal, maxBuffer)
// -> { pid, status, signal, stdout, stderr, error (negative errno or 0), timedOut, overflow }
NATIVE(n_spawn_sync) {
    UNUSED_ARGS; (void)exc;
    char *file = jsr_to_utf8(ctx, ARG(0)), *cwd = optional_string(ctx, ARG(3)), *search = optional_string(ctx, ARG(6));
    char **args = string_list(ctx, ARG(1)), **env = string_list(ctx, ARG(2));
    stdio_spec stdio[3] = {stdio_of(ctx, ARG(4), 0), stdio_of(ctx, ARG(4), 1), stdio_of(ctx, ARG(4), 2)};
    size_t input_len = 0;
    uint8_t *input = bytes_of(ctx, ARG(7), &input_len);
    double timeout = num(ctx, ARG(8)), max_buffer = num(ctx, ARG(10));
    int kill_signal = (int)num(ctx, ARG(9));
    pid_t pid = 0;
    int fds[3];
    int err = start_child(file, args, env, cwd, search, stdio, JSValueToBoolean(ctx, ARG(5)), &pid, fds);
    free(file); free(cwd); free(search); free_list(args); free_list(env);
    JSObjectRef result = JSObjectMake(ctx, NULL, NULL);
    if (err) {
        set_prop(ctx, result, "error", JSValueMakeNumber(ctx, -err));
        return result;
    }
    byte_buf out[3] = {{0}};
    size_t written = 0;
    bool timed_out = false, overflow = false;
    double deadline = timeout > 0 ? monotonic_ms() + timeout : 0;
    if (fds[0] >= 0 && !input) { close(fds[0]); fds[0] = -1; }
    while (fds[0] >= 0 || fds[1] >= 0 || fds[2] >= 0) {
        struct pollfd p[3];
        int which[3], n = 0;
        for (int i = 0; i < 3; i++) {
            if (fds[i] < 0) continue;
            p[n].fd = fds[i];
            p[n].events = i == 0 ? POLLOUT : POLLIN;
            p[n].revents = 0;
            which[n++] = i;
        }
        int wait = -1;
        if (deadline) {
            double left = deadline - monotonic_ms();
            wait = left > 0 ? (int)left + 1 : 0;
        }
        int ready = poll(p, (nfds_t)n, wait);
        if (ready < 0 && errno == EINTR) continue;
        if (ready == 0 && deadline && monotonic_ms() >= deadline) { timed_out = true; break; }
        for (int k = 0; k < n; k++) {
            int i = which[k];
            if (!p[k].revents) continue;
            if (i == 0) {
                ssize_t w = write(fds[0], input + written, input_len - written);
                if (w > 0) written += (size_t)w;
                if ((w < 0 && errno != EAGAIN && errno != EINTR) || written >= input_len) { close(fds[0]); fds[0] = -1; }
                continue;
            }
            uint8_t chunk[65536];
            ssize_t r = read(fds[i], chunk, sizeof chunk);
            if (r > 0) {
                buf_add(&out[i], chunk, (size_t)r);
                if (max_buffer > 0 && (double)out[i].len > max_buffer) overflow = true;
            } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                close(fds[i]);
                fds[i] = -1;
            }
        }
        if (overflow) break;
    }
    // A timeout or too much output kills the child and stops reading, as Node does.
    if (timed_out || overflow) kill(pid, kill_signal);
    for (int i = 0; i < 3; i++) if (fds[i] >= 0) close(fds[i]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    set_prop(ctx, result, "pid", JSValueMakeNumber(ctx, pid));
    if (WIFSIGNALED(status)) {
        set_prop(ctx, result, "status", JSValueMakeNull(ctx));
        set_prop(ctx, result, "signal", JSValueMakeNumber(ctx, WTERMSIG(status)));
    } else {
        set_prop(ctx, result, "status", JSValueMakeNumber(ctx, WEXITSTATUS(status)));
        set_prop(ctx, result, "signal", JSValueMakeNumber(ctx, 0));
    }
    set_prop(ctx, result, "stdout", stdio[1].kind == 1 ? make_u8(ctx, out[1].bytes, out[1].len) : JSValueMakeNull(ctx));
    set_prop(ctx, result, "stderr", stdio[2].kind == 1 ? make_u8(ctx, out[2].bytes, out[2].len) : JSValueMakeNull(ctx));
    set_prop(ctx, result, "error", JSValueMakeNumber(ctx, 0));
    set_prop(ctx, result, "timedOut", JSValueMakeBoolean(ctx, timed_out));
    set_prop(ctx, result, "overflow", JSValueMakeBoolean(ctx, overflow));
    return result;
}

// writeFd(fd, view, offset) -> bytes written, or a negative errno (-EAGAIN: full).
NATIVE(n_write_fd) {
    UNUSED_ARGS; (void)exc;
    size_t len;
    uint8_t *bytes = bytes_of(ctx, ARG(1), &len);
    size_t offset = (size_t)num(ctx, ARG(2));
    if (!bytes || offset > len) return JSValueMakeNumber(ctx, -EINVAL);
    ssize_t n;
    do n = write((int)num(ctx, ARG(0)), bytes + offset, len - offset); while (n < 0 && errno == EINTR);
    return JSValueMakeNumber(ctx, n >= 0 ? (double)n : -errno);
}
// whenWritable(fd, fn) -> id: fn() once on the JS thread when fd can take
// more (or has failed); keeps the loop alive until then or cancelWritable(id).
typedef struct writable_wait {
    int id;
    JSObjectRef fn;
    bool reffed;
#ifdef __APPLE__
    dispatch_source_t source;
#else
    GSource *source;
#endif
    struct writable_wait *next;
} writable_wait;
static writable_wait *g_writable_waits;
static int g_next_writable;
// Unlinks the wait with this id (NULL if it already fired or was cancelled).
static writable_wait *take_writable(jsr_runtime *rt, int id) {
    pthread_mutex_lock(&g_lists_lock);
    writable_wait **link = &g_writable_waits;
    while (*link && (*link)->id != id) link = &(*link)->next;
    writable_wait *w = *link;
    if (w) *link = w->next;
    pthread_mutex_unlock(&g_lists_lock);
    if (!w) return NULL;
    if (w->reffed) jsr_unref(rt);
#ifdef __APPLE__
    dispatch_source_cancel(w->source);
    dispatch_release(w->source); // the wait's own reference
#else
    g_source_destroy(w->source);
    g_source_unref(w->source);
#endif
    return w;
}
static void deliver_writable(jsr_runtime *rt, void *data) {
    writable_wait *w = take_writable(rt, (int)(intptr_t)data);
    if (!w) return;
    jsr_call(rt, w->fn, NULL, 0, NULL);
    JSValueUnprotect(jsr_context(rt), w->fn);
    free(w);
}
#ifndef __APPLE__
static gboolean fd_writable(gint fd, GIOCondition cond, gpointer data) {
    (void)fd; (void)cond;
    deliver_writable(jsr_from_context(NULL), data);
    return G_SOURCE_REMOVE;
}
#endif
static JSValueRef when_ready(JSContextRef ctx, size_t argc, const JSValueRef argv[], JSValueRef *exc, bool readable) {
    int fd = (int)num(ctx, ARG(0));
    if (fd < 0 || !JSValueIsObject(ctx, ARG(1))) return throw_message(ctx, exc, "whenWritable/whenReadable(fd, fn)");
    writable_wait *w = calloc(1, sizeof *w);
    w->fn = (JSObjectRef)ARG(1);
    w->reffed = true;
    JSValueProtect(ctx, w->fn);
    pthread_mutex_lock(&g_lists_lock);
    w->id = ++g_next_writable;
    w->next = g_writable_waits;
    g_writable_waits = w;
    pthread_mutex_unlock(&g_lists_lock);
    jsr_runtime *rt = jsr_from_context(ctx);
    jsr_ref(rt);
    void *id = (void *)(intptr_t)w->id;
#ifdef __APPLE__
    dispatch_source_t source = dispatch_source_create(readable ? DISPATCH_SOURCE_TYPE_READ : DISPATCH_SOURCE_TYPE_WRITE, (uintptr_t)fd, 0, dispatch_get_global_queue(0, 0));
    w->source = source;
    dispatch_retain(source); // held by the wait; the creation reference goes in the cancel handler
    dispatch_source_set_event_handler(source, ^{
        dispatch_source_cancel(source);
        jsr_post(rt, deliver_writable, id);
    });
    dispatch_source_set_cancel_handler(source, ^{ dispatch_release(source); });
    dispatch_resume(source);
#else
    w->source = g_unix_fd_source_new(fd, (readable ? G_IO_IN : G_IO_OUT) | G_IO_ERR | G_IO_HUP);
    g_source_set_callback(w->source, (GSourceFunc)(void (*)(void))fd_writable, id, NULL);
    g_source_attach(w->source, (GMainContext *)jsr_glib_context(rt));
#endif
    return JSValueMakeNumber(ctx, w->id);
}
NATIVE(n_when_writable) {
    UNUSED_ARGS;
    return when_ready(ctx, argc, argv, exc, false);
}
// whenReadable(fd, fn) -> id: as whenWritable, for a listening socket's next
// connection (the read watch, watchFd, reads the bytes itself).
NATIVE(n_when_readable) {
    UNUSED_ARGS;
    return when_ready(ctx, argc, argv, exc, true);
}
// refWait(id, bool): whether a pending wait keeps the loop alive.
NATIVE(n_ref_wait) {
    UNUSED_ARGS; (void)exc;
    int id = (int)num(ctx, ARG(0));
    bool ref = JSValueToBoolean(ctx, ARG(1));
    for (writable_wait *w = g_writable_waits; w; w = w->next) {
        if (w->id != id || w->reffed == ref) continue;
        w->reffed = ref;
        if (ref) jsr_ref(jsr_from_context(ctx)); else jsr_unref(jsr_from_context(ctx));
    }
    return JSValueMakeUndefined(ctx);
}
// cancelWritable(id): drops a pending whenWritable (before its fd closes).
NATIVE(n_cancel_writable) {
    UNUSED_ARGS; (void)exc;
    writable_wait *w = take_writable(jsr_from_context(ctx), (int)num(ctx, ARG(0)));
    if (w) { JSValueUnprotect(ctx, w->fn); free(w); }
    return JSValueMakeUndefined(ctx);
}
// which(file, searchPath | null) -> the path spawn would run, or null.
NATIVE(n_which) {
    UNUSED_ARGS; (void)exc;
    char *file = jsr_to_utf8(ctx, ARG(0)), *search = optional_string(ctx, ARG(1));
    char *found = strchr(file, '/') ? (access(file, X_OK) == 0 ? strdup(file) : NULL) : resolve_program(file, search);
    JSValueRef r = found ? jsr_string(ctx, found) : JSValueMakeNull(ctx);
    free(file); free(search); free(found);
    return r;
}


// --- sockets (net, Bun.listen/connect) -------------------------------------------
// Nonblocking, close-on-exec unix and TCP sockets. Connects complete through
// whenWritable + sockError, accepts through whenReadable; reads use watchFd and
// writes writeFd, as for pipes. Hosts are IP literals; lookup() resolves names
// on a thread.
static int new_socket(int domain) {
#ifdef __APPLE__
    int fd = socket(domain, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    int fd = socket(domain, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
#endif
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}
// An address from (path) or (host literal, port); 0 or a negative errno.
static int make_addr(JSContextRef ctx, JSValueRef host_or_path, JSValueRef port, struct sockaddr_storage *ss, socklen_t *len) {
    memset(ss, 0, sizeof *ss);
    char *s = jsr_to_utf8(ctx, host_or_path);
    int err = 0;
    if (!JSValueIsNumber(ctx, port)) {
        struct sockaddr_un *un = (struct sockaddr_un *)ss;
        if (strlen(s) >= sizeof un->sun_path) err = -ENAMETOOLONG;
        else {
            un->sun_family = AF_UNIX;
            strcpy(un->sun_path, s);
            *len = (socklen_t)sizeof *un;
        }
    } else {
        struct sockaddr_in *in4 = (struct sockaddr_in *)ss;
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ss;
        uint16_t p = htons((uint16_t)num(ctx, port));
        if (inet_pton(AF_INET, s, &in4->sin_addr) == 1) {
            in4->sin_family = AF_INET;
            in4->sin_port = p;
            *len = (socklen_t)sizeof *in4;
        } else if (inet_pton(AF_INET6, s, &in6->sin6_addr) == 1) {
            in6->sin6_family = AF_INET6;
            in6->sin6_port = p;
            *len = (socklen_t)sizeof *in6;
        } else err = -EINVAL;
    }
    free(s);
    return err;
}
// connect(pathOrHost, port | undefined) -> [fd, pending] or a negative errno.
NATIVE(n_sock_connect) {
    UNUSED_ARGS; (void)exc;
    struct sockaddr_storage ss;
    socklen_t len = 0;
    int err = make_addr(ctx, ARG(0), ARG(1), &ss, &len);
    if (err) return JSValueMakeNumber(ctx, err);
    int fd = new_socket(ss.ss_family);
    if (fd < 0) return JSValueMakeNumber(ctx, fd);
    int rc;
    do rc = connect(fd, (struct sockaddr *)&ss, len); while (rc < 0 && errno == EINTR);
    if (rc < 0 && errno != EINPROGRESS) {
        int e = errno;
        close(fd);
        return JSValueMakeNumber(ctx, -e);
    }
    JSValueRef items[2] = {JSValueMakeNumber(ctx, fd), JSValueMakeBoolean(ctx, rc < 0)};
    return JSObjectMakeArray(ctx, 2, items, NULL);
}
// sockError(fd) -> 0 or the pending error (negative errno): a connect's result.
NATIVE(n_sock_error) {
    UNUSED_ARGS; (void)exc;
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt((int)num(ctx, ARG(0)), SOL_SOCKET, SO_ERROR, &err, &len) < 0) err = errno;
    return JSValueMakeNumber(ctx, -err);
}
// listen(pathOrHost, port | undefined, backlog) -> fd or a negative errno.
NATIVE(n_sock_listen) {
    UNUSED_ARGS; (void)exc;
    struct sockaddr_storage ss;
    socklen_t len = 0;
    int err = make_addr(ctx, ARG(0), ARG(1), &ss, &len);
    if (err) return JSValueMakeNumber(ctx, err);
    int fd = new_socket(ss.ss_family);
    if (fd < 0) return JSValueMakeNumber(ctx, fd);
    int one = 1, zero = 0;
    if (ss.ss_family != AF_UNIX) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (ss.ss_family == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero); // "::" takes IPv4 too
    int backlog = JSValueIsNumber(ctx, ARG(2)) ? (int)num(ctx, ARG(2)) : 511;
    if (bind(fd, (struct sockaddr *)&ss, len) < 0 || listen(fd, backlog) < 0) {
        int e = errno;
        close(fd);
        return JSValueMakeNumber(ctx, -e);
    }
    return JSValueMakeNumber(ctx, fd);
}
// accept(fd) -> a connected nonblocking fd, or a negative errno (-EAGAIN: none waiting).
NATIVE(n_sock_accept) {
    UNUSED_ARGS; (void)exc;
    int fd;
    do {
#ifdef __APPLE__
        fd = accept((int)num(ctx, ARG(0)), NULL, NULL);
#else
        fd = accept4((int)num(ctx, ARG(0)), NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
#endif
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return JSValueMakeNumber(ctx, -errno);
#ifdef __APPLE__
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return JSValueMakeNumber(ctx, fd);
}
// sockName(fd, peer) -> { address, family, port } / { path } / null.
NATIVE(n_sock_name) {
    UNUSED_ARGS; (void)exc;
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    int fd = (int)num(ctx, ARG(0));
    int rc = JSValueToBoolean(ctx, ARG(1)) ? getpeername(fd, (struct sockaddr *)&ss, &len) : getsockname(fd, (struct sockaddr *)&ss, &len);
    if (rc < 0) return JSValueMakeNull(ctx);
    JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
    char text[INET6_ADDRSTRLEN] = "";
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *in4 = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &in4->sin_addr, text, sizeof text);
        set_prop(ctx, o, "address", jsr_string(ctx, text));
        set_prop(ctx, o, "family", jsr_string(ctx, "IPv4"));
        set_prop(ctx, o, "port", JSValueMakeNumber(ctx, ntohs(in4->sin_port)));
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&ss;
        inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof text);
        set_prop(ctx, o, "address", jsr_string(ctx, text));
        set_prop(ctx, o, "family", jsr_string(ctx, "IPv6"));
        set_prop(ctx, o, "port", JSValueMakeNumber(ctx, ntohs(in6->sin6_port)));
    } else if (ss.ss_family == AF_UNIX) {
        struct sockaddr_un *un = (struct sockaddr_un *)&ss;
        size_t n = len > offsetof(struct sockaddr_un, sun_path) ? strnlen(un->sun_path, len - offsetof(struct sockaddr_un, sun_path)) : 0;
        char *path = strndup(un->sun_path, n);
        set_prop(ctx, o, "path", jsr_string(ctx, path));
        free(path);
    }
    return o;
}
// shutdown(fd): no more writes (the peer reads EOF).
NATIVE(n_sock_shutdown) {
    UNUSED_ARGS; (void)exc;
    return JSValueMakeNumber(ctx, shutdown((int)num(ctx, ARG(0)), SHUT_WR) == 0 ? 0 : -errno);
}
// sockOption(fd, "nodelay" | "keepalive", on, delaySeconds)
NATIVE(n_sock_option) {
    UNUSED_ARGS; (void)exc;
    int fd = (int)num(ctx, ARG(0));
    char *what = jsr_to_utf8(ctx, ARG(1));
    int on = JSValueToBoolean(ctx, ARG(2)) ? 1 : 0;
    if (!strcmp(what, "nodelay")) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    else if (!strcmp(what, "keepalive")) {
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
        int delay = (int)num(ctx, ARG(3));
        if (on && delay > 0) {
#ifdef __APPLE__
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &delay, sizeof delay);
#else
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &delay, sizeof delay);
#endif
        }
    }
    free(what);
    return JSValueMakeUndefined(ctx);
}

// lookup(host, callback(error code | null, [{ address, family }])): getaddrinfo
// on a thread, as Node's dns.lookup uses libuv's pool.
typedef struct { char *host; JSObjectRef fn; int rc; struct addrinfo *res; jsr_runtime *rt; } lookup_job;
static void lookup_done(jsr_runtime *rt, void *data) {
    lookup_job *j = data;
    JSContextRef ctx = jsr_context(rt);
    JSValueRef args[2] = {JSValueMakeNull(ctx), JSValueMakeNull(ctx)};
    if (j->rc != 0 || !j->res) {
        args[0] = jsr_string(ctx, j->rc == EAI_NONAME
#ifdef EAI_NODATA
            || j->rc == EAI_NODATA
#endif
            ? "ENOTFOUND" : j->rc == EAI_AGAIN ? "EAI_AGAIN" : "EAI_FAIL");
    } else {
        JSValueRef items[64];
        size_t n = 0;
        for (struct addrinfo *a = j->res; a && n < 64; a = a->ai_next) {
            char text[INET6_ADDRSTRLEN] = "";
            void *addr = a->ai_family == AF_INET ? (void *)&((struct sockaddr_in *)a->ai_addr)->sin_addr : (void *)&((struct sockaddr_in6 *)a->ai_addr)->sin6_addr;
            if (!inet_ntop(a->ai_family, addr, text, sizeof text)) continue;
            bool dup = false;
            for (struct addrinfo *b = j->res; b != a; b = b->ai_next) {
                char seen[INET6_ADDRSTRLEN] = "";
                void *baddr = b->ai_family == AF_INET ? (void *)&((struct sockaddr_in *)b->ai_addr)->sin_addr : (void *)&((struct sockaddr_in6 *)b->ai_addr)->sin6_addr;
                if (b->ai_family == a->ai_family && inet_ntop(b->ai_family, baddr, seen, sizeof seen) && !strcmp(seen, text)) dup = true;
            }
            if (dup) continue;
            JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
            set_prop(ctx, o, "address", jsr_string(ctx, text));
            set_prop(ctx, o, "family", JSValueMakeNumber(ctx, a->ai_family == AF_INET ? 4 : 6));
            items[n++] = o;
        }
        args[1] = JSObjectMakeArray(ctx, n, items, NULL);
    }
    if (j->res) freeaddrinfo(j->res);
    jsr_unref(rt);
    jsr_call(rt, j->fn, NULL, 2, args);
    JSValueUnprotect(ctx, j->fn);
    free(j->host);
    free(j);
}
static void *lookup_thread(void *data) {
    lookup_job *j = data;
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    j->rc = getaddrinfo(j->host, NULL, &hints, &j->res);
    jsr_post(j->rt, lookup_done, j);
    return NULL;
}
NATIVE(n_lookup) {
    UNUSED_ARGS;
    if (!JSValueIsObject(ctx, ARG(1))) return throw_message(ctx, exc, "lookup(host, fn)");
    lookup_job *j = calloc(1, sizeof *j);
    j->host = jsr_to_utf8(ctx, ARG(0));
    j->fn = (JSObjectRef)ARG(1);
    j->rt = jsr_from_context(ctx);
    JSValueProtect(ctx, j->fn);
    jsr_ref(j->rt);
    pthread_t thread;
    if (pthread_create(&thread, NULL, lookup_thread, j) != 0) {
        j->rc = EAI_FAIL;
        jsr_post(j->rt, lookup_done, j);
    } else pthread_detach(thread);
    return JSValueMakeUndefined(ctx);
}

// --- zlib ----------------------------------------------------------------------
// __node.zlib(compress, windowBits, bytes, level): one-shot deflate or inflate on
// the system libz. windowBits picks the format as in zlib: 8..15 zlib, -8..-15
// raw, +16 gzip, +32 (inflate only) zlib or gzip by header. Inflating gzip goes
// on through concatenated members, as Node's gunzip does.
static const char *zlib_code(int rc) {
    switch (rc) {
    case Z_NEED_DICT: return "Z_NEED_DICT";
    case Z_ERRNO: return "Z_ERRNO";
    case Z_STREAM_ERROR: return "Z_STREAM_ERROR";
    case Z_DATA_ERROR: return "Z_DATA_ERROR";
    case Z_MEM_ERROR: return "Z_MEM_ERROR";
    case Z_BUF_ERROR: return "Z_BUF_ERROR";
    case Z_VERSION_ERROR: return "Z_VERSION_ERROR";
    default: return "Z_UNKNOWN";
    }
}

static JSValueRef throw_zlib(JSContextRef ctx, JSValueRef *exc, int rc, const char *message) {
    JSValueRef msg = jsr_string(ctx, message);
    JSObjectRef e = JSObjectMakeError(ctx, 1, &msg, NULL);
    set_prop(ctx, e, "errno", JSValueMakeNumber(ctx, rc));
    set_prop(ctx, e, "code", jsr_string(ctx, zlib_code(rc)));
    *exc = e;
    return NULL;
}

NATIVE(n_zlib) {
    UNUSED_ARGS;
    bool compress = JSValueToBoolean(ctx, ARG(0));
    int wbits = (int)num(ctx, ARG(1));
    int level = JSValueIsNumber(ctx, ARG(3)) ? (int)num(ctx, ARG(3)) : Z_DEFAULT_COMPRESSION;
    size_t len = 0;
    const uint8_t *in = bytes_of(ctx, ARG(2), &len);
    if (!in) { in = (const uint8_t *)""; len = 0; }
    z_stream s;
    memset(&s, 0, sizeof s);
    int rc = compress ? deflateInit2(&s, level, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) : inflateInit2(&s, wbits);
    if (rc != Z_OK) return throw_zlib(ctx, exc, rc, s.msg ? s.msg : zError(rc));
    size_t cap = compress ? deflateBound(&s, (uLong)len) + 16 : len * 4 + 256;
    uint8_t *out = malloc(cap);
    s.next_in = (Bytef *)in;
    s.avail_in = (uInt)len;
    bool gzip = !compress && wbits > 15;
    for (;;) {
        size_t used = (size_t)(s.next_out ? (uint8_t *)s.next_out - out : 0);
        if (used == cap) {
            cap *= 2;
            out = realloc(out, cap);
        }
        s.next_out = out + used;
        s.avail_out = (uInt)(cap - used);
        rc = compress ? deflate(&s, Z_FINISH) : inflate(&s, Z_FINISH);
        if (rc == Z_STREAM_END) {
            if (gzip && s.avail_in > 0 && s.next_in[0] == 0x1f) {
                inflateReset(&s);
                continue;
            }
            break;
        }
        if ((rc == Z_OK || rc == Z_BUF_ERROR) && s.avail_out == 0) continue;
        char message[160];
        if (rc == Z_OK || rc == Z_BUF_ERROR) {
            rc = Z_BUF_ERROR;
            snprintf(message, sizeof message, "unexpected end of file");
        } else {
            snprintf(message, sizeof message, "%s", s.msg ? s.msg : zError(rc));
        }
        if (compress) deflateEnd(&s); else inflateEnd(&s);
        free(out);
        return throw_zlib(ctx, exc, rc, message);
    }
    size_t out_len = (size_t)((uint8_t *)s.next_out - out);
    if (compress) deflateEnd(&s); else inflateEnd(&s);
    return make_u8(ctx, out, out_len);
}

NATIVE(n_crc32) {
    UNUSED_ARGS;
    (void)exc;
    size_t len = 0;
    const uint8_t *in = bytes_of(ctx, ARG(0), &len);
    uLong value = JSValueIsNumber(ctx, ARG(1)) ? (uLong)(uint32_t)num(ctx, ARG(1)) : 0;
    return JSValueMakeNumber(ctx, (double)(uint32_t)crc32(value, in ? in : (const Bytef *)"", (uInt)len));
}

// --- secrets -------------------------------------------------------------------
// __node.secrets(op, service, name, value, unrestricted, callback): Bun.secrets
// on the OS store, off the JS thread (a keychain prompt can block), with the
// same items Bun reads and writes: on macOS a generic password keyed by
// service and account; on Linux libsecret's "com.oven-sh.bun.Secret" schema,
// loaded at run time as Bun does. callback(error, result) runs on the JS thread.
enum { SECRET_GET, SECRET_SET, SECRET_DELETE };

typedef struct {
    int op;
    char *service, *name, *value;
    bool unrestricted;
    JSObjectRef callback;
    char *result;           // get: the value; NULL when there's none
    bool deleted;
    char *error, *code;
    jsr_runtime *rt;        // the runtime that asked (a Worker's too)
} secret_job;

#ifdef __APPLE__
static void secret_fail(secret_job *j, OSStatus status) {
    CFStringRef text = SecCopyErrorMessageString(status, NULL);
    char buf[256] = "Keychain error";
    if (text) {
        CFStringGetCString(text, buf, sizeof buf, kCFStringEncodingUTF8);
        CFRelease(text);
    }
    j->error = strdup(buf);
    const char *code = "ERR_SECRETS_PLATFORM_ERROR";
    if (status == errSecUserCanceled) code = "ERR_SECRETS_USER_CANCELED";
    else if (status == errSecAuthFailed) code = "ERR_SECRETS_AUTH_FAILED";
    else if (status == errSecInteractionRequired) code = "ERR_SECRETS_INTERACTION_REQUIRED";
    else if (status == errSecInteractionNotAllowed) code = "ERR_SECRETS_INTERACTION_NOT_ALLOWED";
    else if (status == errSecNotAvailable || status == errSecReadOnlyAttr) code = "ERR_SECRETS_ACCESS_DENIED";
    j->code = strdup(code);
}

static CFMutableDictionaryRef secret_query(secret_job *j) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = CFStringCreateWithCString(NULL, j->service, kCFStringEncodingUTF8);
    CFStringRef account = CFStringCreateWithCString(NULL, j->name, kCFStringEncodingUTF8);
    CFDictionaryAddValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionaryAddValue(q, kSecAttrAccount, account);
    CFDictionaryAddValue(q, kSecAttrService, service);
    CFRelease(service);
    CFRelease(account);
    return q;
}

static void secret_delete(secret_job *j) {
    CFMutableDictionaryRef q = secret_query(j);
    OSStatus status = SecItemDelete(q);
    CFRelease(q);
    j->deleted = status == errSecSuccess;
    if (status != errSecSuccess && status != errSecItemNotFound) secret_fail(j, status);
}

static void secret_run(secret_job *j) {
    if (j->op == SECRET_DELETE || (j->op == SECRET_SET && !j->value[0])) {
        secret_delete(j);
        return;
    }
    CFMutableDictionaryRef q = secret_query(j);
    if (j->op == SECRET_GET) {
        CFDictionaryAddValue(q, kSecReturnData, kCFBooleanTrue);
        CFTypeRef data = NULL;
        OSStatus status = SecItemCopyMatching(q, &data);
        if (status == errSecSuccess && data) {
            CFIndex n = CFDataGetLength(data);
            j->result = malloc((size_t)n + 1);
            memcpy(j->result, CFDataGetBytePtr(data), (size_t)n);
            j->result[n] = 0;
        } else if (status != errSecItemNotFound) {
            secret_fail(j, status);
        }
        if (data) CFRelease(data);
        CFRelease(q);
        return;
    }
    CFDataRef value = CFDataCreate(NULL, (const UInt8 *)j->value, (CFIndex)strlen(j->value));
    CFDictionaryAddValue(q, kSecValueData, value);
    if (j->unrestricted) {
        // SecAccessCreate is deprecated without a replacement for this (any app
        // may read the item, for headless CI), so it's looked up as Bun does.
        typedef OSStatus (*access_create_fn)(CFStringRef, CFArrayRef, void **);
        access_create_fn access_create = (access_create_fn)dlsym(RTLD_DEFAULT, "SecAccessCreate");
        void *access = NULL;
        if (access_create && access_create(CFSTR("Bun secrets access"), NULL, &access) == errSecSuccess && access) {
            CFDictionaryAddValue(q, kSecAttrAccess, access);
            CFRelease(access);
        }
    }
    OSStatus status = SecItemAdd(q, NULL);
    if (status == errSecDuplicateItem) {
        CFMutableDictionaryRef match = secret_query(j);
        CFMutableDictionaryRef update = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionaryAddValue(update, kSecValueData, value);
        status = SecItemUpdate(match, update);
        CFRelease(match);
        CFRelease(update);
    }
    CFRelease(value);
    CFRelease(q);
    if (status != errSecSuccess) secret_fail(j, status);
}
#else
typedef struct { const char *name; int type; } lw_secret_attr;
typedef struct { const char *name; int flags; lw_secret_attr attributes[32]; int reserved; void *reserved1, *reserved2, *reserved3, *reserved4, *reserved5, *reserved6, *reserved7; } lw_secret_schema;
static const lw_secret_schema bun_schema = {"com.oven-sh.bun.Secret", 0, {{"service", 0}, {"account", 0}, {NULL, 0}}, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static void secret_run(secret_job *j) {
    static void *lib;
    if (!lib) lib = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
    typedef gboolean (*store_fn)(const lw_secret_schema *, const char *, const char *, const char *, void *, GError **, ...);
    typedef char *(*lookup_fn)(const lw_secret_schema *, void *, GError **, ...);
    typedef gboolean (*clear_fn)(const lw_secret_schema *, void *, GError **, ...);
    typedef void (*free_fn)(char *);
    store_fn store = lib ? (store_fn)dlsym(lib, "secret_password_store_sync") : NULL;
    lookup_fn lookup = lib ? (lookup_fn)dlsym(lib, "secret_password_lookup_sync") : NULL;
    clear_fn clear = lib ? (clear_fn)dlsym(lib, "secret_password_clear_sync") : NULL;
    free_fn pw_free = lib ? (free_fn)dlsym(lib, "secret_password_free") : NULL;
    if (!store || !lookup || !clear || !pw_free) {
        j->error = strdup("libsecret not available");
        j->code = strdup("ERR_SECRETS_PLATFORM_ERROR");
        return;
    }
    GError *err = NULL;
    if (j->op == SECRET_GET) {
        char *pw = lookup(&bun_schema, NULL, &err, "service", j->service, "account", j->name, NULL);
        if (pw) {
            j->result = strdup(pw);
            pw_free(pw);
        }
    } else if (j->op == SECRET_DELETE || !j->value[0]) {
        j->deleted = clear(&bun_schema, NULL, &err, "service", j->service, "account", j->name, NULL);
    } else {
        char *label = g_strdup_printf("%s/%s", j->service, j->name);
        store(&bun_schema, NULL, label, j->value, NULL, &err, "service", j->service, "account", j->name, NULL);
        g_free(label);
    }
    if (err) {
        j->error = strdup(err->message);
        j->code = strdup("ERR_SECRETS_PLATFORM_ERROR");
        g_error_free(err);
    }
}
#endif

static void secret_done(jsr_runtime *rt, void *data) {
    secret_job *j = data;
    JSContextRef ctx = jsr_context(rt);
    JSValueRef args[2] = {JSValueMakeNull(ctx), JSValueMakeNull(ctx)};
    if (j->error) {
        JSValueRef msg = jsr_string(ctx, j->error);
        JSObjectRef e = JSObjectMakeError(ctx, 1, &msg, NULL);
        set_prop(ctx, e, "code", jsr_string(ctx, j->code));
        args[0] = e;
    } else if (j->op == SECRET_GET) {
        if (j->result) args[1] = jsr_string(ctx, j->result);
    } else if (j->op == SECRET_DELETE) {
        args[1] = JSValueMakeBoolean(ctx, j->deleted);
    }
    JSValueRef exception = NULL;
    JSObjectCallAsFunction(ctx, j->callback, NULL, 2, args, &exception);
    if (exception) jsr_report_exception(rt, exception);
    JSValueUnprotect(ctx, j->callback);
    jsr_unref(rt);
    if (j->result) memset(j->result, 0, strlen(j->result));
    if (j->value) memset(j->value, 0, strlen(j->value));
    free(j->service); free(j->name); free(j->value); free(j->result); free(j->error); free(j->code);
    free(j);
}

static void *secret_thread(void *data) {
    secret_run(data);
    jsr_post(((secret_job *)data)->rt, secret_done, data);
    return NULL;
}

NATIVE(n_secrets) {
    UNUSED_ARGS;
    (void)exc;
    secret_job *j = calloc(1, sizeof *j);
    j->op = (int)num(ctx, ARG(0));
    j->service = jsr_to_utf8(ctx, ARG(1));
    j->name = jsr_to_utf8(ctx, ARG(2));
    j->value = JSValueIsString(ctx, ARG(3)) ? jsr_to_utf8(ctx, ARG(3)) : strdup("");
    j->unrestricted = JSValueToBoolean(ctx, ARG(4));
    j->callback = JSValueToObject(ctx, ARG(5), NULL);
    JSValueProtect(ctx, j->callback);
    j->rt = jsr_from_context(ctx);
    jsr_ref(j->rt);
    pthread_t thread;
    if (pthread_create(&thread, NULL, secret_thread, j) != 0) {
        j->error = strdup("couldn't start a thread for the secret store");
        j->code = strdup("ERR_SECRETS_PLATFORM_ERROR");
        jsr_post(j->rt, secret_done, j);
    } else pthread_detach(thread);
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
    jsr_set_function(ctx, fs, "symlink", n_symlink);
    jsr_set_function(ctx, fs, "link", n_link);
    jsr_set_function(ctx, fs, "readlink", n_readlink);
    jsr_set_function(ctx, fs, "utimes", n_utimes);
    set_prop(ctx, fs, "constants", fs_constants(ctx));
    set_prop(ctx, node, "errnos", errno_names(ctx));
    set_prop(ctx, node, "fs", fs);
    jsr_set_function(ctx, node, "osInfo", n_os_info);
    jsr_set_function(ctx, node, "randomFill", n_random_fill);
    jsr_set_function(ctx, node, "aesGcm", n_aes_gcm);
    jsr_set_function(ctx, node, "zlib", n_zlib);
    jsr_set_function(ctx, node, "crc32", n_crc32);
    jsr_set_function(ctx, node, "secrets", n_secrets);
    jsr_set_function(ctx, node, "watchFd", n_watch_fd);
    jsr_set_function(ctx, node, "unwatchFd", n_unwatch_fd);
    jsr_set_function(ctx, node, "refFd", n_ref_fd);
    jsr_set_function(ctx, node, "spawn", n_spawn);
    jsr_set_function(ctx, node, "spawnSync", n_spawn_sync);
    jsr_set_function(ctx, node, "childRef", n_child_ref);
    jsr_set_function(ctx, node, "kill", n_kill);
    jsr_set_function(ctx, node, "writeFd", n_write_fd);
    jsr_set_function(ctx, node, "whenWritable", n_when_writable);
    jsr_set_function(ctx, node, "cancelWritable", n_cancel_writable);
    jsr_set_function(ctx, node, "whenReadable", n_when_readable);
    jsr_set_function(ctx, node, "refWait", n_ref_wait);
    jsr_set_function(ctx, node, "sockConnect", n_sock_connect);
    jsr_set_function(ctx, node, "sockError", n_sock_error);
    jsr_set_function(ctx, node, "sockListen", n_sock_listen);
    jsr_set_function(ctx, node, "sockAccept", n_sock_accept);
    jsr_set_function(ctx, node, "sockName", n_sock_name);
    jsr_set_function(ctx, node, "sockShutdown", n_sock_shutdown);
    jsr_set_function(ctx, node, "sockOption", n_sock_option);
    jsr_set_function(ctx, node, "lookup", n_lookup);
    jsr_set_function(ctx, node, "which", n_which);
    set_prop(ctx, node, "signals", signal_names(ctx));
    // As in Node and Bun: a write to a closed pipe fails with EPIPE instead of
    // killing the process.
    signal(SIGPIPE, SIG_IGN);
    set_prop(ctx, JSContextGetGlobalObject(ctx), "__node", node);
}
