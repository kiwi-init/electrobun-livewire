// node:net on the Livewire JSC runtime: unix and TCP sockets over the natives
// in node.c (nonblocking connect/listen/accept, watchFd reads, writeFd
// writes), and a Socket over an fd the app already has (the SDK's host-message
// wakeup pipe). Behaviour follows Bun's; conformance/net.mjs checks it.
import { EventEmitter } from "events";
import { Buffer } from "buffer";
import { errnoCode } from "./errors.js";
import { errnos } from "./spawn-common.js";

const n = globalThis.__node;
const nextTick = (fn, ...args) => queueMicrotask(() => fn(...args));
const HIGH_WATER = 16384;

const ipv4 = /^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$/;
export const isIPv4 = (s) => typeof s === "string" && ipv4.test(s);
export function isIPv6(s) {
	if (typeof s !== "string" || !s.includes(":")) return false;
	const [addr] = s.split("%");
	const v4 = addr.match(/:(\d+\.\d+\.\d+\.\d+)$/);
	if (v4 && !isIPv4(v4[1])) return false;
	const head = v4 ? addr.slice(0, -v4[1].length) + "0:0" : addr;
	const halves = head.split("::");
	if (halves.length > 2) return false;
	const groups = (part) => (part === "" ? [] : part.split(":"));
	const all = halves.flatMap(groups);
	if (!all.every((g) => /^[0-9a-fA-F]{1,4}$/.test(g))) return false;
	return halves.length === 2 ? all.length < 8 : all.length === 8;
}
export const isIP = (s) => (isIPv4(s) ? 4 : isIPv6(s) ? 6 : 0);

function socketError(errno, syscall, address, port) {
	const code = errnoCode(errno);
	const where = address === undefined ? "" : " " + address + (port === undefined ? "" : ":" + port);
	const error = new Error(syscall + " " + code + where);
	error.errno = errno;
	error.code = code;
	error.syscall = syscall;
	if (address !== undefined) error.address = address;
	if (port !== undefined) error.port = port;
	return error;
}
const toBuffer = (chunk, encoding) =>
	typeof chunk === "string" ? Buffer.from(chunk, encoding || "utf8")
	: Buffer.isBuffer(chunk) ? chunk
	: ArrayBuffer.isView(chunk) ? Buffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength)
	: Buffer.from(chunk);

export class Socket extends EventEmitter {
	constructor(options = {}) {
		super();
		this.allowHalfOpen = !!options.allowHalfOpen;
		this.connecting = false;
		this.pending = true;
		this.destroyed = false;
		this.readable = false;
		this.writable = false;
		this.readableEnded = false;
		this.writableEnded = false;
		this.writableFinished = false;
		this.readableFlowing = null;
		this.bytesRead = 0;
		this.timeout = undefined;
		this._written = 0;
		this._fd = -1;
		this._watch = 0;
		this._wait = 0;
		this._queue = [];
		this._queued = 0;
		this._needDrain = false;
		this._inbox = [];
		this._eof = false;
		this._readSide = false;
		this._writeSide = false;
		this._decode = null;
		this._timer = null;
		this._refed = true;
		this._names = null;
		// A 'data' listener starts the flow, as in Node.
		this.on("newListener", (event) => {
			if (event === "data" && this.readableFlowing !== false) nextTick(() => this.resume());
		});
		if (typeof options.fd === "number" && options.fd >= 0) {
			this.pending = false;
			this._attach(options.fd, options.readable !== false, options.writable === true);
		}
	}

