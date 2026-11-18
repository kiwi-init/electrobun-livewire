// WebSocket on the system's network stack (NSURLSession on macOS, libsoup on
// Linux, through __node.wsOpen). Behaviour follows Bun's client, including
// its binaryType "nodebuffer" default and new WebSocket(url, { headers,
// protocols }); conformance/websocket.mjs checks it against a Bun server.
import { Buffer } from "buffer";
import { EventTarget, Event, MessageEvent, CloseEvent, ErrorEvent, DOMException, eventHandlers } from "./web-events.js";
import { Blob } from "./fetch.js";

const n = globalThis.__node;
const encoder = new TextEncoder();
const states = { CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3 };

export class WebSocket extends EventTarget {
	constructor(url, protocolsOrOptions) {
		super();
		let target;
		try {
			target = new URL(String(url));
		} catch {
			throw new DOMException(`Invalid url for WebSocket ${url}`, "SyntaxError");
		}
		if (target.protocol === "http:") target.protocol = "ws:";
		else if (target.protocol === "https:") target.protocol = "wss:";
		if (target.protocol !== "ws:" && target.protocol !== "wss:")
			throw new DOMException(`Only ws:// and wss:// are supported here, not ${target.protocol}//`, "SyntaxError");
		if (target.hash) throw new DOMException("A WebSocket URL can't have a fragment", "SyntaxError");
		const options = protocolsOrOptions && typeof protocolsOrOptions === "object" && !Array.isArray(protocolsOrOptions) ? protocolsOrOptions : { protocols: protocolsOrOptions };
		const protocols = options.protocols === undefined ? [] : typeof options.protocols === "string" ? [options.protocols] : [...options.protocols].map(String);
		if (new Set(protocols).size !== protocols.length) throw new DOMException("Duplicate WebSocket protocols", "SyntaxError");
		const headers = [];
		if (options.headers) {
			const entries = typeof options.headers.entries === "function" ? options.headers.entries() : Object.entries(options.headers);
			for (const [name, value] of entries) headers.push(String(name), String(value));
		}
		this._url = target.href;
		this._origin = target.origin;
		this._state = states.CONNECTING;
		this._protocol = "";
		this._buffered = 0;
		this._binaryType = "nodebuffer";
		this._errored = false;
		this._id = n.wsOpen(this._url, protocols, headers, (kind, a, b, c) => this._event(kind, a, b, c));
	}
	get url() {
		return this._url;
	}
	get readyState() {
		return this._state;
	}
	get protocol() {
		return this._protocol;
	}
	get extensions() {
		return "";
	}
	get bufferedAmount() {
		return this._buffered;
	}
	get binaryType() {
		return this._binaryType;
	}
	set binaryType(type) {
		if (["nodebuffer", "arraybuffer", "blob", "uint8array"].includes(type)) this._binaryType = type;
	}
	_event(kind, a, b, c) {
		switch (kind) {
			case "open":
				if (this._state !== states.CONNECTING) return;
				this._state = states.OPEN;
				this._protocol = a;
				this.dispatchEvent(new Event("open"));
				break;
			case "message":
				if (this._state !== states.OPEN) return;
				this.dispatchEvent(new MessageEvent("message", { data: typeof a === "string" ? a : this._binary(a), origin: this._origin }));
				break;
			case "sent":
				this._buffered = Math.max(0, this._buffered - a);
				break;
			case "error":
				this._fail(b);
				break;
			case "close":
				this._state = states.CLOSED;
				this._buffered = 0;
				if (this._abandoned) [a, b, c] = [1006, "", false];
				if (!c) this._fail(this._abandoned ? "closed before the connection was established" : "");
				this.dispatchEvent(new CloseEvent("close", { code: a, reason: b, wasClean: !!c }));
				break;
		}
	}
	_fail(why) {
		if (this._errored) return;
		this._errored = true;
		const message = `WebSocket connection to '${this._url}' failed${why ? ": " + why : ""}`;
		this.dispatchEvent(new ErrorEvent("error", { message, error: new Error(message) }));
	}
	_binary(bytes) {
		switch (this._binaryType) {
			case "arraybuffer":
				return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
			case "blob":
				return new Blob([bytes]);
			case "uint8array":
				return bytes;
			default:
				return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
		}
	}
	send(data) {
		if (this._state === states.CONNECTING) throw new DOMException("WebSocket is not connected", "InvalidStateError");
		if (this._state !== states.OPEN) return;
		if (typeof data === "string") {
			n.wsSend(this._id, data);
			this._buffered += encoder.encode(data).length;
			return;
		}
		const bytes =
			data instanceof ArrayBuffer ? new Uint8Array(data)
			: ArrayBuffer.isView(data) ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
			: data instanceof Blob ? data._bytes
			: encoder.encode(String(data));
		n.wsSend(this._id, bytes);
		this._buffered += bytes.length;
	}
	close(code, reason) {
		// Codes an endpoint may send (as Bun allows): not 1004-1006 or 1015, which are reserved.
		const sendable = (c) => (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1014) || (c >= 3000 && c <= 4999);
		if (code !== undefined && !sendable(code)) throw new DOMException(`Invalid close code: ${code}`, "InvalidAccessError");
		const why = reason === undefined ? "" : String(reason);
		if (encoder.encode(why).length > 123) throw new DOMException("The close reason must be at most 123 bytes", "SyntaxError");
		if (this._state === states.CLOSING || this._state === states.CLOSED) return;
		// Closing before the handshake ends fails the connection (1006).
		if (this._state === states.CONNECTING) this._abandoned = true;
		this._state = states.CLOSING;
		n.wsClose(this._id, this._abandoned ? 1000 : code ?? 0, why);
	}
	terminate() {
		if (this._state === states.CLOSED) return;
		this._state = states.CLOSING;
		n.wsClose(this._id, 1006, "");
	}
}
Object.assign(WebSocket, states);
Object.assign(WebSocket.prototype, states);
eventHandlers(WebSocket.prototype, "open", "message", "error", "close");
