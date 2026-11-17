// A child's stdio pipes as small Node streams: what node:child_process users
// call on them (data/end/close/error, setEncoding, pause/resume, pipe,
// async iteration; write/end with callbacks and drain). Every app bundle has
// child_process (the SDK uses it), so this stays smaller than a stream polyfill.
import { EventEmitter } from "events";
import { Buffer } from "buffer";
import { errnos, errnoError } from "./spawn-common.js";

const n = globalThis.__node;
const nextTick = (fn, ...args) => queueMicrotask(() => fn(...args));

// The read end of a pipe from the child.
export class FdReadable extends EventEmitter {
	constructor(fd) {
		super();
		this.fd = fd;
		this.readable = true;
		this.destroyed = false;
		this.readableEnded = false;
		this.readableFlowing = null;
		this._queue = [];
		this._eof = false;
		this._decode = null;
		this._watch = n.watchFd(fd, (value) => this._onRead(value));
		// A 'data' listener starts the flow, as in Node.
		this.on("newListener", (event) => {
			if (event === "data" && this.readableFlowing !== false) nextTick(() => this.resume());
		});
	}
	_onRead(value) {
		if (this.destroyed) return;
		if (value === null) this._eof = true;
		else if (typeof value === "number") return this.destroy(errnoError(value, "read"));
		else this._queue.push(Buffer.from(value.buffer, value.byteOffset, value.byteLength));
		this._flow();
	}
	_flow() {
		while (this.readableFlowing && this._queue.length && !this.destroyed) this._emitData(this._queue.shift());
		if (this._eof && !this._queue.length && this.readableFlowing && !this.readableEnded && !this.destroyed) {
			const rest = this._decode ? this._decode(null) : "";
			if (rest) this.emit("data", rest);
			this.readableEnded = true;
			this.readable = false;
			this.emit("end");
			this.destroy();
		}
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
		} else {
			this._decode = (chunk) => (chunk ? chunk.toString(e) : "");
		}
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
		this.on("data", (chunk) => destination.write(chunk));
		if (options.end !== false) this.on("end", () => destination.end());
		return destination;
	}
	destroy(error) {
		if (this.destroyed) return this;
		this.destroyed = true;
		this.readable = false;
		n.unwatchFd(this._watch, true);
		nextTick(() => {
			if (error) this.emit("error", error);
			this.emit("close");
		});
		return this;
	}
	ref() {
		if (!this.destroyed) n.refFd(this._watch, true);
		return this;
	}
	unref() {
		if (!this.destroyed) n.refFd(this._watch, false);
		return this;
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
}

const toBuffer = (chunk, encoding) =>
	typeof chunk === "string" ? Buffer.from(chunk, encoding || "utf8")
	: Buffer.isBuffer(chunk) ? chunk
	: ArrayBuffer.isView(chunk) ? Buffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength)
	: Buffer.from(chunk);

// The write end of a pipe to the child: nonblocking writes, queued while the
// pipe is full.
export class FdWritable extends EventEmitter {
	constructor(fd) {
		super();
		this.fd = fd;
		this.writable = true;
		this.destroyed = false;
		this.writableEnded = false;
		this.writableFinished = false;
		this.writableHighWaterMark = 16384;
		this.bytesWritten = 0;
		this._queue = [];
		this._pending = 0;
		this._wait = 0;
		this._needDrain = false;
	}
	get writableLength() {
		return this._pending;
	}
	write(chunk, encoding, callback) {
		if (typeof encoding === "function") [callback, encoding] = [encoding, undefined];
		if (this.writableEnded || this.destroyed) {
			const error = new Error(this.destroyed ? "Cannot call write after a stream was destroyed" : "write after end");
			error.code = this.destroyed ? "ERR_STREAM_DESTROYED" : "ERR_STREAM_WRITE_AFTER_END";
			nextTick(() => { if (callback) callback(error); this.emit("error", error); });
			return false;
		}
		const bytes = toBuffer(chunk, encoding);
		this._queue.push({ bytes, offset: 0, callback });
		this._pending += bytes.length;
		// As in Node: the answer counts this chunk as buffered, however much
		// of it the kernel then takes at once.
		const ok = this._pending < this.writableHighWaterMark;
		if (!ok) this._needDrain = true;
		this._flush();
		return ok;
	}
	end(chunk, encoding, callback) {
		if (typeof chunk === "function") [callback, chunk] = [chunk, undefined];
		else if (typeof encoding === "function") [callback, encoding] = [encoding, undefined];
		if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
		if (callback) this.once("finish", callback);
		if (!this.writableEnded) {
			this.writableEnded = true;
			this._flush();
		}
		return this;
	}
	_flush() {
		if (this._wait || this.destroyed) return;
		while (this._queue.length) {
			const item = this._queue[0];
			const r = item.bytes.length > item.offset ? n.writeFd(this.fd, item.bytes, item.offset) : 0;
			if (r === -errnos.EAGAIN) {
				this._wait = n.whenWritable(this.fd, () => {
					this._wait = 0;
					this._flush();
				});
				return;
			}
			if (r < 0) return void this.destroy(errnoError(r, "write"));
			item.offset += r;
			this._pending -= r;
			this.bytesWritten += r;
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
			this._close();
			nextTick(() => {
				this.emit("finish");
				this.destroy();
			});
		}
	}
	_close() {
		if (this._wait) n.cancelWritable(this._wait);
		this._wait = 0;
		if (this.fd >= 0) n.fs.close(this.fd);
		this.fd = -1;
	}
	destroy(error) {
		if (this.destroyed) return this;
		this.destroyed = true;
		this.writable = false;
		this._close();
		const dropped = this._queue.splice(0);
		nextTick(() => {
			for (const item of dropped) if (item.callback) item.callback(error ?? null);
			if (error) this.emit("error", error);
			this.emit("close");
		});
		return this;
	}
	cork() {}
	uncork() {}
	setDefaultEncoding() {
		return this;
	}
}
