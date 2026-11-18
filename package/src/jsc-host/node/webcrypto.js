// WebCrypto (crypto.subtle) on the Livewire JSC runtime, over the natives in
// node-crypto.inc: digest, AES-GCM and AES-CBC, HMAC, HKDF, PBKDF2, X25519
// and Ed25519, with raw/pkcs8/spki/jwk keys. Errors are DOMExceptions with
// WebCrypto's names; messages aren't Bun's.
import { Buffer } from "buffer";
import { DOMException } from "./web-events.js";

const n = globalThis.__node;
const c = n.crypto;

const fail = (name, message) => { throw new DOMException(message, name); };
// Usages in the order browsers and Bun list them.
const USAGE_ORDER = ["encrypt", "decrypt", "sign", "verify", "deriveKey", "deriveBits", "wrapKey", "unwrapKey"];
const sortUsages = (usages) => USAGE_ORDER.filter((u) => usages.includes(u));
// A JWK with its members in Bun's (sorted) order.
const sortedJwk = (jwk) => Object.fromEntries(Object.keys(jwk).sort().map((k) => [k, jwk[k]]));

// A copy of a BufferSource's bytes.
function bytes(data, what = "data") {
	if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength));
	if (data instanceof ArrayBuffer) return new Uint8Array(data.slice(0));
	throw new TypeError(`Failed to execute: ${what} is not of type ArrayBuffer or ArrayBufferView`);
}
const buffer = (u8) => u8.buffer.slice(u8.byteOffset, u8.byteOffset + u8.byteLength);
const b64url = (u8) => Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength).toString("base64").replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
const unb64url = (s) => {
	if (typeof s !== "string" || !/^[A-Za-z0-9_-]*$/.test(s)) fail("DataError", "Invalid JWK member");
	return new Uint8Array(Buffer.from(s.replace(/-/g, "+").replace(/_/g, "/"), "base64"));
};
const concat = (...parts) => {
	const out = new Uint8Array(parts.reduce((s, p) => s + p.length, 0));
	let o = 0;
	for (const p of parts) { out.set(p, o); o += p.length; }
	return out;
};

const NAMES = ["AES-GCM", "AES-CBC", "HMAC", "HKDF", "PBKDF2", "X25519", "Ed25519", "SHA-1", "SHA-256", "SHA-384", "SHA-512"];
const canonical = (name) => NAMES.find((x) => x.toLowerCase() === String(name).toLowerCase());
function normalize(algorithm) {
	const a = typeof algorithm === "string" ? { name: algorithm } : algorithm;
	if (!a || typeof a !== "object" || a.name === undefined) throw new TypeError("Algorithm: name is missing");
	const name = canonical(a.name);
	if (!name) fail("NotSupportedError", "Unrecognized algorithm name");
	return { ...a, name };
}
const HASHES = { "SHA-1": "sha1", "SHA-256": "sha256", "SHA-384": "sha384", "SHA-512": "sha512" };
const BLOCK = { "SHA-1": 64, "SHA-256": 64, "SHA-384": 128, "SHA-512": 128 };
function hashOf(a) {
	if (a.hash === undefined) throw new TypeError("Algorithm: hash is missing");
	const h = normalize(a.hash).name;
	if (!HASHES[h]) fail("NotSupportedError", "Unrecognized hash");
	return h;
}

// Key material lives here, not on the CryptoKey.
const material = new WeakMap();
let constructing = false;
class CryptoKey {
	constructor() {
		if (!constructing) throw new TypeError("Illegal constructor");
	}
	get [Symbol.toStringTag]() { return "CryptoKey"; }
}
function makeKey(type, extractable, algorithm, usages, data) {
	constructing = true;
	const key = new CryptoKey();
	constructing = false;
	Object.defineProperties(key, {
		type: { value: type, enumerable: true },
		extractable: { value: !!extractable, enumerable: true },
		algorithm: { value: Object.freeze(algorithm), enumerable: true },
		usages: { value: Object.freeze(sortUsages(usages)), enumerable: true },
	});
	material.set(key, data);
	return key;
}
const dataOf = (key) => {
	const d = material.get(key);
	if (!d) throw new TypeError("parameter is not of type 'CryptoKey'");
	return d;
};
function use(key, name, usage) {
	dataOf(key);
	if (key.algorithm.name !== name) fail("InvalidAccessError", "The requested operation is not valid for the provided key");
	if (!key.usages.includes(usage)) fail("InvalidAccessError", "The requested operation is not valid for the provided key");
}

