// fetch on the system's network stack (NSURLSession on macOS, libsoup on
// Linux, through __node.httpRequest), and the types that come with it:
// Headers, Blob, File, Request and Response. Behaviour follows Bun's;
// conformance/fetch.mjs checks it against a local Bun server.
import { ReadableStream } from "./web-streams.js";
import { AbortController } from "./web-events.js";

const n = globalThis.__node;
const encoder = new TextEncoder();
const copy = (view) => new Uint8Array(view.buffer.slice(view.byteOffset, view.byteOffset + view.byteLength));
function concat(chunks) {
	let length = 0;
	for (const c of chunks) length += c.length;
	const all = new Uint8Array(length);
	let at = 0;
	for (const c of chunks) {
		all.set(c, at);
		at += c.length;
	}
	return all;
}

// --- Blob and File ------------------------------------------------------------
export class Blob {
	constructor(parts = [], options = {}) {
		const chunks = [];
		for (const part of parts) {
			if (part instanceof Blob) chunks.push(part._bytes);
			else if (part instanceof ArrayBuffer) chunks.push(new Uint8Array(part.slice(0)));
			else if (ArrayBuffer.isView(part)) chunks.push(copy(part));
			else chunks.push(encoder.encode(String(part)));
		}
		this._bytes = concat(chunks);
		const type = String(options?.type ?? "");
		this._type = /^[\x20-\x7e]*$/.test(type) ? type.toLowerCase() : "";
	}
	get size() {
		return this._bytes.length;
	}
	get type() {
		return this._type;
	}
	slice(start = 0, end = this.size, type = "") {
		const at = (i) => (i < 0 ? Math.max(this.size + i, 0) : Math.min(i, this.size));
		const from = at(start), to = at(end);
		return new Blob([this._bytes.subarray(from, Math.max(from, to))], { type });
	}
	async text() {
		return new TextDecoder().decode(this._bytes);
	}
	async arrayBuffer() {
		return copy(this._bytes).buffer;
	}
	async bytes() {
		return copy(this._bytes);
	}
	stream() {
		const bytes = copy(this._bytes);
		return new ReadableStream({
			start(controller) {
				if (bytes.length) controller.enqueue(bytes);
				controller.close();
			},
		});
	}
	get [Symbol.toStringTag]() {
		return "Blob";
	}
}
export class File extends Blob {
	constructor(parts, name, options = {}) {
		super(parts, options);
		this.name = String(name);
		this.lastModified = options.lastModified ?? Date.now();
	}
	get [Symbol.toStringTag]() {
		return "File";
	}
}

// --- Headers --------------------------------------------------------------------
const token = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
function headerName(name) {
	name = String(name);
	if (!token.test(name)) throw new TypeError(`Invalid header name: "${name}"`);
	return name.toLowerCase();
}
function headerValue(value) {
	value = String(value).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, "");
	if (/[\0\r\n]/.test(value)) throw new TypeError(`Invalid header value: "${value}"`);
	return value;
}
export class Headers {
	constructor(init) {
		this._map = new Map();
		if (init === undefined || init === null) return;
		if (init instanceof Headers) {
			for (const [name, value] of init) this.append(name, value);
		} else if (typeof init[Symbol.iterator] === "function") {
			for (const pair of init) {
				const [name, value, extra] = [...pair];
				if (extra !== undefined || value === undefined) throw new TypeError("Headers constructor: expected name/value pair to be length 2");
				this.append(name, value);
			}
		} else if (typeof init === "object") {
			for (const name of Object.keys(init)) this.append(name, init[name]);
		} else throw new TypeError("Headers constructor: expected an object or iterable");
	}
	append(name, value) {
		name = headerName(name);
		value = headerValue(value);
		const list = this._map.get(name);
		if (list) list.push(value);
		else this._map.set(name, [value]);
	}
	set(name, value) {
		this._map.set(headerName(name), [headerValue(value)]);
	}
	get(name) {
		return this._map.get(headerName(name))?.join(", ") ?? null;
	}
	has(name) {
		return this._map.has(headerName(name));
	}
	delete(name) {
		this._map.delete(headerName(name));
	}
	getSetCookie() {
		return [...(this._map.get("set-cookie") ?? [])];
	}
	getAll(name) {
		return [...(this._map.get(headerName(name)) ?? [])];
	}
	get count() {
		return [...this.keys()].length;
	}
	// Sorted by name, set-cookie last and one entry per cookie, as Bun's.
	*entries() {
		for (const name of [...this._map.keys()].filter((k) => k !== "set-cookie").sort()) yield [name, this._map.get(name).join(", ")];
		for (const value of this._map.get("set-cookie") ?? []) yield ["set-cookie", value];
	}
	*keys() {
		for (const [name] of this.entries()) yield name;
	}
	*values() {
		for (const [, value] of this.entries()) yield value;
	}
	forEach(fn, self) {
		for (const [name, value] of this.entries()) fn.call(self, value, name, this);
	}
	[Symbol.iterator]() {
		return this.entries();
	}
	toJSON() {
		const out = {};
		for (const [name, value] of this.entries()) {
			if (name === "set-cookie") (out[name] ??= []).push(value);
			else out[name] = value;
		}
		return out;
	}
	get [Symbol.toStringTag]() {
		return "Headers";
	}
}

