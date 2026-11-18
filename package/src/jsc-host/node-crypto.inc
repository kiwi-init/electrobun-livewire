// --- crypto beyond randomness and AES-GCM ------------------------------------
// Included by node.c (it uses node.c's helpers). Hashes, HMAC, PBKDF2 and
// AES-CBC come from the system: CommonCrypto on macOS, libgcrypt on Linux.
// X25519, Ed25519 and BLAKE2b come from the vendored Monocypher
// (vendor/monocypher), the same code on every OS. node/webcrypto.js and
// node/crypto.js build crypto.subtle and node:crypto on these.
#include "vendor/monocypher/monocypher.h"
#include "vendor/monocypher/monocypher-ed25519.h"
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#endif

typedef enum { HASH_SHA1, HASH_SHA256, HASH_SHA384, HASH_SHA512, HASH_BAD } hash_alg;
static const size_t hash_size[] = {20, 32, 48, 64};
static hash_alg hash_named(JSContextRef ctx, JSValueRef v) {
    char *name = jsr_to_utf8(ctx, v);
    hash_alg a = !name ? HASH_BAD
        : !strcmp(name, "sha1") ? HASH_SHA1
        : !strcmp(name, "sha256") ? HASH_SHA256
        : !strcmp(name, "sha384") ? HASH_SHA384
        : !strcmp(name, "sha512") ? HASH_SHA512 : HASH_BAD;
    free(name);
    return a;
}
static const uint8_t no_bytes[1] = {0};
#define BYTES_OR_EMPTY(p) ((p) ? (const uint8_t *)(p) : no_bytes)

#ifndef __APPLE__
static const int gcry_hash_id[] = {GCRY_MD_SHA1, GCRY_MD_SHA256, GCRY_MD_SHA384, GCRY_MD_SHA512};
static void gcry_ready(void) {
    if (!gcry_control(GCRYCTL_INITIALIZATION_FINISHED_P)) {
        gcry_check_version(NULL);
        gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
    }
}
#endif