const ALLOWED = {
	"AES-GCM": ["encrypt", "decrypt", "wrapKey", "unwrapKey"],
	"AES-CBC": ["encrypt", "decrypt", "wrapKey", "unwrapKey"],
	HMAC: ["sign", "verify"],
	HKDF: ["deriveKey", "deriveBits"],
	PBKDF2: ["deriveKey", "deriveBits"],
	X25519: { private: ["deriveKey", "deriveBits"], public: [] },
	Ed25519: { private: ["sign"], public: ["verify"] },
};
function checkUsages(name, type, usages) {
	const allowed = Array.isArray(ALLOWED[name]) ? ALLOWED[name] : ALLOWED[name][type];
	for (const u of usages) if (!allowed.includes(u)) fail("SyntaxError", "Cannot create a key using the specified key usages.");
	return [...new Set(usages)];
}

// DER for the OKP keys: fixed prefixes around the 32 key bytes (RFC 8410).
const OID = { X25519: [0x2b, 0x65, 0x6e], Ed25519: [0x2b, 0x65, 0x70] };
const pkcs8Prefix = (name) => new Uint8Array([0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, ...OID[name], 0x04, 0x22, 0x04, 0x20]);
const spkiPrefix = (name) => new Uint8Array([0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, ...OID[name], 0x03, 0x21, 0x00]);
function unwrapDer(der, prefix) {
	if (der.length !== prefix.length + 32 || prefix.some((b, i) => der[i] !== b)) fail("DataError", "Invalid keyData");
	return der.slice(prefix.length);
}
const publicOf = (name, secret) => (name === "X25519" ? c.x25519Public(secret) : c.ed25519Public(secret));

function importKeySync(format, keyData, algorithm, extractable, usages) {
	const a = normalize(algorithm);
	const name = a.name;
	switch (name) {
		case "AES-GCM":
		case "AES-CBC":
		case "HMAC": {
			let raw;
			if (format === "raw") raw = bytes(keyData, "keyData");
			else if (format === "jwk") {
				if (keyData?.kty !== "oct") fail("DataError", "The JWK \"kty\" member was not \"oct\"");
				raw = unb64url(keyData.k);
			} else fail("NotSupportedError", "Unsupported key format for this algorithm");
			if (name === "HMAC") {
				const hash = hashOf(a);
				if (!raw.length) fail("DataError", "HMAC key data must not be empty");
				const length = a.length ?? raw.length * 8;
				return makeKey("secret", extractable, { name, hash: { name: hash }, length }, checkUsages(name, "secret", usages), raw);
			}
			if (![16, 24, 32].includes(raw.length)) fail("DataError", "AES key data must be 128 or 256 bits");
			return makeKey("secret", extractable, { name, length: raw.length * 8 }, checkUsages(name, "secret", usages), raw);
		}
		case "HKDF":
		case "PBKDF2":
			if (format !== "raw") fail("NotSupportedError", "Unsupported key format for this algorithm");
			if (extractable) fail("SyntaxError", "KDF keys must not be extractable");
			return makeKey("secret", false, { name }, checkUsages(name, "secret", usages), bytes(keyData, "keyData"));
		case "X25519":
		case "Ed25519": {
			let type, data;
			if (format === "raw") { type = "public"; data = bytes(keyData, "keyData"); if (data.length !== 32) fail("DataError", "Invalid keyData"); }
			else if (format === "spki") { type = "public"; data = unwrapDer(bytes(keyData, "keyData"), spkiPrefix(name)); }
			else if (format === "pkcs8") { type = "private"; data = unwrapDer(bytes(keyData, "keyData"), pkcs8Prefix(name)); }
			else if (format === "jwk") {
				if (keyData?.kty !== "OKP" || keyData.crv !== name) fail("DataError", "Invalid JWK");
				const x = unb64url(keyData.x);
				if (keyData.d !== undefined) {
					type = "private";
					data = unb64url(keyData.d);
					if (data.length !== 32 || !equal(publicOf(name, data), x)) fail("DataError", "Invalid JWK");
				} else { type = "public"; data = x; if (data.length !== 32) fail("DataError", "Invalid JWK"); }
			} else fail("NotSupportedError", "Unsupported key format for this algorithm");
			return makeKey(type, type === "public" ? true : extractable, { name }, checkUsages(name, type, usages), data);
		}
		default:
			fail("NotSupportedError", "Unsupported algorithm for importKey");
	}
}
function equal(a, b) {
	if (a.length !== b.length) return false;
	let d = 0;
	for (let i = 0; i < a.length; i++) d |= a[i] ^ b[i];
	return d === 0;
}

