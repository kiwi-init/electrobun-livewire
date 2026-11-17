// Globals an app bundle gets on the Livewire JSC runtime, before its own
// code: Buffer, and the slice of Bun's API the SDK uses.
import { Buffer } from "buffer";
import { readFileSync, existsSync, statSync } from "./fs.js";
import { getRandomValues, randomUUID } from "./crypto.js";
import { spawn, spawnSync, which } from "./bun-spawn.js";
import { kill } from "./spawn-common.js";
import { listen, connect } from "./bun-socket.js";
import { ReadableStream, Response } from "./web-streams.js";
import { Worker, structuredClone } from "./worker.js";
import { subtle, CryptoKey, SubtleCrypto, DOMException } from "./webcrypto.js";

// Defined, not assigned: the package typecheck reads this file (allowJs), and
// an assignment to globalThis.Bun or .Buffer would redeclare those globals' types.
const defineGlobal = (name, value) =>
	Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
defineGlobal("Buffer", Buffer);
if (!("Worker" in globalThis)) defineGlobal("Worker", Worker);
if (!("structuredClone" in globalThis)) defineGlobal("structuredClone", structuredClone);

// Bun's browser buffer fallback predates "base64url" (Node 15.7); Channels
// and the SDK use it for tokens and JWKs. Teach the few entry points it.
if (!Buffer.isEncoding("base64url")) {
	const toBase64 = (s) => String(s).replace(/-/g, "+").replace(/_/g, "/");
	const { from, byteLength, isEncoding } = Buffer;
	const { toString, write } = Buffer.prototype;
	const isUrl = (e) => typeof e === "string" && e.toLowerCase() === "base64url";
	Buffer.isEncoding = (e) => isUrl(e) || isEncoding.call(Buffer, e);
	Buffer.from = function (value, encodingOrOffset, length) {
		return typeof value === "string" && isUrl(encodingOrOffset) ? from.call(Buffer, toBase64(value), "base64") : from.call(Buffer, value, encodingOrOffset, length);
	};
	Buffer.byteLength = (s, e) => (typeof s === "string" && isUrl(e) ? byteLength.call(Buffer, toBase64(s), "base64") : byteLength.call(Buffer, s, e));
	Buffer.prototype.toString = function (encoding, start, end) {
		if (!isUrl(encoding)) return toString.apply(this, arguments);
		return toString.call(this, "base64", start, end).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
	};
	Buffer.prototype.write = function (string, ...rest) {
		const i = rest.findIndex(isUrl);
		if (i < 0) return write.call(this, string, ...rest);
		rest[i] = "base64";
		return write.call(this, toBase64(string), ...rest);
	};
}
globalThis.crypto ??= { getRandomValues, randomUUID, subtle };
if (!("DOMException" in globalThis)) defineGlobal("DOMException", DOMException);
if (!("btoa" in globalThis)) {
	defineGlobal("btoa", (s) => {
		s = String(s);
		if (/[^\x00-\xff]/.test(s)) throw new DOMException("Invalid character", "InvalidCharacterError");
		return Buffer.from(s, "latin1").toString("base64");
	});
	defineGlobal("atob", (s) => {
		s = String(s).replace(/[\t\n\f\r ]/g, "");
		if (s.length % 4 === 0) s = s.replace(/==?$/, "");
		if (s.length % 4 === 1 || /[^A-Za-z0-9+/]/.test(s)) throw new DOMException("The string to be decoded is not correctly encoded.", "InvalidCharacterError");
		return Buffer.from(s, "base64").toString("latin1");
	});
}
if (!("CryptoKey" in globalThis)) defineGlobal("CryptoKey", CryptoKey);
if (!("SubtleCrypto" in globalThis)) defineGlobal("SubtleCrypto", SubtleCrypto);