	// --- connecting ---------------------------------------------------------
	connect(...args) {
		const [options, callback] = normalizeConnect(args);
		if (callback) this.once("connect", callback);
		if (options.timeout) this.setTimeout(options.timeout);
		this.connecting = true;
		if (options.path !== undefined) {
			this._connectTo([String(options.path)], undefined);
			return this;
		}
		const port = Number(options.port);
		const host = options.host || "localhost";
		if (isIP(host)) this._connectTo([host], port);
		else {
			n.lookup(host, (code, list) => {
				if (this.destroyed) return;
				if (code) {
					const error = new Error("getaddrinfo " + code + " " + host);
					error.code = code;
					error.syscall = "getaddrinfo";
					error.hostname = host;
					return void this.destroy(error);
				}
				// IPv4 first, as Node's default "ipv4first" order for a name.
				const sorted = list.filter((a) => a.family === 4).concat(list.filter((a) => a.family === 6));
				this._connectTo(sorted.map((a) => a.address), port);
			});
		}
		return this;
	}
	_connectTo(addresses, port) {
		const [address, ...rest] = addresses;
		const failed = (errno) => (rest.length ? this._connectTo(rest, port) : this.destroy(socketError(errno, "connect", address, port)));
		const r = n.sockConnect(address, port);
		if (typeof r === "number") return void failed(r);
		const [fd, pending] = r;
		this._fd = fd;
		if (!pending) return void nextTick(() => this._connected());
		this._wait = n.whenWritable(fd, () => {
			this._wait = 0;
			if (this.destroyed) return;
			const errno = n.sockError(fd);
			if (!errno) return void this._connected();
			n.fs.close(fd);
			this._fd = -1;
			failed(errno);
		});
		if (!this._refed) n.refWait(this._wait, false);
	}
	_connected() {
		if (this.destroyed) return;
		this.connecting = false;
		this.pending = false;
		this._attach(this._fd, true, true);
		this.emit("connect");
		this.emit("ready");
		this._flush();
	}
	_attach(fd, readable, writable) {
		this._fd = fd;
		this._readSide = this.readable = readable;
		this._writeSide = this.writable = writable;
		if (readable) {
			this._watch = n.watchFd(fd, (value) => this._onRead(value));
			if (!this._refed) n.refFd(this._watch, false);
		}
		if (this.timeout) this._arm();
	}

	// --- reading ------------------------------------------------------------
	_onRead(value) {
		if (this.destroyed) return;
		if (value === null) this._eof = true;
		else if (typeof value === "number") return void this.destroy(socketError(value, "read"));
		else {
			this.bytesRead += value.length;
			this._arm();
			this._inbox.push(Buffer.from(value.buffer, value.byteOffset, value.byteLength));
		}
		this._flow();
	}
	_flow() {
		while (this.readableFlowing && this._inbox.length && !this.destroyed) this._emitData(this._inbox.shift());
		if (!this._eof || this._inbox.length || this.readableEnded || this.destroyed) return;
		const rest = this._decode ? this._decode(null) : "";
		if (rest) this.emit("data", rest);
		this.readableEnded = true;
		this.readable = false;
		this.emit("end");
		if (!this.allowHalfOpen && !this.writableEnded) this.end();
		this._maybeDone();
	}
	_emitData(chunk) {
		if (!this._decode) return this.emit("data", chunk);
		const text = this._decode(chunk);
		if (text) this.emit("data", text);
	}
	setEncoding(encoding) {
		const e = String(encoding || "utf8").toLowerCase();
		if (e === "utf8" || e === "utf-8") {
			const decoder = new TextDecoder("utf-8");
			this._decode = (chunk) => (chunk ? decoder.decode(chunk, { stream: true }) : decoder.decode());
		} else this._decode = (chunk) => (chunk ? chunk.toString(e) : "");
		return this;
	}
	resume() {
		this.readableFlowing = true;
		this._flow();
		return this;
	}
	pause() {
		this.readableFlowing = false;
		return this;
	}
	isPaused() {
		return this.readableFlowing === false;
	}
	pipe(destination, options = {}) {
		this.on("data", (chunk) => {
			if (destination.write(chunk) === false && destination.once) {
				this.pause();
				destination.once("drain", () => this.resume());
			}
		});
		if (options.end !== false) this.on("end", () => destination.end());
		return destination;
	}
	async *[Symbol.asyncIterator]() {
		const chunks = [];
		let done = false, failure = null, wake = null;
		const signal = () => { if (wake) { wake(); wake = null; } };
		this.on("data", (chunk) => { chunks.push(chunk); signal(); });
		this.on("end", () => { done = true; signal(); });
		this.on("error", (error) => { failure = error; signal(); });
		this.on("close", () => { done = true; signal(); });
		while (true) {
			if (chunks.length) yield chunks.shift();
			else if (failure) throw failure;
			else if (done) return;
			else await new Promise((resolve) => (wake = resolve));
		}
	}