static void hash_bytes(hash_alg a, const uint8_t *d, size_t n, uint8_t *out) {
    d = BYTES_OR_EMPTY(d);
#ifdef __APPLE__
    // CC_LONG is 32 bits: feed large inputs in 1 GB pieces.
#define CC_HASH(T, CTX) { CTX c; CC_##T##_Init(&c); \
        for (size_t o = 0; o < n;) { CC_LONG k = (CC_LONG)(n - o > (1u << 30) ? (1u << 30) : n - o); CC_##T##_Update(&c, d + o, k); o += k; } \
        CC_##T##_Final(out, &c); }
    switch (a) {
    case HASH_SHA1: CC_HASH(SHA1, CC_SHA1_CTX) break;
    case HASH_SHA256: CC_HASH(SHA256, CC_SHA256_CTX) break;
    case HASH_SHA384: CC_HASH(SHA384, CC_SHA512_CTX) break;
    case HASH_SHA512: CC_HASH(SHA512, CC_SHA512_CTX) break;
    case HASH_BAD: break;
    }
#undef CC_HASH
#else
    gcry_ready();
    gcry_md_hash_buffer(gcry_hash_id[a], out, d, n);
#endif
}

// hash(alg, data) -> digest; alg is sha1, sha256, sha384 or sha512.
NATIVE(n_hash) {
    UNUSED_ARGS;
    hash_alg a = hash_named(ctx, ARG(0));
    if (a == HASH_BAD) return throw_message(ctx, exc, "Unsupported digest");
    size_t len;
    const uint8_t *data = bytes_of(ctx, ARG(1), &len);
    uint8_t *out = malloc(hash_size[a]);
    hash_bytes(a, data, len, out);
    return make_u8(ctx, out, hash_size[a]);
}

// hmac(alg, key, data) -> mac
NATIVE(n_hmac) {
    UNUSED_ARGS;
    hash_alg a = hash_named(ctx, ARG(0));
    if (a == HASH_BAD) return throw_message(ctx, exc, "Unsupported digest");
    size_t key_len, len;
    const uint8_t *key = BYTES_OR_EMPTY(bytes_of(ctx, ARG(1), &key_len));
    const uint8_t *data = BYTES_OR_EMPTY(bytes_of(ctx, ARG(2), &len));
    uint8_t *out = malloc(hash_size[a]);
#ifdef __APPLE__
    static const CCHmacAlgorithm ids[] = {kCCHmacAlgSHA1, kCCHmacAlgSHA256, kCCHmacAlgSHA384, kCCHmacAlgSHA512};
    CCHmac(ids[a], key, key_len, data, len, out);
#else
    gcry_ready();
    gcry_md_hd_t h;
    // An empty HMAC key is valid: it's padded with zeros like any short key.
    uint8_t zero_key[1] = {0};
    if (gcry_md_open(&h, gcry_hash_id[a], GCRY_MD_FLAG_HMAC) ||
        gcry_md_setkey(h, key_len ? key : zero_key, key_len ? key_len : 1)) {
        free(out);
        return throw_message(ctx, exc, "HMAC failed");
    }
    gcry_md_write(h, data, len);
    memcpy(out, gcry_md_read(h, 0), hash_size[a]);
    gcry_md_close(h);
#endif
    return make_u8(ctx, out, hash_size[a]);
}

// pbkdf2(alg, password, salt, iterations, length) -> key bytes
NATIVE(n_pbkdf2) {
    UNUSED_ARGS;
    hash_alg a = hash_named(ctx, ARG(0));
    if (a == HASH_BAD) return throw_message(ctx, exc, "Unsupported digest");
    size_t pw_len, salt_len;
    const uint8_t *pw = BYTES_OR_EMPTY(bytes_of(ctx, ARG(1), &pw_len));
    const uint8_t *salt = BYTES_OR_EMPTY(bytes_of(ctx, ARG(2), &salt_len));
    double iterations = JSValueToNumber(ctx, ARG(3), NULL), length = JSValueToNumber(ctx, ARG(4), NULL);
    if (!(iterations >= 1 && iterations <= 0xffffffffu) || !(length >= 0 && length <= (1 << 30))) return throw_message(ctx, exc, "Invalid PBKDF2 parameters");
    size_t out_len = (size_t)length;
    uint8_t *out = malloc(out_len ? out_len : 1);
    bool ok;
#ifdef __APPLE__
    static const CCPseudoRandomAlgorithm prf[] = {kCCPRFHmacAlgSHA1, kCCPRFHmacAlgSHA256, kCCPRFHmacAlgSHA384, kCCPRFHmacAlgSHA512};
    ok = !out_len || CCKeyDerivationPBKDF(kCCPBKDF2, (const char *)pw, pw_len, salt, salt_len, prf[a], (unsigned)iterations, out, out_len) == kCCSuccess;
#else
    gcry_ready();
    ok = !out_len || gcry_kdf_derive(pw, pw_len, GCRY_KDF_PBKDF2, gcry_hash_id[a], salt, salt_len, (unsigned long)iterations, out_len, out) == 0;
#endif
    if (!ok) { free(out); return throw_message(ctx, exc, "PBKDF2 failed"); }
    return make_u8(ctx, out, out_len);
}

// aesCbc(encrypt, key, iv, data) -> data, with PKCS#7 padding. Decrypting
// with a wrong key or corrupt data throws (bad padding).
NATIVE(n_aes_cbc) {
    UNUSED_ARGS;
    bool encrypt = JSValueToBoolean(ctx, ARG(0));
    size_t key_len, iv_len, len;
    const uint8_t *key = bytes_of(ctx, ARG(1), &key_len), *iv = bytes_of(ctx, ARG(2), &iv_len);
    const uint8_t *data = BYTES_OR_EMPTY(bytes_of(ctx, ARG(3), &len));
    if (!key || (key_len != 16 && key_len != 24 && key_len != 32)) return throw_message(ctx, exc, "Invalid key length");
    if (!iv || iv_len != 16) return throw_message(ctx, exc, "Invalid initialization vector");
    if (!encrypt && (len == 0 || len % 16)) return throw_message(ctx, exc, "The operation failed for an operation-specific reason");
    size_t cap = len + 16, out_len = 0;
    uint8_t *out = malloc(cap);
#ifdef __APPLE__
    bool ok = CCCrypt(encrypt ? kCCEncrypt : kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key, key_len, iv,
                      data, len, out, cap, &out_len) == kCCSuccess;
#else
    gcry_ready();
    int algo = key_len == 16 ? GCRY_CIPHER_AES128 : key_len == 24 ? GCRY_CIPHER_AES192 : GCRY_CIPHER_AES256;
    bool ok = encrypt || (len && len % 16 == 0);
    if (ok) {
        if (encrypt) {
            uint8_t pad = (uint8_t)(16 - len % 16);
            memcpy(out, data, len);
            memset(out + len, pad, pad);
            out_len = len + pad;
        } else {
            memcpy(out, data, len);
            out_len = len;
        }
        gcry_cipher_hd_t hd;
        ok = !gcry_cipher_open(&hd, algo, GCRY_CIPHER_MODE_CBC, 0);
        if (ok) {
            ok = !gcry_cipher_setkey(hd, key, key_len) && !gcry_cipher_setiv(hd, iv, 16) &&
                 !(encrypt ? gcry_cipher_encrypt(hd, out, out_len, NULL, 0) : gcry_cipher_decrypt(hd, out, out_len, NULL, 0));
            gcry_cipher_close(hd);
        }
        if (ok && !encrypt) {
            uint8_t pad = out[out_len - 1];
            ok = pad >= 1 && pad <= 16;
            for (size_t i = 0; ok && i < pad; i++) ok = out[out_len - 1 - i] == pad;
            if (ok) out_len -= pad;
        }
    }
#endif
    if (!ok) { free(out); return throw_message(ctx, exc, "The operation failed for an operation-specific reason"); }
    return make_u8(ctx, out, out_len);
}

static const uint8_t *key32(JSContextRef ctx, JSValueRef v) {
    size_t len;
    const uint8_t *p = bytes_of(ctx, v, &len);
    return p && len == 32 ? p : NULL;
}

// x25519Public(secret) -> public key
NATIVE(n_x25519_public) {
    UNUSED_ARGS;
    const uint8_t *sk = key32(ctx, ARG(0));
    if (!sk) return throw_message(ctx, exc, "Invalid X25519 key");
    uint8_t *pk = malloc(32);
    crypto_x25519_public_key(pk, sk);
    return make_u8(ctx, pk, 32);
}
// x25519(secret, theirPublic) -> shared secret; an all-zero result (a
// small-order public key) throws, as WebCrypto requires.
NATIVE(n_x25519) {
    UNUSED_ARGS;
    const uint8_t *sk = key32(ctx, ARG(0)), *pk = key32(ctx, ARG(1));
    if (!sk || !pk) return throw_message(ctx, exc, "Invalid X25519 key");
    uint8_t *shared = malloc(32), any = 0;
    crypto_x25519(shared, sk, pk);
    for (int i = 0; i < 32; i++) any |= shared[i];
    if (!any) { free(shared); return throw_message(ctx, exc, "The operation failed for an operation-specific reason"); }
    return make_u8(ctx, shared, 32);
}
// ed25519Public(seed) -> public key
NATIVE(n_ed25519_public) {
    UNUSED_ARGS;
    const uint8_t *seed = key32(ctx, ARG(0));
    if (!seed) return throw_message(ctx, exc, "Invalid Ed25519 key");
    uint8_t s[32], sk[64], *pk = malloc(32);
    memcpy(s, seed, 32); // key_pair wipes its seed
    crypto_ed25519_key_pair(sk, pk, s);
    crypto_wipe(sk, sizeof sk);
    return make_u8(ctx, pk, 32);
}
// ed25519Sign(seed, message) -> signature
NATIVE(n_ed25519_sign) {
    UNUSED_ARGS;
    const uint8_t *seed = key32(ctx, ARG(0));
    if (!seed) return throw_message(ctx, exc, "Invalid Ed25519 key");
    size_t len;
    const uint8_t *msg = BYTES_OR_EMPTY(bytes_of(ctx, ARG(1), &len));
    uint8_t s[32], sk[64], pk[32], *sig = malloc(64);
    memcpy(s, seed, 32);
    crypto_ed25519_key_pair(sk, pk, s);
    crypto_ed25519_sign(sig, sk, msg, len);
    crypto_wipe(sk, sizeof sk);
    return make_u8(ctx, sig, 64);
}
// ed25519Verify(publicKey, message, signature) -> boolean
NATIVE(n_ed25519_verify) {
    UNUSED_ARGS; (void)exc;
    size_t len, sig_len;
    const uint8_t *pk = key32(ctx, ARG(0));
    const uint8_t *msg = BYTES_OR_EMPTY(bytes_of(ctx, ARG(1), &len)), *sig = bytes_of(ctx, ARG(2), &sig_len);
    return JSValueMakeBoolean(ctx, pk && sig && sig_len == 64 && crypto_ed25519_check(sig, pk, msg, len) == 0);
}
// blake2b(data, size) -> hash of size bytes (1..64)
NATIVE(n_blake2b) {
    UNUSED_ARGS;
    size_t len;
    const uint8_t *data = BYTES_OR_EMPTY(bytes_of(ctx, ARG(0), &len));
    double size = JSValueToNumber(ctx, ARG(1), NULL);
    if (!(size >= 1 && size <= 64)) return throw_message(ctx, exc, "Invalid BLAKE2b size");
    uint8_t *out = malloc((size_t)size);
    crypto_blake2b(out, (size_t)size, data, len);
    return make_u8(ctx, out, (size_t)size);
}

// --- incremental digests: node:crypto's Hash and Hmac ---------------------
// digestNew(alg, key) -> a digest; alg is sha1/sha256/sha384/sha512 (an HMAC
// when key is bytes) or blake2b512. digestUpdate(d, data) feeds it,
// digestCopy(d) clones it, digestFinal(d) -> its bytes and ends it. Each
// update goes straight into the native context: nothing is buffered in JS.
typedef enum { DIGEST_HASH, DIGEST_HMAC, DIGEST_BLAKE2B } digest_kind;
typedef struct {
    digest_kind kind;
    hash_alg alg;
    bool done;
    union {
#ifdef __APPLE__
        CC_SHA1_CTX sha1;
        CC_SHA256_CTX sha256;
        CC_SHA512_CTX sha512; // SHA-384 too
        CCHmacContext hmac;
#else
        gcry_md_hd_t md; // a hash or an HMAC
#endif
        crypto_blake2b_ctx blake2b;
    } u;
} digest_state;

static void digest_finalize(JSObjectRef o) {
    digest_state *d = JSObjectGetPrivate(o);
    if (!d) return;
#ifndef __APPLE__
    if (d->kind != DIGEST_BLAKE2B && d->u.md) gcry_md_close(d->u.md);
#endif
    free(d);
}
static JSClassRef g_digest_class;
static void digest_class_create(void) {
    JSClassDefinition def = kJSClassDefinitionEmpty;
    def.className = "LivewireDigest";
    def.finalize = digest_finalize;
    g_digest_class = JSClassCreate(&def);
}
static JSClassRef digest_class(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, digest_class_create);
    return g_digest_class;
}
static digest_state *digest_of(JSContextRef ctx, JSValueRef v, JSValueRef *exc) {
    digest_state *d = JSValueIsObjectOfClass(ctx, v, digest_class()) ? JSObjectGetPrivate((JSObjectRef)v) : NULL;
    if (!d) { throw_message(ctx, exc, "Not a digest"); return NULL; }
    if (d->done) { throw_message(ctx, exc, "Digest already called"); return NULL; }
    return d;
}
static size_t digest_size(const digest_state *d) {
    return d->kind == DIGEST_BLAKE2B ? 64 : hash_size[d->alg];
}

