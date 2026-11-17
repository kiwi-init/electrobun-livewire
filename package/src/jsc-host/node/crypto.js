// node:crypto on the Livewire JSC runtime: randomness and AES-GCM (what the
// SDK uses). The rest lands with milestone 4.
import { Buffer } from "buffer";

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
function checkGcm(algorithm, key) {
	const keyLength = GCM[String(algorithm).toLowerCase()];
	if (!keyLength) throw new Error(`Unsupported cipher on the JSC runtime: ${algorithm}`);
	if (bytesOf(key).length !== keyLength) throw new RangeError("Invalid key length");
}
// Node's streaming interface; the work happens in final().
class GcmCipher {
	constructor(encrypt, algorithm, key, iv) {
		checkGcm(algorithm, key);
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
			this._tag = Buffer.from(r.tag.buffer, r.tag.byteOffset, r.tag.byteLength);
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
		this._tag = Buffer.from(bytesOf(tag));
		return this;
	}
}
export const createCipheriv = (algorithm, key, iv) => new GcmCipher(true, algorithm, key, iv);
export const createDecipheriv = (algorithm, key, iv) => new GcmCipher(false, algorithm, key, iv);
const unsupported = (name) => () => { throw new Error(`crypto.${name} isn't available on the JSC runtime yet`); };
export const createHash = unsupported("createHash");
export const createHmac = unsupported("createHmac");
export const webcrypto = { getRandomValues, randomUUID };
export default { randomBytes, randomFillSync, randomUUID, randomInt, getRandomValues, createCipheriv, createDecipheriv, createHash, createHmac, webcrypto };
