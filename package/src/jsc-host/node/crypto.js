// node:crypto on the Livewire JSC runtime: randomness, hashes, HMAC, PBKDF2,
// HKDF, AES-GCM/CBC ciphers and Ed25519/X25519 KeyObjects with sign/verify.
// RSA and EC keys aren't here.
import { Buffer } from "buffer";
import { subtle, CryptoKey, keyMaterial, importKeySync, exportKeySync, hkdf as hkdfBits } from "./webcrypto.js";

const n = globalThis.__node;
const bytesOf = (data, encoding) => {
	if (typeof data === "string") return Buffer.from(data, encoding ?? "utf8");
	if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
	if (data instanceof ArrayBuffer) return new Uint8Array(data);
	throw new TypeError("Expected a string, Buffer, TypedArray or ArrayBuffer");
};

export function randomBytes(size, callback) {
	const out = Buffer.alloc(size);
	n.randomFill(out);
	if (callback) { queueMicrotask(() => callback(null, out)); return; }
	return out;
}
export function randomFillSync(view, offset = 0, size) {
	const bytes = new Uint8Array(view.buffer ?? view, (view.byteOffset ?? 0) + offset, size ?? (view.byteLength - offset));
	n.randomFill(bytes);
	return view;
}
export function randomUUID() {
	const b = randomBytes(16);
	b[6] = (b[6] & 0x0f) | 0x40;
	b[8] = (b[8] & 0x3f) | 0x80;
	const h = b.toString("hex");
	return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
}
export function randomInt(min, max) {
	if (max === undefined) { max = min; min = 0; }
	const range = max - min;
	const b = randomBytes(6);
	return min + Math.floor((b.readUIntBE(0, 6) / 2 ** 48) * range);
}
export const getRandomValues = (view) => { n.randomFill(new Uint8Array(view.buffer, view.byteOffset, view.byteLength)); return view; };

const GCM = { "aes-128-gcm": 16, "aes-192-gcm": 24, "aes-256-gcm": 32 };
const CBC = { "aes-128-cbc": 16, "aes-192-cbc": 24, "aes-256-cbc": 32 };
function checkGcm(algorithm, key) {
	const keyLength = GCM[String(algorithm).toLowerCase()];
	if (!keyLength) throw new Error(`Unsupported cipher on the JSC runtime: ${algorithm}`);
	if (bytesOf(key).length !== keyLength) throw new RangeError("Invalid key length");
}
// AES-CBC with PKCS#7 padding; like GCM, the work happens in final().
class CbcCipher {
	constructor(encrypt, algorithm, key, iv) {
		if (bytesOf(key).length !== CBC[String(algorithm).toLowerCase()]) throw new RangeError("Invalid key length");
		if (bytesOf(iv).length !== 16) throw new TypeError("Invalid initialization vector");
		this._encrypt = encrypt;
		this._key = Buffer.from(bytesOf(key));
		this._iv = Buffer.from(bytesOf(iv));
		this._chunks = [];
		this._done = false;
	}
	setAutoPadding(on = true) { if (!on) throw new Error("setAutoPadding(false) isn't available on the JSC runtime"); return this; }
	update(data, inputEncoding, outputEncoding) {
		if (this._done) throw new Error("Cipher already finalized");
		this._chunks.push(Buffer.from(bytesOf(data, inputEncoding)));
		return outputEncoding ? "" : Buffer.alloc(0);
	}
	final(outputEncoding) {
		if (this._done) throw new Error("Cipher already finalized");
		this._done = true;
		let r;
		try { r = n.crypto.aesCbc(this._encrypt, this._key, this._iv, Buffer.concat(this._chunks)); }
		catch { throw new Error(this._encrypt ? "Encryption failed" : "bad decrypt"); }
		const out = Buffer.from(r.buffer, r.byteOffset, r.byteLength);
		return outputEncoding ? out.toString(outputEncoding) : out;
	}
}
// Node's streaming interface; the work happens in final().
class GcmCipher {
	constructor(encrypt, algorithm, key, iv, options) {
		checkGcm(algorithm, key);
		this._tagLength = options?.authTagLength ?? 16;
		if (!Number.isInteger(this._tagLength) || this._tagLength < 4 || this._tagLength > 16) throw new TypeError("Invalid authentication tag length: " + this._tagLength);
		this._encrypt = encrypt;
		this._key = Buffer.from(bytesOf(key));
		this._iv = Buffer.from(bytesOf(iv));
		this._chunks = [];
		this._aad = Buffer.alloc(0);
		this._tag = null;
		this._done = false;
	}
	setAAD(aad) { this._aad = Buffer.from(bytesOf(aad)); return this; }
	setAutoPadding() { return this; }
	update(data, inputEncoding, outputEncoding) {
		if (this._done) throw new Error("Cipher already finalized");
		this._chunks.push(Buffer.from(bytesOf(data, inputEncoding)));
		return outputEncoding ? "" : Buffer.alloc(0);
	}
	final(outputEncoding) {
		if (this._done) throw new Error("Cipher already finalized");
		this._done = true;
		const input = Buffer.concat(this._chunks);
		let out;
		if (this._encrypt) {
			const r = n.aesGcm(true, this._key, this._iv, input, this._aad);
			out = Buffer.from(r.data.buffer, r.data.byteOffset, r.data.byteLength);
			this._tag = Buffer.from(r.tag.buffer, r.tag.byteOffset, this._tagLength);
		} else {
			if (!this._tag) throw new Error("Unsupported state or unable to authenticate data");
			const r = n.aesGcm(false, this._key, this._iv, input, this._aad, this._tag);
			out = Buffer.from(r.buffer, r.byteOffset, r.byteLength);
		}
		return outputEncoding ? out.toString(outputEncoding) : out;
	}
	getAuthTag() {
		if (!this._encrypt || !this._tag) throw new Error("Invalid state for operation getAuthTag");
		return this._tag;
	}
	setAuthTag(tag) {
		if (this._encrypt) throw new Error("Invalid state for operation setAuthTag");
		const bytes = Buffer.from(bytesOf(tag));
		if (bytes.length !== this._tagLength) throw new TypeError("Invalid authentication tag length: " + bytes.length);
		this._tag = bytes;
		return this;
	}
}
const isCbc = (algorithm) => String(algorithm).toLowerCase() in CBC;
export const createCipheriv = (algorithm, key, iv, options) => (isCbc(algorithm) ? new CbcCipher(true, algorithm, key, iv) : new GcmCipher(true, algorithm, key, iv, options));
export const createDecipheriv = (algorithm, key, iv, options) => (isCbc(algorithm) ? new CbcCipher(false, algorithm, key, iv) : new GcmCipher(false, algorithm, key, iv, options));
const out = (u8, encoding) => {
	const b = Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength);
	return encoding && encoding !== "buffer" ? b.toString(encoding) : b;
};