NATIVE(n_digest_new) {
    UNUSED_ARGS;
    digest_state *d = calloc(1, sizeof *d);
    char *name = jsr_to_utf8(ctx, ARG(0));
    bool blake = name && !strcmp(name, "blake2b512");
    free(name);
    if (blake) {
        d->kind = DIGEST_BLAKE2B;
        crypto_blake2b_init(&d->u.blake2b, 64);
        return JSObjectMake(ctx, digest_class(), d);
    }
    d->alg = hash_named(ctx, ARG(0));
    if (d->alg == HASH_BAD) { free(d); return throw_message(ctx, exc, "Unsupported digest"); }
    size_t key_len = 0;
    const uint8_t *key = JSValueIsUndefined(ctx, ARG(1)) ? NULL : BYTES_OR_EMPTY(bytes_of(ctx, ARG(1), &key_len));
    d->kind = key ? DIGEST_HMAC : DIGEST_HASH;
#ifdef __APPLE__
    if (key) {
        static const CCHmacAlgorithm ids[] = {kCCHmacAlgSHA1, kCCHmacAlgSHA256, kCCHmacAlgSHA384, kCCHmacAlgSHA512};
        CCHmacInit(&d->u.hmac, ids[d->alg], key, key_len);
    } else switch (d->alg) {
    case HASH_SHA1: CC_SHA1_Init(&d->u.sha1); break;
    case HASH_SHA256: CC_SHA256_Init(&d->u.sha256); break;
    case HASH_SHA384: CC_SHA384_Init(&d->u.sha512); break;
    case HASH_SHA512: CC_SHA512_Init(&d->u.sha512); break;
    case HASH_BAD: break;
    }
#else
    gcry_ready();
    // An empty HMAC key is valid: it's padded with zeros like any short key.
    uint8_t zero_key[1] = {0};
    if (gcry_md_open(&d->u.md, gcry_hash_id[d->alg], key ? GCRY_MD_FLAG_HMAC : 0) ||
        (key && gcry_md_setkey(d->u.md, key_len ? key : zero_key, key_len ? key_len : 1))) {
        if (d->u.md) gcry_md_close(d->u.md);
        free(d);
        return throw_message(ctx, exc, key ? "HMAC failed" : "Hash failed");
    }
#endif
    return JSObjectMake(ctx, digest_class(), d);
}

