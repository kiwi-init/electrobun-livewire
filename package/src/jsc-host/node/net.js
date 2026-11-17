// node:net on the Livewire JSC runtime. Milestone 1 covers what the SDK uses:
// a readable Socket over an existing file descriptor (the core's host-message
// wakeup pipe). Connecting and listening land with milestone 3.
import { EventEmitter } from "events";
import { Buffer } from "buffer";
import { errnoCode } from "./errors.js";

const n = globalThis.__node;

export class Socket extends EventEmitter {
	constructor(options = {}) {
		super();
		this.destroyed = false;
		this.readable = options.readable !== false;
		this.writable = false;
		this._fd = typeof options.fd === "number" ? options.fd : -1;
		this._watch = 0;
		if (this._fd >= 0 && this.readable) {
			this._watch = n.watchFd(this._fd, (value) => this._onRead(value));
		}
	}
	_onRead(value) {
		if (value === null) {
			this.emit("end");
			this.destroy();
		} else if (typeof value === "number") {
			const error = new Error(errnoCode(value) + ": read failed");
			error.code = errnoCode(value);
			error.errno = value;
			this.destroy(error);
		} else {
			this.emit("data", Buffer.from(value.buffer, value.byteOffset, value.byteLength));
		}
	}
	connect() {
		throw new Error("net.Socket.connect isn't available on the JSC runtime yet");
	}
	write() {
		throw new Error("net.Socket.write isn't available on the JSC runtime yet");
	}
	destroy(error) {
		if (this.destroyed) return this;
		this.destroyed = true;
		if (this._watch) n.unwatchFd(this._watch, true);
		this._watch = 0;
		queueMicrotask(() => {
			if (error) this.emit("error", error);
			this.emit("close", !!error);
		});
		return this;
	}
	end() { return this.destroy(); }
	ref() { if (this._watch) n.refFd(this._watch, true); return this; }
	unref() { if (this._watch) n.refFd(this._watch, false); return this; }
	setNoDelay() { return this; }
	setKeepAlive() { return this; }
}
export function createConnection() {
	throw new Error("net.createConnection isn't available on the JSC runtime yet");
}
export const connect = createConnection;
export function createServer() {
	throw new Error("net.createServer isn't available on the JSC runtime yet");
}
export const isIP = (s) => (/^\d{1,3}(\.\d{1,3}){3}$/.test(s) ? 4 : s.includes(":") ? 6 : 0);
export default { Socket, createConnection, connect, createServer, isIP };