// Hashes: Node's names (and OpenSSL's aliases) for what the natives do.
const HASH = { sha1: "sha1", "sha-1": "sha1", sha256: "sha256", "sha-256": "sha256", sha384: "sha384", "sha-384": "sha384", sha512: "sha512", "sha-512": "sha512", blake2b512: "blake2b512" };
function hashName(algorithm) {
	const name = HASH[String(algorithm).toLowerCase()];
	if (!name) throw new Error(`Digest method not supported: ${algorithm}`);
	return name;
}
const digestOf = (name, data) => (name === "blake2b512" ? n.crypto.blake2b(data, 64) : n.crypto.hash(name, data));
// Chunks are kept and hashed in digest(): the natives take one buffer.
class Hash {
	constructor(algorithm, chunks = []) { this._name = hashName(algorithm); this._chunks = chunks; this._done = false; }
	update(data, encoding) {
		if (this._done) throw new Error("Digest already called");
		this._chunks.push(Buffer.from(bytesOf(data, encoding)));
		return this;
	}
	digest(encoding) {
		if (this._done) throw new Error("Digest already called");
		this._done = true;
		return out(digestOf(this._name, Buffer.concat(this._chunks)), encoding);
	}
	copy() {
		if (this._done) throw new Error("Digest already called");
		return new Hash(this._name, [...this._chunks]);
	}
}
class Hmac {
	constructor(algorithm, key) {
		this._name = hashName(algorithm);
		if (this._name === "blake2b512") throw new Error("Invalid digest: blake2b512");
		this._key = Buffer.from(key instanceof KeyObject ? key.export() : bytesOf(key));
		this._chunks = [];
		this._done = false;
	}
	update(data, encoding) {
		if (this._done) throw new Error("Digest already called");
		this._chunks.push(Buffer.from(bytesOf(data, encoding)));
		return this;
	}
	digest(encoding) {
		if (this._done) throw new Error("Digest already called");
		this._done = true;
		return out(n.crypto.hmac(this._name, this._key, Buffer.concat(this._chunks)), encoding);
	}
}
export const createHash = (algorithm) => new Hash(algorithm);
export const createHmac = (algorithm, key) => new Hmac(algorithm, key);
export const hash = (algorithm, data, encoding = "hex") => createHash(algorithm).update(data).digest(encoding);
export const getHashes = () => ["blake2b512", "sha1", "sha256", "sha384", "sha512"];
export const getCiphers = () => ["aes-128-cbc", "aes-128-gcm", "aes-192-cbc", "aes-192-gcm", "aes-256-cbc", "aes-256-gcm"];