// --- bodies ---------------------------------------------------------------------
// A body is null, bytes (Uint8Array) or a ReadableStream; extract returns it
// and the content type it implies (none for a string, as in Bun).
function extract(body) {
	if (body === null || body === undefined) return [null, null];
	if (typeof body === "string") return [encoder.encode(body), null, true];
	if (body instanceof ArrayBuffer) return [new Uint8Array(body.slice(0)), null];
	if (ArrayBuffer.isView(body)) return [copy(body), null];
	if (body instanceof Blob) return [copy(body._bytes), body.type || null];
	if (body instanceof URLSearchParams) return [encoder.encode(body.toString()), "application/x-www-form-urlencoded;charset=UTF-8"];
	if (body instanceof ReadableStream) return [body, null];
	if (typeof body.getReader === "function") return [body, null];
	return [encoder.encode(String(body)), null];
}
async function drain(stream) {
	const reader = stream.getReader();
	const chunks = [];
	while (true) {
		const { value, done } = await reader.read();
		if (done) break;
		chunks.push(typeof value === "string" ? encoder.encode(value) : value instanceof Uint8Array ? value : new Uint8Array(value.buffer ?? value, value.byteOffset ?? 0, value.byteLength));
	}
	reader.releaseLock();
	return concat(chunks);
}
const streamOf = (bytes) =>
	new ReadableStream({
		start(controller) {
			if (bytes.length) controller.enqueue(bytes);
			controller.close();
		},
	});
// Two streams that each see every chunk of one.
function tee(stream) {
	const reader = stream.getReader();
	const controllers = [];
	const branches = [0, 1].map(() => new ReadableStream({ start: (c) => void controllers.push(c) }));
	(async () => {
		try {
			while (true) {
				const { value, done } = await reader.read();
				if (done) break;
				for (const c of controllers) c.enqueue(value);
			}
			for (const c of controllers) c.close();
		} catch (error) {
			for (const c of controllers) c.error(error);
		}
	})();
	return branches;
}

class Body {
	_setBody(body, headers) {
		const [value, type, text] = extract(body);
		this._body = value;
		this._textBody = !!text;
		this._used = false;
		if (type && !headers.has("content-type")) headers.set("content-type", type);
	}
	get body() {
		if (this._body === null) return null;
		if (!(this._body instanceof ReadableStream) && typeof this._body.getReader !== "function") this._body = streamOf(this._body);
		return this._body;
	}
	get bodyUsed() {
		return this._used;
	}
	async _take() {
		if (this._used) throw new TypeError("Body already used");
		this._used = true;
		if (this._body === null) return new Uint8Array(0);
		if (this._body instanceof Uint8Array) return this._body;
		if (this._body.locked) throw new TypeError("ReadableStream is locked");
		return drain(this._body);
	}
	_cloneBody() {
		if (this._used) throw new TypeError("Body already used");
		if (this._body === null || this._body instanceof Uint8Array) return this._body;
		const [a, b] = tee(this._body);
		this._body = a;
		return b;
	}
	async arrayBuffer() {
		return copy(await this._take()).buffer;
	}
	async bytes() {
		return copy(await this._take());
	}
	async text() {
		return new TextDecoder().decode(await this._take());
	}
	async json() {
		return JSON.parse(await this.text());
	}
	async blob() {
		return new Blob([await this._take()], { type: this.headers.get("content-type") ?? "" });
	}
	async formData() {
		throw new TypeError("formData() isn't supported on the Livewire JSC runtime");
	}
}