	// --- writing ------------------------------------------------------------
	get bytesWritten() {
		return this._written + this._queued;
	}
	get writableLength() {
		return this._queued;
	}
	get readyState() {
		if (this.connecting) return "opening";
		if (this.readable && this.writable) return "open";
		if (this.readable) return "readOnly";
		if (this.writable) return "writeOnly";
		return "closed";
	}
	write(chunk, encoding, callback) {
		if (typeof encoding === "function") [callback, encoding] = [encoding, undefined];
		if (this.destroyed) {
			const error = new Error("Cannot call write after a stream was destroyed");
			error.code = "ERR_STREAM_DESTROYED";
			if (callback) nextTick(callback, error);
			return false;
		}
		if (this.writableEnded) {
			const error = new Error("write after end");
			error.code = "ERR_STREAM_WRITE_AFTER_END";
			if (callback) nextTick(callback, error);
			this.destroy(error);
			return false;
		}
		const bytes = toBuffer(chunk, encoding);
		this._queue.push({ bytes, offset: 0, callback });
		this._queued += bytes.length;
		// As in Node: the answer counts this chunk as buffered, however much
		// of it the kernel then takes at once.
		const ok = this._queued < HIGH_WATER;
		if (!ok) this._needDrain = true;
		if (!this.connecting) this._flush();
		return ok;
	}
	end(chunk, encoding, callback) {
		if (typeof chunk === "function") [callback, chunk] = [chunk, undefined];
		else if (typeof encoding === "function") [callback, encoding] = [encoding, undefined];
		if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
		if (callback) this.once("finish", callback);
		if (!this.writableEnded) {
			this.writableEnded = true;
			if (!this.connecting) this._flush();
		}
		return this;
	}
	_flush() {
		if (this._wait || this.destroyed || this._fd < 0) return;
		while (this._queue.length) {
			const item = this._queue[0];
			const r = item.bytes.length > item.offset ? n.writeFd(this._fd, item.bytes, item.offset) : 0;
			if (r === -errnos.EAGAIN) {
				this._wait = n.whenWritable(this._fd, () => {
					this._wait = 0;
					this._flush();
				});
				if (!this._refed) n.refWait(this._wait, false);
				return;
			}
			if (r < 0) return void this.destroy(socketError(r, "write"));
			item.offset += r;
			this._queued -= r;
			this._written += r;
			if (r > 0) this._arm();
			if (item.offset < item.bytes.length) continue;
			this._queue.shift();
			if (item.callback) nextTick(item.callback, null);
		}
		if (this._needDrain) {
			this._needDrain = false;
			nextTick(() => this.emit("drain"));
		}
		if (this.writableEnded && !this.writableFinished) {
			this.writableFinished = true;
			this.writable = false;
			if (this._writeSide) n.sockShutdown(this._fd);
			nextTick(() => {
				this.emit("finish");
				this._maybeDone();
			});
		}
	}
	_maybeDone() {
		const readDone = !this._readSide || this.readableEnded;
		const writeDone = !this._writeSide || this.writableFinished;
		if (readDone && writeDone) this.destroy();
	}

