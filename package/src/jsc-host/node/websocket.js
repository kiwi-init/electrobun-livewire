// WebSocket on the system's network stack (NSURLSession on macOS, libsoup on
// Linux, through __node.wsOpen), and ws+unix:// over node:net. Behaviour follows Bun's client, including
// its binaryType "nodebuffer" default and new WebSocket(url, { headers,
// protocols }); conformance/websocket.mjs checks it against a Bun server.
import { Buffer } from "buffer";
import { EventTarget, Event, MessageEvent, CloseEvent, ErrorEvent, DOMException, eventHandlers } from "./web-events.js";
import { Blob } from "./fetch.js";
import { createConnection } from "./net.js";

const n = globalThis.__node;
const encoder = new TextEncoder();
const states = { CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3 };

const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const base64 = (bytes) => Buffer.from(bytes).toString("base64");
const randomBytes = (size) => crypto.getRandomValues(new Uint8Array(size));
const fatalUtf8 = new TextDecoder("utf-8", { fatal: true });

// A WebSocket client over a unix socket, for ws+unix:///path as Bun takes it
// (GET / with Host: localhost): Codex's app-server listens on one. The
// system stacks only speak TCP, so this runs RFC 6455 itself over node:net.
// emit(kind, a, b, c) is the same callback __node.wsOpen takes.
class UnixSocketTransport {
	constructor(path, protocols, headers, emit) {
		this._emit = emit;
		this._state = "connecting"; // then open, closing, closed
		this._buf = Buffer.alloc(0);
		this._message = null; // a fragmented message: { opcode, parts }
		this._closeFrame = null; // the peer's close frame: { code, reason }
		this._sentClose = false;
		this._timer = null;
		this._scheduled = false;
		this._socketGone = false;
		this._protocols = protocols;
		const key = base64(randomBytes(16));
		this._expectAccept = base64(n.crypto.hash("sha1", encoder.encode(key + GUID)));
		const head = ["GET / HTTP/1.1", "Host: localhost", "Upgrade: websocket", "Connection: Upgrade", "Sec-WebSocket-Key: " + key, "Sec-WebSocket-Version: 13"];
		if (protocols.length) head.push("Sec-WebSocket-Protocol: " + protocols.join(", "));
		for (let i = 0; i < headers.length; i += 2) head.push(headers[i] + ": " + headers[i + 1]);
		this._socket = createConnection({ path });
		this._socket.on("connect", () => this._socket.write(head.join("\r\n") + "\r\n\r\n"));
		this._socket.on("data", (data) => this._data(data));
		this._socket.on("error", (error) => this._emit("error", null, error.message));
		this._socket.on("close", () => this._socketClosed());
	}
	send(data) {
		if (this._state !== "open") return;
		const text = typeof data === "string";
		const bytes = text ? encoder.encode(data) : data;
		this._frame(text ? 0x1 : 0x2, bytes, () => this._emit("sent", bytes.length));
	}
	// code 0: a close frame with no code (the peer sees 1005).
	close(code, reason) {
		if (this._state === "connecting") return void this._socket.destroy();
		if (this._state !== "open") return;
		this._state = "closing";
		this._sendClose(code, reason);
		// The peer answers with its own close frame, then ends the connection.
		this._timer = setTimeout(() => this._socket.destroy(), 5000);
	}
	terminate() {
		this._socket.destroy();
	}
	_data(data) {
		this._buf = this._buf.length ? Buffer.concat([this._buf, data]) : data;
		if (this._state === "connecting" && !this._handshake()) return;
		this._schedule();
	}
	// One frame per task, as the native stacks deliver events: code that
	// awaits one message (or open) has its next listener in place in time.
	_schedule() {
		if (this._scheduled) return;
		this._scheduled = true;
		setImmediate(() => {
			this._scheduled = false;
			if (this._nextFrame()) this._schedule();
			else if (this._socketGone) this._finish();
		});
	}
	// The server's 101 answer; false while it's incomplete or when it fails.
	_handshake() {
		const end = this._buf.indexOf("\r\n\r\n");
		if (end < 0) {
			if (this._buf.length > 65536) this._failConnection("the handshake response is too large");
			return false;
		}
		const [status, ...lines] = this._buf.subarray(0, end).toString("latin1").split("\r\n");
		this._buf = this._buf.subarray(end + 4);
		const fields = new Map();
		for (const line of lines) {
			const colon = line.indexOf(":");
			if (colon > 0) fields.set(line.slice(0, colon).trim().toLowerCase(), line.slice(colon + 1).trim());
		}
		const code = /^HTTP\/1\.1 (\d{3})/.exec(status)?.[1];
		if (code !== "101") return void this._failConnection("Expected 101 status code, got " + (code ?? "no status"));
		if ((fields.get("upgrade") ?? "").toLowerCase() !== "websocket" || fields.get("sec-websocket-accept") !== this._expectAccept)
			return void this._failConnection("invalid handshake response");
		const protocol = fields.get("sec-websocket-protocol") ?? "";
		if (protocol && !this._protocols.includes(protocol)) return void this._failConnection("the server chose a protocol that wasn't offered");
		this._state = "open";
		this._emit("open", protocol);
		return true;
	}
	// Handles the next whole frame in the buffer; false when there's none.
	_nextFrame() {
		if (this._state !== "open" && this._state !== "closing") return false;
		const b = this._buf;
		if (b.length < 2) return false;
		if (b[0] & 0x70) return void this._protocolError(1002, "reserved bits set"), false;
		if (b[1] & 0x80) return void this._protocolError(1002, "a masked frame from the server"), false;
		let length = b[1] & 0x7f, at = 2;
		if (length === 126) {
			if (b.length < 4) return false;
			length = (b[2] << 8) | b[3];
			at = 4;
		} else if (length === 127) {
			if (b.length < 10) return false;
			const view = new DataView(b.buffer, b.byteOffset + 2, 8);
			length = view.getUint32(0) * 2 ** 32 + view.getUint32(4);
			at = 10;
		}
		if (b.length < at + length) return false;
		const payload = Uint8Array.prototype.slice.call(b, at, at + length);
		this._buf = b.subarray(at + length);
		this._onFrame(!!(b[0] & 0x80), b[0] & 0x0f, payload);
		return true;
	}
	_onFrame(fin, opcode, payload) {
		if (opcode >= 0x8) {
			if (!fin || payload.length > 125) return void this._protocolError(1002, "an invalid control frame");
			if (opcode === 0x9) return void this._frame(0xa, payload);
			if (opcode === 0xa) return;
			if (opcode !== 0x8) return void this._protocolError(1002, "an unknown opcode");
			const code = payload.length >= 2 ? (payload[0] << 8) | payload[1] : 1005;
			let reason = "";
			try {
				reason = fatalUtf8.decode(payload.subarray(2));
			} catch {
				return void this._protocolError(1007, "a close reason that isn't UTF-8");
			}
			this._closeFrame = { code, reason };
			if (!this._sentClose) this._sendClose(code === 1005 ? 0 : code, "");
			this._state = "closing";
			this._socket.end();
			return;
		}
		if (opcode === 0x0) {
			if (!this._message) return void this._protocolError(1002, "a continuation with no message");
			this._message.parts.push(payload);
		} else if (opcode === 0x1 || opcode === 0x2) {
			if (this._message) return void this._protocolError(1002, "a new message inside a fragmented one");
			this._message = { opcode, parts: [payload] };
		} else return void this._protocolError(1002, "an unknown opcode");
		if (!fin) return;
		const { opcode: kind, parts } = this._message;
		this._message = null;
		const bytes = parts.length === 1 ? parts[0] : Uint8Array.from(Buffer.concat(parts));
		if (kind === 0x2) return void this._emit("message", bytes);
		try {
			this._emit("message", fatalUtf8.decode(bytes));
		} catch {
			this._protocolError(1007, "a text message that isn't UTF-8");
		}
	}
	_frame(opcode, bytes, sent) {
		const length = bytes.length;
		const headLength = length < 126 ? 2 : length < 65536 ? 4 : 10;
		const frame = new Uint8Array(headLength + 4 + length);
		frame[0] = 0x80 | opcode;
		if (length < 126) frame[1] = 0x80 | length;
		else if (length < 65536) {
			frame[1] = 0x80 | 126;
			frame[2] = length >> 8;
			frame[3] = length & 0xff;
		} else {
			frame[1] = 0x80 | 127;
			const view = new DataView(frame.buffer, 2, 8);
			view.setUint32(0, Math.floor(length / 2 ** 32));
			view.setUint32(4, length >>> 0);
		}
		const mask = randomBytes(4);
		frame.set(mask, headLength);
		for (let i = 0; i < length; i++) frame[headLength + 4 + i] = bytes[i] ^ mask[i & 3];
		this._socket.write(frame, sent);
	}
	_sendClose(code, reason) {
		if (this._sentClose) return;
		this._sentClose = true;
		if (!code) return void this._frame(0x8, new Uint8Array(0));
		const why = encoder.encode(reason);
		const payload = new Uint8Array(2 + why.length);
		payload[0] = code >> 8;
		payload[1] = code & 0xff;
		payload.set(why, 2);
		this._frame(0x8, payload);
	}
	_failConnection(why) {
		this._emit("error", null, why);
		this._socket.destroy();
	}
	_protocolError(code, why) {
		this._emit("error", null, "the server sent " + why);
		this._sendClose(code, "");
		this._state = "closing";
		this._socket.end();
		this._timer = setTimeout(() => this._socket.destroy(), 1000);
	}
	// The connection is gone; frames that came before it are still handled
	// first (the peer's close frame among them).
	_socketClosed() {
		this._socketGone = true;
		if (!this._scheduled) this._finish();
	}
	_finish() {
		if (this._state === "closed") return;
		this._state = "closed";
		if (this._timer) clearTimeout(this._timer);
		const clean = this._closeFrame && this._sentClose;
		this._emit("close", clean ? this._closeFrame.code : 1006, clean ? this._closeFrame.reason : "", !!clean);
	}
}


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
		if (target.protocol !== "ws:" && target.protocol !== "wss:" && target.protocol !== "ws+unix:")
			throw new DOMException(`Only ws://, wss:// and ws+unix:// are supported here, not ${target.protocol}//`, "SyntaxError");
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
		const emit = (kind, a, b, c) => this._event(kind, a, b, c);
		if (target.protocol === "ws+unix:") {
			const unix = new UnixSocketTransport(decodeURIComponent(target.host + target.pathname), protocols, headers, emit);
			this._transport = { send: (data) => unix.send(data), close: (code, why) => (code === 1006 ? unix.terminate() : unix.close(code, why)) };
		} else {
			const id = n.wsOpen(this._url, protocols, headers, emit);
			this._transport = { send: (data) => n.wsSend(id, data), close: (code, why) => n.wsClose(id, code, why) };
		}
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
			this._transport.send(data);
			this._buffered += encoder.encode(data).length;
			return;
		}
		const bytes =
			data instanceof ArrayBuffer ? new Uint8Array(data)
			: ArrayBuffer.isView(data) ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
			: data instanceof Blob ? data._bytes
			: encoder.encode(String(data));
		this._transport.send(bytes);
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
		this._transport.close(this._abandoned ? 1000 : code ?? 0, why);
	}
	terminate() {
		if (this._state === states.CLOSED) return;
		this._state = states.CLOSING;
		this._transport.close(1006, "");
	}
}
Object.assign(WebSocket, states);
Object.assign(WebSocket.prototype, states);
eventHandlers(WebSocket.prototype, "open", "message", "error", "close");