// --- Request --------------------------------------------------------------------
const methods = ["DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT", "PATCH"];
export class Request extends Body {
	constructor(input, init = {}) {
		super();
		init = init ?? {};
		const from = input instanceof Request ? input : null;
		this._url = from ? from.url : new URL(String(input)).href;
		const method = init.method !== undefined ? String(init.method) : from ? from.method : "GET";
		this._method = methods.includes(method.toUpperCase()) ? method.toUpperCase() : method;
		this._headers = new Headers(init.headers !== undefined ? init.headers : from?.headers);
		this._redirect = init.redirect ?? from?.redirect ?? "follow";
		if (!["follow", "manual", "error"].includes(this._redirect)) throw new TypeError(`Invalid redirect: "${this._redirect}"`);
		this._signal = init.signal ?? from?.signal ?? new AbortController().signal;
		const body = init.body !== undefined ? init.body : null;
		if (body !== null && (this._method === "GET" || this._method === "HEAD")) throw new TypeError("Request with a GET or HEAD method cannot have a body.");
		if (init.body !== undefined) this._setBody(body, this._headers);
		else if (from) {
			this._body = from._cloneBody();
			this._used = false;
		} else this._setBody(null, this._headers);
	}
	get url() {
		return this._url;
	}
	get method() {
		return this._method;
	}
	get headers() {
		return this._headers;
	}
	get redirect() {
		return this._redirect;
	}
	get signal() {
		return this._signal;
	}
	clone() {
		return new Request(this);
	}
	get [Symbol.toStringTag]() {
		return "Request";
	}
}

// --- Response -------------------------------------------------------------------
const reasons = {
	100: "Continue", 101: "Switching Protocols", 200: "OK", 201: "Created", 202: "Accepted", 203: "Non-Authoritative Information",
	204: "No Content", 205: "Reset Content", 206: "Partial Content", 300: "Multiple Choices", 301: "Moved Permanently", 302: "Found",
	303: "See Other", 304: "Not Modified", 307: "Temporary Redirect", 308: "Permanent Redirect", 400: "Bad Request", 401: "Unauthorized",
	402: "Payment Required", 403: "Forbidden", 404: "Not Found", 405: "Method Not Allowed", 406: "Not Acceptable", 407: "Proxy Authentication Required",
	408: "Request Timeout", 409: "Conflict", 410: "Gone", 411: "Length Required", 412: "Precondition Failed", 413: "Payload Too Large",
	414: "URI Too Long", 415: "Unsupported Media Type", 416: "Range Not Satisfiable", 417: "Expectation Failed", 418: "I'm a Teapot",
	421: "Misdirected Request", 422: "Unprocessable Entity", 423: "Locked", 424: "Failed Dependency", 425: "Too Early", 426: "Upgrade Required",
	428: "Precondition Required", 429: "Too Many Requests", 431: "Request Header Fields Too Large", 451: "Unavailable For Legal Reasons",
	500: "Internal Server Error", 501: "Not Implemented", 502: "Bad Gateway", 503: "Service Unavailable", 504: "Gateway Timeout",
	505: "HTTP Version Not Supported", 506: "Variant Also Negotiates", 507: "Insufficient Storage", 508: "Loop Detected", 510: "Not Extended",
	511: "Network Authentication Required",
};
const internal = Symbol("internal");
export class Response extends Body {
	constructor(body = null, init = {}) {
		super();
		init = init ?? {};
		const status = init.status ?? 200;
		if (init[internal]) Object.assign(this, init[internal]);
		else if (!Number.isInteger(status) || status < 200 || status > 599) throw new RangeError(`The status provided (${status}) must be 200 to 599`);
		this._status = status;
		this._statusText = init.statusText !== undefined ? String(init.statusText) : init[internal] ? reasons[status] ?? "" : "";
		this._headers = init.headers instanceof Headers && init[internal] ? init.headers : new Headers(init.headers);
		this._type ??= "default";
		this._url ??= "";
		this._redirected ??= false;
		this._setBody(body, this._headers);
	}
	get status() {
		return this._status;
	}
	get statusText() {
		return this._statusText;
	}
	get ok() {
		return this._status >= 200 && this._status < 300;
	}
	get headers() {
		return this._headers;
	}
	get type() {
		return this._type;
	}
	get url() {
		return this._url;
	}
	get redirected() {
		return this._redirected;
	}
	clone() {
		const body = this._cloneBody();
		return new Response(body, {
			status: this._status,
			statusText: this._statusText,
			headers: new Headers(this._headers),
			[internal]: { _type: this._type, _url: this._url, _redirected: this._redirected },
		});
	}
	static json(data, init = {}) {
		const headers = new Headers(init?.headers);
		if (!headers.has("content-type")) headers.set("content-type", "application/json;charset=utf-8");
		return new Response(JSON.stringify(data), { ...init, headers });
	}
	static redirect(url, status = 302) {
		if (![301, 302, 303, 307, 308].includes(status)) throw new RangeError(`Invalid redirect status: ${status}`);
		return new Response(null, { status, headers: { location: new URL(String(url)).href } });
	}
	static error() {
		return new Response(null, { status: 0, [internal]: { _type: "error" } });
	}
	get [Symbol.toStringTag]() {
		return "Response";
	}
}