NATIVE(n_digest_update) {
    UNUSED_ARGS;
    digest_state *d = digest_of(ctx, ARG(0), exc);
    if (!d) return JSValueMakeUndefined(ctx);
    size_t n;
    const uint8_t *p = BYTES_OR_EMPTY(bytes_of(ctx, ARG(1), &n));
    if (d->kind == DIGEST_BLAKE2B) {
        crypto_blake2b_update(&d->u.blake2b, p, n);
        return JSValueMakeUndefined(ctx);
    }
#ifdef __APPLE__
    // CC_LONG is 32 bits: feed large inputs in 1 GB pieces.
    for (size_t o = 0; o < n;) {
        CC_LONG k = (CC_LONG)(n - o > (1u << 30) ? (1u << 30) : n - o);
        if (d->kind == DIGEST_HMAC) CCHmacUpdate(&d->u.hmac, p + o, k);
        else switch (d->alg) {
        case HASH_SHA1: CC_SHA1_Update(&d->u.sha1, p + o, k); break;
        case HASH_SHA256: CC_SHA256_Update(&d->u.sha256, p + o, k); break;
        case HASH_SHA384: CC_SHA384_Update(&d->u.sha512, p + o, k); break;
        case HASH_SHA512: CC_SHA512_Update(&d->u.sha512, p + o, k); break;
        case HASH_BAD: break;
        }
        o += k;
    }
#else
    gcry_md_write(d->u.md, p, n);
#endif
    return JSValueMakeUndefined(ctx);
}