export function timingSafeEqual(a, b) {
	const x = bytesOf(a), y = bytesOf(b);
	if (x.length !== y.length) throw new RangeError("Input buffers must have the same byte length");
	let d = 0;
	for (let i = 0; i < x.length; i++) d |= x[i] ^ y[i];
	return d === 0;
}

export function pbkdf2Sync(password, salt, iterations, keylen, digest) {
	return out(n.crypto.pbkdf2(hashName(digest), bytesOf(password), bytesOf(salt), iterations, keylen));
}
export function pbkdf2(password, salt, iterations, keylen, digest, callback) {
	let r, err = null;
	try { r = pbkdf2Sync(password, salt, iterations, keylen, digest); } catch (e) { err = e; }
	queueMicrotask(() => callback(err, r));
}
export function hkdfSync(digest, ikm, salt, info, keylen) {
	const key = ikm instanceof KeyObject ? ikm.export() : bytesOf(ikm);
	const name = { sha1: "SHA-1", sha256: "SHA-256", sha384: "SHA-384", sha512: "SHA-512" }[hashName(digest)];
	const r = hkdfBits(name, key, bytesOf(salt), bytesOf(info), keylen * 8);
	return r.buffer.slice(r.byteOffset, r.byteOffset + r.byteLength);
}
export function hkdf(digest, ikm, salt, info, keylen, callback) {
	let r, err = null;
	try { r = hkdfSync(digest, ikm, salt, info, keylen); } catch (e) { err = e; }
	queueMicrotask(() => callback(err, r));
}

// KeyObjects: secret keys, and Ed25519/X25519 keys (from DER, PEM or JWK).
const OKP = ["ed25519", "x25519"];
const okpName = (type) => (type === "ed25519" ? "Ed25519" : "X25519");
function pemBody(text) {
	const m = /-----BEGIN ([A-Z ]+)-----([\s\S]*?)-----END \1-----/.exec(text);
	if (!m) throw new TypeError("Invalid PEM");
	return { label: m[1], der: Buffer.from(m[2].replace(/\s+/g, ""), "base64") };
}
const pem = (label, der) => `-----BEGIN ${label}-----\n${Buffer.from(der).toString("base64").replace(/.{1,64}/g, "$&\n")}-----END ${label}-----\n`;
// The OKP type a DER key names: the last byte of its 1.3.101.x algorithm OID
// (RFC 8410), at a fixed offset in pkcs8 and spki.
function derType(der, kind) {
	const at = kind === "pkcs8" ? 9 : 6;
	if (der[at] === 0x2b && der[at + 1] === 0x65) {
		if (der[at + 2] === 0x70) return "ed25519";
		if (der[at + 2] === 0x6e) return "x25519";
	}
	throw new Error("Unsupported key type on the JSC runtime (only Ed25519 and X25519)");
}