function exportKeySync(format, key) {
	const data = dataOf(key);
	if (!key.extractable) fail("InvalidAccessError", "key is not extractable");
	const name = key.algorithm.name;
	const ops = { key_ops: [...key.usages], ext: true };
	if (name === "X25519" || name === "Ed25519") {
		if (format === "raw" && key.type === "public") return buffer(data);
		if (format === "spki" && key.type === "public") return buffer(concat(spkiPrefix(name), data));
		if (format === "pkcs8" && key.type === "private") return buffer(concat(pkcs8Prefix(name), data));
		if (format === "jwk") {
			const x = key.type === "private" ? publicOf(name, data) : data;
			return sortedJwk({ ...(name === "Ed25519" ? { alg: "Ed25519" } : {}), crv: name, ...(key.type === "private" ? { d: b64url(data) } : {}), ...ops, kty: "OKP", x: b64url(x) });
		}
		fail("InvalidAccessError", "Unsupported key format for this key");
	}
	if (name === "AES-GCM" || name === "AES-CBC" || name === "HMAC") {
		if (format === "raw") return buffer(data);
		if (format === "jwk") {
			const alg = name === "HMAC" ? "HS" + key.algorithm.hash.name.slice(4) : "A" + data.length * 8 + (name === "AES-GCM" ? "GCM" : "CBC");
			return sortedJwk({ alg, ...ops, k: b64url(data), kty: "oct" });
		}
	}
	fail("NotSupportedError", "Unsupported key format for this key");
}

function hkdf(hash, ikm, salt, info, length) {
	const h = HASHES[hash], size = c.hash(h, new Uint8Array(0)).length;
	if (length > 255 * size * 8) fail("OperationError", "HKDF length is too long");
	const prk = c.hmac(h, salt.length ? salt : new Uint8Array(size), ikm);
	const out = new Uint8Array(Math.ceil(length / 8 / size) * size);
	let t = new Uint8Array(0);
	for (let i = 0; i * size < length / 8; i++) {
		t = c.hmac(h, prk, concat(t, info, new Uint8Array([i + 1])));
		out.set(t, i * size);
	}
	return out.slice(0, length / 8);
}