	// --- lifetime -----------------------------------------------------------
	destroy(error) {
		if (this.destroyed) return this;
		this.destroyed = true;
		this.readable = false;
		this.writable = false;
		this.connecting = false;
		if (this._timer) clearTimeout(this._timer);
		this._timer = null;
		if (this._wait) n.cancelWritable(this._wait);
		this._wait = 0;
		if (this._watch) n.unwatchFd(this._watch, true);
		else if (this._fd >= 0) n.fs.close(this._fd);
		this._watch = 0;
		this._fd = -1;
		const dropped = this._queue.splice(0);
		this._queued = 0;
		nextTick(() => {
			for (const item of dropped) if (item.callback) item.callback(error ?? null);
			if (error) this.emit("error", error);
			this.emit("close", !!error);
		});
		return this;
	}
	destroySoon() {
		this.end();
	}
	resetAndDestroy() {
		return this.destroy();
	}
	setTimeout(ms, callback) {
		this.timeout = ms;
		if (callback) (ms === 0 ? this.off("timeout", callback) : this.once("timeout", callback));
		if (this._timer) clearTimeout(this._timer);
		this._timer = null;
		this._arm();
		return this;
	}
	_arm() {
		if (!this.timeout || this.destroyed) return;
		if (this._timer) clearTimeout(this._timer);
		this._timer = setTimeout(() => {
			this._timer = null;
			this.emit("timeout");
		}, this.timeout);
		if (!this._refed) this._timer.unref();
	}
	setNoDelay(on = true) {
		if (this._fd >= 0 && !this._names?.path) n.sockOption(this._fd, "nodelay", !!on, 0);
		return this;
	}
	setKeepAlive(on = false, delay = 0) {
		if (this._fd >= 0) n.sockOption(this._fd, "keepalive", !!on, Math.floor(delay / 1000));
		return this;
	}
	ref() {
		this._setRef(true);
		return this;
	}
	unref() {
		this._setRef(false);
		return this;
	}
	_setRef(on) {
		if (this._refed === on) return;
		this._refed = on;
		if (this._watch) n.refFd(this._watch, on);
		if (this._wait) n.refWait(this._wait, on);
		if (this._timer) (on ? this._timer.ref() : this._timer.unref());
	}

	// --- addresses ----------------------------------------------------------
	_name(peer) {
		if (this._fd < 0) return null;
		return n.sockName(this._fd, peer);
	}
	address() {
		const a = this._name(false);
		return a && a.address ? { address: a.address, family: a.family, port: a.port } : {};
	}
	get remoteAddress() { return this._name(true)?.address; }
	get remoteFamily() { return this._name(true)?.family; }
	get remotePort() { return this._name(true)?.port; }
	get localAddress() { return this._name(false)?.address; }
	get localPort() { return this._name(false)?.port; }
	get localFamily() { return this._name(false)?.family; }
}

function normalizeConnect(args) {
	let options = {};
	let callback = typeof args[args.length - 1] === "function" ? args.pop() : undefined;
	const [first, second] = args;
	if (first !== null && typeof first === "object") options = { ...first };
	else if (typeof first === "string" && !/^\d+$/.test(first)) options = { path: first };
	else {
		options = { port: first };
		if (typeof second === "string") options.host = second;
	}
	return [options, callback];
}