export class KeyObject {
	constructor(type, handle) {
		if (handle === undefined || !["secret", "public", "private"].includes(type)) throw new TypeError("Invalid KeyObject");
		this._type = type;
		this._key = handle; // a CryptoKey, or the secret bytes
	}
	static from(key) {
		if (!(key instanceof CryptoKey)) throw new TypeError('The "key" argument must be an instance of CryptoKey');
		return key.type === "secret" ? new KeyObject("secret", Buffer.from(keyMaterial(key))) : new KeyObject(key.type, key);
	}
	get type() { return this._type; }
	get asymmetricKeyType() { return this._type === "secret" ? undefined : this._key.algorithm.name.toLowerCase(); }
	get symmetricKeySize() { return this._type === "secret" ? this._key.length : undefined; }
	get asymmetricKeyDetails() { return this._type === "secret" ? undefined : {}; }
	equals(other) {
		if (!(other instanceof KeyObject) || other._type !== this._type) return false;
		return timingSafeEqual(this._raw(), other._raw()) && this.asymmetricKeyType === other.asymmetricKeyType;
	}
	_raw() { return this._type === "secret" ? this._key : keyMaterial(this._key); }
	export(options = {}) {
		if (this._type === "secret") {
			if (options.format === "jwk") return { kty: "oct", k: Buffer.from(this._key).toString("base64url") };
			return Buffer.from(this._key);
		}
		const key = this._extractable();
		if (options.format === "jwk") {
			const { crv, d, kty, x } = exportKeySync("jwk", key);
			return d === undefined ? { crv, x, kty } : { crv, d, x, kty };
		}
		const wanted = this._type === "public" ? "spki" : "pkcs8";
		if (options.type && options.type !== wanted) throw new Error(`Unsupported key encoding type: ${options.type}`);
		const der = Buffer.from(exportKeySync(wanted, key));
		if (options.format === "der") return der;
		if (options.format === "pem") return pem(this._type === "public" ? "PUBLIC KEY" : "PRIVATE KEY", der);
		throw new TypeError("The property 'options.format' must be one of: 'pem', 'der', 'jwk'");
	}
	// The same key as an extractable CryptoKey with no usages, to export.
	_extractable() {
		const name = this._key.algorithm.name;
		const raw = keyMaterial(this._key);
		if (this._type === "public") return importKeySync("raw", raw, { name }, true, []);
		const der = new Uint8Array([0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, name === "Ed25519" ? 0x70 : 0x6e, 0x04, 0x22, 0x04, 0x20, ...raw]);
		return importKeySync("pkcs8", der, { name }, true, name === "Ed25519" ? ["sign"] : ["deriveBits"]);
	}
	toCryptoKey(algorithm, extractable, usages) {
		if (this._type === "secret") return importKeySync("raw", this._key, algorithm, extractable, usages);
		const raw = this._extractable();
		return this._type === "public"
			? importKeySync("raw", keyMaterial(raw), algorithm, extractable, usages)
			: importKeySync("pkcs8", exportKeySync("pkcs8", raw), algorithm, extractable, usages);
	}
}
const okpKey = (type, format, data) => {
	const name = okpName(type);
	const usages = format === "pkcs8" ? (name === "Ed25519" ? ["sign"] : ["deriveBits"]) : name === "Ed25519" ? ["verify"] : [];
	return importKeySync(format, data, { name }, true, usages);
};
function asymmetric(input, wantPrivate) {
	if (input instanceof KeyObject) {
		if (input._type === "secret") throw new TypeError("Invalid key object type secret");
		if (!wantPrivate && input._type === "private") return new KeyObject("public", importKeySync("raw", (input._key.algorithm.name === "Ed25519" ? n.crypto.ed25519Public : n.crypto.x25519Public)(keyMaterial(input._key)), { name: input._key.algorithm.name }, true, input._key.algorithm.name === "Ed25519" ? ["verify"] : []));
		return input;
	}
	if (input instanceof CryptoKey) return KeyObject.from(input);
	const opts = typeof input === "string" || ArrayBuffer.isView(input) ? { key: input } : input ?? {};
	let { key, format, type } = opts;
	if (opts.format === "jwk") {
		if (key?.kty !== "OKP" || !["Ed25519", "X25519"].includes(key.crv)) throw new Error("Unsupported JWK key on the JSC runtime (only OKP Ed25519 and X25519)");
		const isPrivate = key.d !== undefined;
		if (wantPrivate && !isPrivate) throw new TypeError("Invalid JWK: missing d");
		const ck = importKeySync("jwk", key, { name: key.crv }, true, isPrivate ? (key.crv === "Ed25519" ? ["sign"] : ["deriveBits"]) : key.crv === "Ed25519" ? ["verify"] : []);
		const ko = new KeyObject(isPrivate ? "private" : "public", ck);
		return wantPrivate || !isPrivate ? ko : asymmetric(ko, false);
	}
	if (typeof key === "string" || format === "pem" || format === undefined) {
		const { label, der } = pemBody(typeof key === "string" ? key : Buffer.from(key).toString());
		if (label === "PRIVATE KEY") {
			const ko = new KeyObject("private", okpKey(derType(der, "pkcs8"), "pkcs8", der));
			return wantPrivate ? ko : asymmetric(ko, false);
		}
		if (label === "PUBLIC KEY" && !wantPrivate) return new KeyObject("public", okpKey(derType(der, "spki"), "spki", der));
		throw new Error(`Unsupported PEM ${label} on the JSC runtime`);
	}
	if (format === "der") {
		const der = bytesOf(key);
		if (type === "pkcs8") {
			const ko = new KeyObject("private", okpKey(derType(der, "pkcs8"), "pkcs8", der));
			return wantPrivate ? ko : asymmetric(ko, false);
		}
		if (type === "spki" && !wantPrivate) return new KeyObject("public", okpKey(derType(der, "spki"), "spki", der));
	}
	throw new TypeError("Unsupported key format on the JSC runtime");
}
export const createPublicKey = (key) => asymmetric(key, false);
export const createPrivateKey = (key) => asymmetric(key, true);
export const createSecretKey = (key, encoding) => new KeyObject("secret", Buffer.from(bytesOf(key, encoding)));