NATIVE(n_digest_copy) {
    UNUSED_ARGS;
    digest_state *d = digest_of(ctx, ARG(0), exc);
    if (!d) return JSValueMakeUndefined(ctx);
    digest_state *c = malloc(sizeof *c);
    *c = *d; // CommonCrypto and Monocypher contexts are plain structs
#ifndef __APPLE__
    if (d->kind != DIGEST_BLAKE2B && gcry_md_copy(&c->u.md, d->u.md)) {
        free(c);
        return throw_message(ctx, exc, "Hash copy failed");
    }
#endif
    return JSObjectMake(ctx, digest_class(), c);
}

NATIVE(n_digest_final) {
    UNUSED_ARGS;
    digest_state *d = digest_of(ctx, ARG(0), exc);
    if (!d) return JSValueMakeUndefined(ctx);
    size_t size = digest_size(d);
    uint8_t *out = malloc(size);
    d->done = true;
    if (d->kind == DIGEST_BLAKE2B) {
        crypto_blake2b_final(&d->u.blake2b, out);
        return make_u8(ctx, out, size);
    }
#ifdef __APPLE__
    if (d->kind == DIGEST_HMAC) CCHmacFinal(&d->u.hmac, out);
    else switch (d->alg) {
    case HASH_SHA1: CC_SHA1_Final(out, &d->u.sha1); break;
    case HASH_SHA256: CC_SHA256_Final(out, &d->u.sha256); break;
    case HASH_SHA384: CC_SHA384_Final(out, &d->u.sha512); break;
    case HASH_SHA512: CC_SHA512_Final(out, &d->u.sha512); break;
    case HASH_BAD: break;
    }
#else
    memcpy(out, gcry_md_read(d->u.md, 0), size);
    gcry_md_close(d->u.md);
    d->u.md = NULL;
#endif
    return make_u8(ctx, out, size);
}

static void crypto_install(JSContextRef ctx, JSObjectRef node) {
    JSObjectRef c = JSObjectMake(ctx, NULL, NULL);
    jsr_set_function(ctx, c, "hash", n_hash);
    jsr_set_function(ctx, c, "hmac", n_hmac);
    jsr_set_function(ctx, c, "pbkdf2", n_pbkdf2);
    jsr_set_function(ctx, c, "aesCbc", n_aes_cbc);
    jsr_set_function(ctx, c, "x25519Public", n_x25519_public);
    jsr_set_function(ctx, c, "x25519", n_x25519);
    jsr_set_function(ctx, c, "ed25519Public", n_ed25519_public);
    jsr_set_function(ctx, c, "ed25519Sign", n_ed25519_sign);
    jsr_set_function(ctx, c, "ed25519Verify", n_ed25519_verify);
    jsr_set_function(ctx, c, "blake2b", n_blake2b);
    jsr_set_function(ctx, c, "digestNew", n_digest_new);
    jsr_set_function(ctx, c, "digestUpdate", n_digest_update);
    jsr_set_function(ctx, c, "digestCopy", n_digest_copy);
    jsr_set_function(ctx, c, "digestFinal", n_digest_final);
    set_prop(ctx, node, "crypto", c);
}
