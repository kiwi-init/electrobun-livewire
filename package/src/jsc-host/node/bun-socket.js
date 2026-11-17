// Bun.listen and Bun.connect on the Livewire JSC runtime: handler-object
// sockets over unix paths and TCP, as Channels' daemon uses them. As in Bun,
// write() doesn't buffer: it returns the bytes the kernel took, and drain()
// says when to send the rest. conformance/bun-socket.mjs checks the shapes.
import { Buffer } from "buffer";
import { errnoCode } from "./errors.js";
import { errnos } from "./spawn-common.js";

const n = globalThis.__node;
const encoder = new TextEncoder();

function socketError(errno, syscall) {
	const code = errnoCode(errno);
	const error = new Error(code + ": " + syscall + " failed");
	error.errno = errno;
	error.code = code;
	error.syscall = syscall;
	return error;
}
const bytesOf = (data) =>
	typeof data === "string" ? encoder.encode(data)
	: data instanceof ArrayBuffer ? new Uint8Array(data)
	: new Uint8Array(data.buffer, data.byteOffset, data.byteLength);

class Socket {
	constructor(fd, handlers, data) {
		this._fd = fd;
		this._handlers = handlers || {};
		this.data = data;
		this.readyState = 1;
		this._closed = false;
		this._watch = 0;
		this._wait = 0;
		this._timer = null;
		this._timeoutMs = 0;
		this._refed = true;
	}
	_start() {
		this._watch = n.watchFd(this._fd, (value) => this._onRead(value));
		this._handlers.open?.(this);
	}
	_onRead(value) {
		if (this._closed) return;
		if (value === null) {
			this._handlers.end?.(this);
			return void this._close(undefined);
		}
		if (typeof value === "number") {
			const error = socketError(value, "read");
			this._handlers.error?.(this, error);
			return void this._close(error);
		}
		this._arm();
		this._handlers.data?.(this, Buffer.from(value.buffer, value.byteOffset, value.byteLength));
	}
	write(data, byteOffset = 0, byteLength) {
		if (this._closed) return -1;
		let bytes = bytesOf(data);
		if (byteOffset || byteLength !== undefined) bytes = bytes.subarray(byteOffset, byteLength === undefined ? undefined : byteOffset + byteLength);
		if (!bytes.length) return 0;
		const r = n.writeFd(this._fd, bytes, 0);
		if (r === -errnos.EAGAIN) {
			this._awaitDrain();
			return 0;
		}
		if (r < 0) {
			const error = socketError(r, "write");
			this._handlers.error?.(this, error);
			this._close(error);
			return -1;
		}
		this._arm();
		if (r < bytes.length) this._awaitDrain();
		return r;
	}
	_awaitDrain() {
		if (this._wait || this._closed) return;
		this._wait = n.whenWritable(this._fd, () => {
			this._wait = 0;
			if (!this._closed) this._handlers.drain?.(this);
		});
		if (!this._refed) n.refWait(this._wait, false);
	}
	end(data, byteOffset, byteLength) {
		const wrote = data === undefined ? 0 : this.write(data, byteOffset, byteLength);
		if (!this._closed) {
			n.sockShutdown(this._fd);
			this._close(undefined);
		}
		return wrote;
	}
	shutdown(halfClose) {
		if (!this._closed) n.sockShutdown(this._fd);
		void halfClose;
	}
	flush() {}
	terminate() {
		this._close(undefined);
	}
	timeout(seconds) {
		this._timeoutMs = Math.max(0, Number(seconds) || 0) * 1000;
		this._arm();
	}
	_arm() {
		if (this._timer) clearTimeout(this._timer);
		this._timer = null;
		if (!this._timeoutMs || this._closed) return;
		this._timer = setTimeout(() => {
			this._timer = null;
			this._handlers.timeout?.(this);
		}, this._timeoutMs);
		if (!this._refed) this._timer.unref();
	}
	ref() {
		this._setRef(true);
	}
	unref() {
		this._setRef(false);
	}
	_setRef(on) {
		if (this._refed === on) return;
		this._refed = on;
		if (this._watch) n.refFd(this._watch, on);
		if (this._wait) n.refWait(this._wait, on);
		if (this._timer) (on ? this._timer.ref() : this._timer.unref());
	}
	get remoteAddress() {
		return this._closed ? undefined : n.sockName(this._fd, true)?.address;
	}
	get localPort() {
		return this._closed ? undefined : n.sockName(this._fd, false)?.port;
	}
	get localAddress() {
		return this._closed ? undefined : n.sockName(this._fd, false)?.address;
	}
	get remotePort() {
		return this._closed ? undefined : n.sockName(this._fd, true)?.port;
	}
	_close(error) {
		if (this._closed) return;
		this._closed = true;
		this.readyState = -1;
		if (this._timer) clearTimeout(this._timer);
		this._timer = null;
		if (this._wait) n.cancelWritable(this._wait);
		this._wait = 0;
		if (this._watch) n.unwatchFd(this._watch, true);
		else n.fs.close(this._fd);
		this._watch = 0;
		queueMicrotask(() => this._handlers.close?.(this, error));
	}
}

