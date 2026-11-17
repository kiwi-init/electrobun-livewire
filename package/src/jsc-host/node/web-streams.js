// The slice of web streams Bun.spawn's pipes need on the system
// JavaScriptCore, which has no ReadableStream or Response: a ReadableStream
// fed by a controller (readers, async iteration) and a Response that reads a
// body to text, JSON or bytes. Installed only where the engine lacks them.
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

const encoder = new TextEncoder();
async function bodyBytes(body) {
	if (body === null || body === undefined) return new Uint8Array(0);
	if (typeof body === "string") return encoder.encode(body);
	if (body instanceof ArrayBuffer) return new Uint8Array(body);
	if (ArrayBuffer.isView(body)) return new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
	if (typeof body.getReader === "function") {
		const chunks = [];
		let length = 0;
		for await (const chunk of body) {
			const bytes = typeof chunk === "string" ? encoder.encode(chunk) : new Uint8Array(chunk.buffer ?? chunk, chunk.byteOffset ?? 0, chunk.byteLength);
			chunks.push(bytes);
			length += bytes.length;
		}
		const all = new Uint8Array(length);
		let at = 0;
		for (const bytes of chunks) {
			all.set(bytes, at);
			at += bytes.length;
		}
		return all;
	}
	return encoder.encode(String(body));
}
export class Response {
	constructor(body = null, init = {}) {
		this.body = body;
		this.status = init.status ?? 200;
		this.statusText = init.statusText ?? "";
		this.ok = this.status >= 200 && this.status < 300;
		this.bodyUsed = false;
	}
	_take() {
		if (this.bodyUsed) return Promise.reject(new TypeError("Body already used"));
		this.bodyUsed = true;
		return bodyBytes(this.body);
	}
	async bytes() {
		return this._take();
	}
	async arrayBuffer() {
		const bytes = await this._take();
		return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
	}
	async text() {
		return new TextDecoder().decode(await this._take());
	}
	async json() {
		return JSON.parse(await this.text());
	}
}
