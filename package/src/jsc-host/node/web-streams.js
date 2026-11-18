// The slice of web streams the system JavaScriptCore lacks and Bun.spawn's
// pipes and fetch's bodies need: a ReadableStream fed by a controller
// (readers, async iteration). Installed only where the engine lacks it.
export class ReadableStream {
	constructor(source = {}) {
		this._source = source;
		this._queue = [];
		this._state = "readable";
		this._error = undefined;
		this._waiters = [];
		this._reader = null;
		this._closed = new Promise((resolve, reject) => (this._settle = { resolve, reject }));
		this._closed.catch(() => {});
		const controller = {
			enqueue: (chunk) => {
				if (this._state !== "readable") return;
				const waiter = this._waiters.shift();
				if (waiter) waiter.resolve({ value: chunk, done: false });
				else this._queue.push(chunk);
			},
			close: () => {
				if (this._state !== "readable") return;
				this._state = "closed";
				this._settle.resolve();
				for (const waiter of this._waiters.splice(0)) waiter.resolve({ value: undefined, done: true });
			},
			error: (error) => {
				if (this._state !== "readable") return;
				this._state = "errored";
				this._error = error;
				this._settle.reject(error);
				for (const waiter of this._waiters.splice(0)) waiter.reject(error);
			},
			get desiredSize() {
				return 1;
			},
		};
		this._controller = controller;
		if (source.start) source.start(controller);
	}
	get locked() {
		return this._reader !== null;
	}
	_read() {
		if (this._queue.length) return Promise.resolve({ value: this._queue.shift(), done: false });
		if (this._state === "closed") return Promise.resolve({ value: undefined, done: true });
		if (this._state === "errored") return Promise.reject(this._error);
		const pending = new Promise((resolve, reject) => this._waiters.push({ resolve, reject }));
		if (this._source.pull) this._source.pull(this._controller);
		return pending;
	}
	cancel(reason) {
		if (this._state === "readable") {
			this._state = "closed";
			this._queue = [];
			this._settle.resolve();
			for (const waiter of this._waiters.splice(0)) waiter.resolve({ value: undefined, done: true });
			if (this._source.cancel) this._source.cancel(reason);
		}
		return Promise.resolve();
	}
	getReader() {
		if (this._reader) throw new TypeError("ReadableStream is locked");
		const stream = this;
		const reader = {
			read: () => stream._read(),
			releaseLock: () => {
				if (stream._reader === reader) stream._reader = null;
			},
			cancel: (reason) => stream.cancel(reason),
			get closed() {
				return stream._closed;
			},
		};
		this._reader = reader;
		return reader;
	}
	async *values() {
		const reader = this.getReader();
		try {
			while (true) {
				const { value, done } = await reader.read();
				if (done) return;
				yield value;
			}
		} finally {
			reader.releaseLock();
		}
	}
	[Symbol.asyncIterator]() {
		return this.values();
	}
}

// Piping, writable and transform streams, and (de)compression streams on
// zlib: what Blob.stream().pipeThrough(new CompressionStream(...)) needs.
ReadableStream.prototype.pipeTo = async function (dest, options = {}) {
	const reader = this.getReader();
	const writer = dest.getWriter();
	try {
		while (true) {
			const { value, done } = await reader.read();
			if (done) break;
			await writer.write(value);
		}
		if (!options.preventClose) await writer.close();
	} catch (error) {
		if (!options.preventAbort) await writer.abort(error).catch(() => {});
		if (!options.preventCancel) await this.cancel(error).catch(() => {});
		throw error;
	} finally {
		reader.releaseLock();
		writer.releaseLock();
	}
};
ReadableStream.prototype.pipeThrough = function (pair, options) {
	this.pipeTo(pair.writable, options).catch(() => {});
	return pair.readable;
};