// --- fetch ----------------------------------------------------------------------
// Failures as Bun reports them: a TypeError with Node's error code.
const failures = [
	["ERR_NETWORK", (url, message) => message || "fetch failed"],
	["ECONNREFUSED", () => "Unable to connect. Is the computer able to access the url?"],
	["ENOTFOUND", (url) => "getaddrinfo ENOTFOUND " + new URL(url).hostname],
	["ETIMEDOUT", () => "The operation timed out."],
	["ECONNRESET", () => "The socket connection was closed unexpectedly. For more information, pass `verbose: true` in the second argument to fetch()"],
	["UNABLE_TO_VERIFY_LEAF_SIGNATURE", (url, message) => message || "unable to verify the first certificate"],
	["UnexpectedRedirect", () => "fetch: redirect mode is set to error"],
	["ERR_INVALID_URL", (url) => "fetch() URL is invalid: " + url],
	["CERT_HAS_EXPIRED", (url, message) => message || "certificate has expired"],
];
function fetchError(kind, message, url) {
	const [code, text] = failures[kind] ?? failures[0];
	const error = new TypeError(text(url, message));
	error.code = code;
	error.path = url;
	error.errno = 0;
	return error;
}

export async function fetch(input, init) {
	const request = new Request(input, init);
	const signal = request.signal;
	signal.throwIfAborted();
	const protocol = new URL(request.url).protocol;
	if (protocol !== "http:" && protocol !== "https:") throw new TypeError(`fetch: unsupported protocol "${protocol}"`);
	const body = request._body === null ? null : await request._take();
	signal.throwIfAborted();
	const headers = [];
	for (const [name, value] of request.headers) headers.push(name, value);
	// Bun's form; libsoup sends none of its own, and some APIs (GitHub's) refuse a request without one.
	if (!request.headers.has("user-agent")) headers.push("user-agent", "Bun/" + (globalThis.Bun?.version ?? "0"));
	// macOS's CFNetwork labels an untyped body as a form; Bun sends no type, so
	// say what it is instead (text as the Fetch spec does, else bytes).
	if (body !== null && !request.headers.has("content-type") && process.platform === "darwin")
		headers.push("content-type", request._textBody ? "text/plain;charset=UTF-8" : "application/octet-stream");
	const redirect = { follow: 0, manual: 1, error: 2 }[request.redirect];
	return new Promise((resolve, reject) => {
		let controller = null;
		let response = null;
		let done = false;
		const stream = new ReadableStream({
			start: (c) => void (controller = c),
			cancel: () => {
				if (!done) n.httpCancel(id);
			},
		});
		const fail = (error) => {
			if (response) controller.error(error);
			else reject(error);
		};
		const onAbort = () => {
			if (done) return;
			done = true;
			n.httpCancel(id);
			fail(signal.reason);
		};
		const id = n.httpRequest(request.url, request.method, headers, body, redirect, (kind, a, b, c) => {
			if (kind === "response") {
				const list = new Headers();
				for (let i = 0; i + 1 < b.length; i += 2) list.append(b[i], b[i + 1]);
				const empty = request.method === "HEAD" || a === 204 || a === 304;
				response = new Response(empty ? null : stream, { status: a, headers: list, [internal]: { _url: c || request.url, _redirected: !!b.redirected } });
				resolve(response);
			} else if (kind === "data") {
				if (!done) controller.enqueue(a);
			} else {
				signal.removeEventListener("abort", onAbort);
				if (done) return;
				done = true;
				if (kind === "end") controller.close();
				else if (kind === "error") fail(fetchError(a, b, request.url));
			}
		});
		signal.addEventListener("abort", onAbort, { once: true });
	});
}
fetch.preconnect = () => {};