export class Server extends EventEmitter {
	constructor(options, listener) {
		super();
		if (typeof options === "function") [listener, options] = [options, {}];
		options = options || {};
		this.allowHalfOpen = !!options.allowHalfOpen;
		this.listening = false;
		this._fd = -1;
		this._wait = 0;
		this._path = null;
		this._connections = new Set();
		this._refed = true;
		this._closing = false;
		this._closeEmitted = false;
		if (listener) this.on("connection", listener);
	}
	listen(...args) {
		const callback = typeof args[args.length - 1] === "function" ? args.pop() : undefined;
		if (callback) this.once("listening", callback);
		let [first, second, third] = args;
		let options;
		if (first !== null && typeof first === "object") options = first;
		else if (typeof first === "string" && !/^\d+$/.test(first)) options = { path: first, backlog: second };
		else options = { port: first, host: typeof second === "string" ? second : undefined, backlog: typeof second === "number" ? second : third };
		this._closing = false;
		this._closeEmitted = false;
		if (options.path !== undefined) {
			this._bind([String(options.path)], undefined, options.backlog);
			return this;
		}
		const port = Number(options.port) || 0;
		const host = options.host;
		if (!host) this._bind(["::", "0.0.0.0"], port, options.backlog);
		else if (isIP(host)) this._bind([host], port, options.backlog);
		else {
			n.lookup(host, (code, list) => {
				if (code) {
					const error = new Error("getaddrinfo " + code + " " + host);
					error.code = code;
					error.syscall = "getaddrinfo";
					error.hostname = host;
					return void this.emit("error", error);
				}
				this._bind([list.find((a) => a.family === 4)?.address ?? list[0].address], port, options.backlog);
			});
		}
		return this;
	}
	_bind(addresses, port, backlog) {
		const [address, ...rest] = addresses;
		const fd = n.sockListen(address, port, backlog);
		if (fd < 0) {
			// No IPv6 here: listen on IPv4's any-address instead.
			if (rest.length && fd !== -errnos.EADDRINUSE) return void this._bind(rest, port, backlog);
			const error = socketError(fd, "listen", address, port);
			return void nextTick(() => this.emit("error", error));
		}
		this._fd = fd;
		this._path = port === undefined ? address : null;
		this.listening = true;
		this._arm();
		nextTick(() => this.emit("listening"));
	}
	_arm() {
		this._wait = n.whenReadable(this._fd, () => {
			this._wait = 0;
			this._accept();
		});
		if (!this._refed) n.refWait(this._wait, false);
	}
	_accept() {
		if (this._fd < 0) return;
		while (true) {
			const fd = n.sockAccept(this._fd);
			if (fd === -errnos.EAGAIN) break;
			if (fd < 0) {
				if (fd === -errnos.EMFILE || fd === -errnos.ENFILE) this.emit("error", socketError(fd, "accept"));
				break;
			}
			const socket = new Socket({ allowHalfOpen: this.allowHalfOpen });
			socket.server = this;
			socket._attach(fd, true, true);
			socket.pending = false;
			this._connections.add(socket);
			socket.once("close", () => {
				this._connections.delete(socket);
				this._maybeClosed();
			});
			this.emit("connection", socket);
		}
		this._arm();
	}
	close(callback) {
		if (callback) {
			if (this._fd < 0) {
				const error = new Error("Server is not running.");
				error.code = "ERR_SERVER_NOT_RUNNING";
				nextTick(callback, error);
				return this;
			}
			this.once("close", callback);
		}
		if (this._fd >= 0) {
			if (this._wait) n.cancelWritable(this._wait);
			this._wait = 0;
			n.fs.close(this._fd);
			this._fd = -1;
			if (this._path) {
				try {
					n.fs.unlink(this._path);
				} catch {
					// Already gone: nothing to clean up.
				}
			}
		}
		this.listening = false;
		this._closing = true;
		this._maybeClosed();
		return this;
	}
	_maybeClosed() {
		if (!this._closing || this._closeEmitted || this._connections.size) return;
		this._closeEmitted = true;
		nextTick(() => this.emit("close"));
	}
	address() {
		if (this._fd < 0) return null;
		if (this._path) return this._path;
		const a = n.sockName(this._fd, false);
		return { address: a.address, family: a.family, port: a.port };
	}
	getConnections(callback) {
		nextTick(callback, null, this._connections.size);
		return this;
	}
	ref() {
		if (!this._refed) {
			this._refed = true;
			if (this._wait) n.refWait(this._wait, true);
		}
		return this;
	}
	unref() {
		if (this._refed) {
			this._refed = false;
			if (this._wait) n.refWait(this._wait, false);
		}
		return this;
	}
	[Symbol.asyncDispose]() {
		return new Promise((resolve) => this.close(() => resolve()));
	}
}

export const createServer = (options, listener) => new Server(options, listener);
export function createConnection(...args) {
	const first = args[0];
	const socket = new Socket(first !== null && typeof first === "object" ? { allowHalfOpen: first.allowHalfOpen } : {});
	return socket.connect(...args);
}
export const connect = createConnection;
export const Stream = Socket;
export default { Socket, Stream, Server, createServer, createConnection, connect, isIP, isIPv4, isIPv6 };