// A literal address for a hostname, without waiting: Bun.listen is synchronous.
const literal = (hostname) => (hostname === "localhost" ? "127.0.0.1" : hostname || "0.0.0.0");

export function listen(options) {
	const handlers = options.socket || {};
	const unix = options.unix;
	const host = unix === undefined ? literal(options.hostname) : String(unix);
	const port = unix === undefined ? Number(options.port) || 0 : undefined;
	const fd = n.sockListen(host, port, options.backlog);
	if (fd < 0) throw socketError(fd, "listen");
	const sockets = new Set();
	let wait = 0, stopped = false, refed = true;
	const arm = () => {
		wait = n.whenReadable(fd, accept);
		if (!refed) n.refWait(wait, false);
	};
	function accept() {
		wait = 0;
		if (stopped) return;
		while (true) {
			const conn = n.sockAccept(fd);
			if (conn < 0) break;
			const socket = new Socket(conn, handlers, options.data);
			sockets.add(socket);
			const close = handlers.close;
			socket._handlers = { ...handlers, close: (s, e) => { sockets.delete(s); close?.(s, e); } };
			socket._start();
		}
		arm();
	}
	arm();
	const address = unix === undefined ? n.sockName(fd, false) : null;
	return {
		unix: unix === undefined ? undefined : String(unix),
		hostname: unix === undefined ? host : undefined,
		port: address?.port,
		data: options.data,
		stop(closeActive = false) {
			if (stopped) return;
			stopped = true;
			if (wait) n.cancelWritable(wait);
			wait = 0;
			n.fs.close(fd);
			if (unix !== undefined) {
				try {
					n.fs.unlink(String(unix));
				} catch {
					// Already gone: nothing to clean up.
				}
			}
			if (closeActive) for (const socket of [...sockets]) socket.terminate();
		},
		ref() {
			refed = true;
			if (wait) n.refWait(wait, true);
		},
		unref() {
			refed = false;
			if (wait) n.refWait(wait, false);
		},
		reload(next) {
			Object.assign(handlers, next?.socket || {});
		},
	};
}

export function connect(options) {
	const handlers = options.socket || {};
	return new Promise((resolve, reject) => {
		const fail = (errno) => {
			const error = socketError(errno, "connect");
			handlers.connectError?.(undefined, error);
			reject(error);
		};
		const attempt = (addresses, port) => {
			const [address, ...rest] = addresses;
			const failed = (errno) => (rest.length ? attempt(rest, port) : fail(errno));
			const r = n.sockConnect(address, port);
			if (typeof r === "number") return void failed(r);
			const [fd, pending] = r;
			const opened = () => {
				const socket = new Socket(fd, handlers, options.data);
				socket._start();
				resolve(socket);
			};
			if (!pending) return void queueMicrotask(opened);
			n.whenWritable(fd, () => {
				const errno = n.sockError(fd);
				if (!errno) return void opened();
				n.fs.close(fd);
				failed(errno);
			});
		};
		if (options.unix !== undefined) return void attempt([String(options.unix)], undefined);
		const port = Number(options.port);
		const hostname = options.hostname || "localhost";
		if (/^[\d.]+$/.test(hostname) || hostname.includes(":")) return void attempt([hostname], port);
		n.lookup(hostname, (code, list) => {
			if (code) {
				const error = new Error("getaddrinfo " + code + " " + hostname);
				error.code = code;
				handlers.connectError?.(undefined, error);
				return void reject(error);
			}
			attempt(list.filter((a) => a.family === 4).concat(list.filter((a) => a.family === 6)).map((a) => a.address), port);
		});
	});
}