function deriveBitsSync(algorithm, baseKey, length) {
	const a = normalize(algorithm);
	use(baseKey, a.name, "deriveBits");
	return deriveWith(a, baseKey, length);
}
function deriveWith(a, baseKey, length) {
	const data = dataOf(baseKey);
	if (length !== null && length !== undefined && (!Number.isInteger(length) || length % 8)) fail("OperationError", "length must be a multiple of 8");
	switch (a.name) {
		case "X25519": {
			const pub = a.public;
			if (!material.has(pub) || pub.type !== "public" || pub.algorithm.name !== "X25519") fail("InvalidAccessError", "Invalid public key");
			if (baseKey.type !== "private") fail("InvalidAccessError", "baseKey must be a private key");
			let shared;
			try { shared = c.x25519(data, dataOf(pub)); } catch { fail("OperationError", "The X25519 result is all zero"); }
			if (length === null || length === undefined) return buffer(shared);
			if (length > 256) fail("OperationError", "length is too long");
			return buffer(shared.slice(0, length / 8));
		}
		case "HKDF":
			if (length === null || length === undefined) fail("OperationError", "length is required");
			return buffer(hkdf(hashOf(a), data, bytes(a.salt, "salt"), bytes(a.info, "info"), length));
		case "PBKDF2":
			if (length === null || length === undefined) fail("OperationError", "length is required");
			if (!(a.iterations > 0)) fail("OperationError", "iterations must be greater than 0");
			return buffer(c.pbkdf2(HASHES[hashOf(a)], data, bytes(a.salt, "salt"), a.iterations, length / 8));
		default:
			fail("NotSupportedError", "Unsupported algorithm for deriveBits");
	}
}

function keyLength(derived) {
	if (derived.name === "AES-GCM" || derived.name === "AES-CBC") {
		if (![128, 192, 256].includes(derived.length)) fail("OperationError", "AES key length must be 128, 192 or 256 bits");
		return derived.length;
	}
	if (derived.name === "HMAC") return derived.length ?? BLOCK[hashOf(derived)] * 8;
	if (derived.name === "HKDF" || derived.name === "PBKDF2") return null;
	fail("NotSupportedError", "Unsupported derived key algorithm");
}

function cipher(encrypt, algorithm, key, data) {
	const a = normalize(algorithm);
	use(key, a.name, encrypt ? "encrypt" : "decrypt");
	const raw = dataOf(key), input = bytes(data);
	if (a.name === "AES-GCM") {
		const iv = bytes(a.iv, "iv"), aad = a.additionalData === undefined ? new Uint8Array(0) : bytes(a.additionalData, "additionalData");
		const tagBytes = (a.tagLength ?? 128) / 8;
		if (![4, 8, 12, 13, 14, 15, 16].includes(tagBytes)) fail("OperationError", "Invalid tagLength");
		if (!iv.length) fail("OperationError", "Invalid iv length");
		if (encrypt) {
			const r = n.aesGcm(true, raw, iv, input, aad);
			return buffer(concat(r.data, r.tag.slice(0, tagBytes)));
		}
		if (input.length < tagBytes) fail("OperationError", "The provided data is too small");
		try {
			return buffer(n.aesGcm(false, raw, iv, input.slice(0, input.length - tagBytes), aad, input.slice(input.length - tagBytes)));
		} catch { fail("OperationError", "The operation failed for an operation-specific reason"); }
	}
	if (a.name === "AES-CBC") {
		const iv = bytes(a.iv, "iv");
		if (iv.length !== 16) fail("OperationError", "iv must be 16 bytes");
		if (!encrypt && (!input.length || input.length % 16)) fail("OperationError", "The data isn't a whole number of AES blocks");
		try { return buffer(c.aesCbc(encrypt, raw, iv, input)); } catch { fail("OperationError", "The operation failed for an operation-specific reason"); }
	}
	fail("NotSupportedError", "Unsupported algorithm");
}

function signSync(algorithm, key, data) {
	const a = normalize(algorithm);
	use(key, a.name, "sign");
	if (a.name === "HMAC") return buffer(c.hmac(HASHES[key.algorithm.hash.name], dataOf(key), bytes(data)));
	if (a.name === "Ed25519") return buffer(c.ed25519Sign(dataOf(key), bytes(data)));
	fail("NotSupportedError", "Unsupported algorithm");
}
function verifySync(algorithm, key, signature, data) {
	const a = normalize(algorithm);
	use(key, a.name, "verify");
	const sig = bytes(signature, "signature");
	if (a.name === "HMAC") return equal(c.hmac(HASHES[key.algorithm.hash.name], dataOf(key), bytes(data)), sig);
	if (a.name === "Ed25519") return c.ed25519Verify(dataOf(key), bytes(data), sig);
	fail("NotSupportedError", "Unsupported algorithm");
}