export function generateKeyPairSync(type, options = {}) {
	if (!OKP.includes(type)) throw new Error(`Unsupported key type on the JSC runtime: ${type} (only ed25519 and x25519)`);
	const pair = subtleGenerate(type);
	const encode = (ko, enc) => (enc ? ko.export(enc) : ko);
	return { publicKey: encode(pair.publicKey, options.publicKeyEncoding), privateKey: encode(pair.privateKey, options.privateKeyEncoding) };
}
function subtleGenerate(type) {
	const secret = n.randomFill(new Uint8Array(32));
	const name = okpName(type);
	const der = new Uint8Array([0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, name === "Ed25519" ? 0x70 : 0x6e, 0x04, 0x22, 0x04, 0x20, ...secret]);
	const privateKey = new KeyObject("private", okpKey(type, "pkcs8", der));
	return { privateKey, publicKey: asymmetric(privateKey, false) };
}
export function generateKeyPair(type, options, callback) {
	if (typeof options === "function") { callback = options; options = {}; }
	let r, err = null;
	try { r = generateKeyPairSync(type, options); } catch (e) { err = e; }
	queueMicrotask(() => (err ? callback(err) : callback(null, r.publicKey, r.privateKey)));
}

// One-shot Ed25519 signatures; the algorithm must be null or undefined.
export function sign(algorithm, data, key, callback) {
	if (algorithm !== null && algorithm !== undefined) throw new Error(`Unsupported sign algorithm on the JSC runtime: ${algorithm}`);
	const k = createPrivateKey(key);
	if (k.asymmetricKeyType !== "ed25519") throw new Error("Only Ed25519 keys sign on the JSC runtime");
	const sig = out(n.crypto.ed25519Sign(keyMaterial(k._key), bytesOf(data)));
	if (callback) { queueMicrotask(() => callback(null, sig)); return; }
	return sig;
}
export function verify(algorithm, data, key, signature, callback) {
	if (algorithm !== null && algorithm !== undefined) throw new Error(`Unsupported verify algorithm on the JSC runtime: ${algorithm}`);
	const k = createPublicKey(key);
	if (k.asymmetricKeyType !== "ed25519") throw new Error("Only Ed25519 keys verify on the JSC runtime");
	const ok = n.crypto.ed25519Verify(keyMaterial(k._key), bytesOf(data), bytesOf(signature));
	if (callback) { queueMicrotask(() => callback(null, ok)); return; }
	return ok;
}
export function diffieHellman({ privateKey, publicKey }) {
	if (privateKey.asymmetricKeyType !== "x25519" || publicKey.asymmetricKeyType !== "x25519") throw new Error("Only X25519 keys on the JSC runtime");
	return out(n.crypto.x25519(keyMaterial(privateKey._key), keyMaterial(publicKey._key)));
}

export const webcrypto = { getRandomValues, randomUUID, subtle, CryptoKey };
export { subtle };
export const constants = {};
export default {
	randomBytes, randomFillSync, randomUUID, randomInt, getRandomValues, createCipheriv, createDecipheriv, createHash, createHmac, hash,
	getHashes, getCiphers, timingSafeEqual, pbkdf2, pbkdf2Sync, hkdf, hkdfSync, KeyObject, createPublicKey, createPrivateKey, createSecretKey,
	generateKeyPair, generateKeyPairSync, sign, verify, diffieHellman, webcrypto, subtle, constants,
};