export class WritableStream {
	constructor(sink = {}) {
		this._sink = sink;
		this._state = "writable";
		this._error = undefined;
		this._writer = null;
		this._closed = new Promise((resolve, reject) => (this._settle = { resolve, reject }));
		this._closed.catch(() => {});
		this._controller = { error: (error) => this._fail(error) };
		this._chain = Promise.resolve().then(() => sink.start?.(this._controller));
		this._chain.catch((error) => this._fail(error));
	}
	get locked() {
		return this._writer !== null;
	}
	_fail(error) {
		if (this._state === "errored" || this._state === "closed") return;
		this._state = "errored";
		this._error = error;
		this._settle.reject(error);
	}
	_run(step) {
		const done = this._chain.then(() => {
			if (this._state === "errored") throw this._error;
			return step();
		});
		this._chain = done.catch((error) => this._fail(error));
		return done;
	}
	_write(chunk) {
		if (this._state !== "writable") return Promise.reject(this._error ?? new TypeError("The stream is closing or closed"));
		return this._run(() => this._sink.write?.(chunk, this._controller)).then(() => undefined);
	}
	_close() {
		if (this._state !== "writable") return Promise.reject(this._error ?? new TypeError("The stream is closing or closed"));
		this._state = "closing";
		return this._run(async () => {
			await this._sink.close?.();
			this._state = "closed";
			this._settle.resolve();
		});
	}
	abort(reason) {
		if (this._state === "closed" || this._state === "errored") return Promise.resolve();
		this._fail(reason);
		return Promise.resolve(this._sink.abort?.(reason)).then(() => undefined);
	}
	close() {
		if (this._writer) return Promise.reject(new TypeError("WritableStream is locked"));
		return this._close();
	}
	getWriter() {
		if (this._writer) throw new TypeError("WritableStream is locked");
		const stream = this;
		const writer = {
			write: (chunk) => stream._write(chunk),
			close: () => stream._close(),
			abort: (reason) => stream.abort(reason),
			releaseLock: () => {
				if (stream._writer === writer) stream._writer = null;
			},
			get closed() {
				return stream._closed;
			},
			get ready() {
				return Promise.resolve();
			},
			get desiredSize() {
				return stream._state === "writable" ? 1 : 0;
			},
		};
		this._writer = writer;
		return writer;
	}
}

export class TransformStream {
	constructor(transformer = {}) {
		let readable;
		this.readable = new ReadableStream({
			start: (c) => void (readable = c),
			cancel: (reason) => void this.writable.abort(reason),
		});
		const controller = {
			enqueue: (chunk) => readable.enqueue(chunk),
			error: (error) => {
				readable.error(error);
				this.writable._fail(error);
			},
			terminate: () => readable.close(),
			get desiredSize() {
				return 1;
			},
		};
		const started = Promise.resolve().then(() => transformer.start?.(controller));
		this.writable = new WritableStream({
			write: async (chunk) => {
				await started;
				try {
					if (transformer.transform) await transformer.transform(chunk, controller);
					else controller.enqueue(chunk);
				} catch (error) {
					readable.error(error);
					throw error;
				}
			},
			close: async () => {
				await started;
				try {
					await transformer.flush?.(controller);
				} catch (error) {
					readable.error(error);
					throw error;
				}
				readable.close();
			},
			abort: (reason) => readable.error(reason),
		});
	}
}

// One-shot: the input is collected and (de)compressed when it ends.
const zlibFormats = { gzip: 31, deflate: 15, "deflate-raw": -15 };
function zlibTransformer(compress, format) {
	const wbits = zlibFormats[format];
	if (wbits === undefined) throw new TypeError("Unsupported compression format: '" + format + "'");
	const chunks = [];
	return {
		transform(chunk) {
			if (chunk instanceof ArrayBuffer) chunks.push(new Uint8Array(chunk.slice(0)));
			else if (ArrayBuffer.isView(chunk)) chunks.push(new Uint8Array(chunk.buffer.slice(chunk.byteOffset, chunk.byteOffset + chunk.byteLength)));
			else throw new TypeError("The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");
		},
		flush(controller) {
			const all = new Uint8Array(chunks.reduce((n, c) => n + c.byteLength, 0));
			let at = 0;
			for (const c of chunks) all.set(c, (at += c.byteLength) - c.byteLength);
			const out = globalThis.__node.zlib(compress, wbits, all, compress ? -1 : undefined);
			const bytes = out instanceof Uint8Array ? out : new Uint8Array(out);
			if (bytes.byteLength) controller.enqueue(bytes);
		},
	};
}
export class CompressionStream extends TransformStream {
	constructor(format) {
		super(zlibTransformer(true, format));
	}
}
export class DecompressionStream extends TransformStream {
	constructor(format) {
		super(zlibTransformer(false, format));
	}
}