const mime = {
	html: "text/html", htm: "text/html", js: "text/javascript", mjs: "text/javascript", css: "text/css", json: "application/json",
	svg: "image/svg+xml", png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", webp: "image/webp", ico: "image/x-icon",
	woff: "font/woff", woff2: "font/woff2", ttf: "font/ttf", otf: "font/otf", wasm: "application/wasm", txt: "text/plain", map: "application/json",
	mp3: "audio/mpeg", mp4: "video/mp4", webm: "video/webm", wav: "audio/wav", pdf: "application/pdf", xml: "application/xml",
};
class BunFile {
	constructor(path) {
		this.name = String(path);
		const ext = this.name.split(".").pop().toLowerCase();
		this.type = (mime[ext] ?? "application/octet-stream") + (/^(text\/|application\/(json|javascript|xml))/.test(mime[ext] ?? "") ? ";charset=utf-8" : "");
	}
	get size() { try { return statSync(this.name).size; } catch { return 0; } }
	async exists() { return existsSync(this.name); }
	async text() { return readFileSync(this.name, "utf8"); }
	async json() { return JSON.parse(readFileSync(this.name, "utf8")); }
	async arrayBuffer() { const b = readFileSync(this.name); return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); }
	async bytes() { return new Uint8Array(await this.arrayBuffer()); }
}
// The system JavaScriptCore has no web streams or fetch types; Bun.spawn's
// pipes are ReadableStreams that apps read with new Response(stream).text().
if (typeof globalThis.ReadableStream !== "function") defineGlobal("ReadableStream", ReadableStream);
if (typeof globalThis.Response !== "function") defineGlobal("Response", Response);

const proc = globalThis["process"];
if (proc) {
	proc.kill = kill;
	proc.stdout.fd = 1;
	proc.stderr.fd = 2;
}

const secretOptions = (method, args, withValue) => {
	const [options] = args;
	const fail = (message) => {
		const err = new TypeError(message);
		err.code = "ERR_INVALID_ARG_TYPE";
		return err;
	};
	if (args.length === 0) throw fail(`secrets.${method} requires an options object`);
	if (options === null || typeof options !== "object") throw fail("Expected options to be an object");
	if (typeof options.service !== "string" || typeof options.name !== "string") throw fail("Expected service and name to be strings");
	if (!options.service || !options.name) throw fail("Expected service and name to not be empty");
	if (withValue && typeof options.value !== "string") throw fail("Expected 'value' to be a string");
	return options;
};
const secretsCall = (op, options) =>
	new Promise((resolve, reject) =>
		globalThis.__node.secrets(op, options.service, options.name, options.value, Boolean(options.allowUnrestrictedAccess), (err, result) =>
			err ? reject(err) : resolve(op === 1 ? undefined : result),
		),
	);
const secrets = {
	get: async (...args) => secretsCall(0, secretOptions("get", args, false)),
	set: async (...args) => secretsCall(1, secretOptions("set", args, true)),
	delete: async (...args) => secretsCall(2, secretOptions("delete", args, false)),
};
const zlibBytes = (data) => (typeof data === "string" ? new TextEncoder().encode(data) : data instanceof ArrayBuffer ? new Uint8Array(data) : new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
const zlibCall = (compress, wbits) => (data, options) => {
	try {
		return zlibRaw(compress, wbits, data, options);
	} catch (e) {
		// Bun's own zlib errors carry only the message.
		throw new Error(e.message);
	}
};
const zlibRaw = (compress, wbits, data, options) =>
	globalThis.__node.zlib(compress, options?.windowBits !== undefined ? (wbits < 0 ? -options.windowBits : options.windowBits + (wbits - 15)) : wbits, zlibBytes(data), compress ? (options?.level ?? -1) : undefined);
if (!("Bun" in globalThis)) defineGlobal("Bun", {
	file: (path) => new BunFile(path),
	spawn,
	spawnSync,
	which,
	listen,
	connect,
	secrets,
	gzipSync: zlibCall(true, 31),
	gunzipSync: zlibCall(false, 31),
	deflateSync: zlibCall(true, -15),
	inflateSync: zlibCall(false, -15),
	env: proc?.env ?? {},
	version: "0.0.0-livewire-jsc",
});