function generateKeySync(algorithm, extractable, usages) {
	const a = normalize(algorithm);
	const random = (len) => n.randomFill(new Uint8Array(len));
	switch (a.name) {
		case "AES-GCM":
		case "AES-CBC":
			if (![128, 192, 256].includes(a.length)) fail("OperationError", "AES key length must be 128, 192 or 256 bits");
			return makeKey("secret", extractable, { name: a.name, length: a.length }, checkUsages(a.name, "secret", usages), random(a.length / 8));
		case "HMAC": {
			const hash = hashOf(a), length = a.length ?? BLOCK[hash] * 8;
			return makeKey("secret", extractable, { name: "HMAC", hash: { name: hash }, length }, checkUsages("HMAC", "secret", usages), random(Math.ceil(length / 8)));
		}
		case "X25519":
		case "Ed25519": {
			const secret = random(32);
			const privateUsages = checkUsages(a.name, "private", usages.filter((u) => ALLOWED[a.name].private.includes(u)));
			const publicUsages = usages.filter((u) => ALLOWED[a.name].public.includes(u));
			if (usages.some((u) => !ALLOWED[a.name].private.includes(u) && !ALLOWED[a.name].public.includes(u))) fail("SyntaxError", "Cannot create a key using the specified key usages.");
			if (!privateUsages.length) fail("SyntaxError", "Usages cannot be empty when creating a key.");
			return {
				publicKey: makeKey("public", true, { name: a.name }, publicUsages, publicOf(a.name, secret)),
				privateKey: makeKey("private", extractable, { name: a.name }, privateUsages, secret),
			};
		}
		default:
			fail("NotSupportedError", "Unsupported algorithm for generateKey");
	}
}

// Each method runs synchronously and settles its promise with the result or
// the error, as the spec's "in parallel" steps would.
const run = (fn) => new Promise((resolve) => resolve(fn()));
class SubtleCrypto {
	constructor() {
		if (!constructing) throw new TypeError("Illegal constructor");
	}
	get [Symbol.toStringTag]() { return "SubtleCrypto"; }
	digest(algorithm, data) {
		return run(() => {
			const a = normalize(algorithm);
			if (!HASHES[a.name]) fail("NotSupportedError", "Unsupported digest");
			return buffer(c.hash(HASHES[a.name], bytes(data)));
		});
	}
	generateKey(algorithm, extractable, usages) { return run(() => generateKeySync(algorithm, extractable, usages)); }
	importKey(format, keyData, algorithm, extractable, usages) { return run(() => importKeySync(format, keyData, algorithm, extractable, usages)); }
	exportKey(format, key) { return run(() => exportKeySync(format, key)); }
	encrypt(algorithm, key, data) { return run(() => cipher(true, algorithm, key, data)); }
	decrypt(algorithm, key, data) { return run(() => cipher(false, algorithm, key, data)); }
	sign(algorithm, key, data) { return run(() => signSync(algorithm, key, data)); }
	verify(algorithm, key, signature, data) { return run(() => verifySync(algorithm, key, signature, data)); }
	deriveBits(algorithm, baseKey, length) { return run(() => deriveBitsSync(algorithm, baseKey, length)); }
	deriveKey(algorithm, baseKey, derivedKeyAlgorithm, extractable, usages) {
		return run(() => {
			const a = normalize(algorithm);
			use(baseKey, a.name, "deriveKey");
			const derived = normalize(derivedKeyAlgorithm);
			const bits = deriveWith(a, baseKey, keyLength(derived));
			return importKeySync("raw", bits, derived, extractable, usages);
		});
	}
	wrapKey() { return run(() => fail("NotSupportedError", "wrapKey isn't available on the JSC runtime")); }
	unwrapKey() { return run(() => fail("NotSupportedError", "unwrapKey isn't available on the JSC runtime")); }
}
constructing = true;
export const subtle = new SubtleCrypto();
constructing = false;
export { CryptoKey, SubtleCrypto };
// For node:crypto's KeyObject: raw material and a key from it.
export const keyMaterial = dataOf;
export { importKeySync, exportKeySync, hkdf };
