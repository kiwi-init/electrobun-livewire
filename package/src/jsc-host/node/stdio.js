// process.stdin and Bun.stdin on fd 0, and real TTY facts for stdout/stderr.
// stdin is opened lazily, on its first read: until then, nothing watches fd 0
// and it doesn't keep the process alive, as in Node.
import { EventEmitter } from "events";
import { Buffer } from "buffer";
import { FdReadable } from "./child-streams.js";
import { ReadableStream } from "./web-streams.js";

const n = globalThis.__node;
const ttySize = (fd) => (n.ttySize ? n.ttySize(fd) : undefined);

let reader = null;
const open = () => (reader ??= new FdReadable(0));

// Reads that open fd 0; the rest are plain EventEmitter calls on the stdin
// object until then, and forward once it's open.
const starts = new Set(["data", "readable", "end", "close"]);
export class Stdin extends EventEmitter {
	constructor() {
		super();
		this.fd = 0;
		this.isTTY = n.isatty(0);
		this.isRaw = false;
		this.on("newListener", (event) => {
			if (starts.has(event)) this._open();
		});
	}
	_open() {
		if (this._reader) return this._reader;
		const r = (this._reader = open());
		for (const event of ["data", "end", "close", "error"]) r.on(event, (...args) => this.emit(event, ...args));
		if (this._encoding) r.setEncoding(this._encoding);
		return r;
	}
	setEncoding(encoding) {
		this._encoding = encoding;
		if (this._reader) this._reader.setEncoding(encoding);
		return this;
	}
	resume() { this._open().resume(); return this; }
	pause() { if (this._reader) this._reader.pause(); return this; }
	isPaused() { return this._reader ? this._reader.isPaused() : false; }
	read() { return null; }
	pipe(destination, options) { return this._open().pipe(destination, options); }
	ref() { if (this._reader) this._reader.ref(); return this; }
	unref() { if (this._reader) this._reader.unref(); return this; }
	destroy(error) { if (this._reader) this._reader.destroy(error); return this; }
	setRawMode(mode) {
		if (n.setRawMode && this.isTTY) n.setRawMode(0, !!mode);
		this.isRaw = !!mode;
		return this;
	}
	[Symbol.asyncIterator]() { return this._open()[Symbol.asyncIterator](); }
}

// Bun.stdin: a BunFile-like view of fd 0.
const chunks = async () => {
	const parts = [];
	for await (const chunk of open()) parts.push(chunk);
	return Buffer.concat(parts);
};
export const bunStdin = {
	name: "/dev/stdin",
	type: "application/octet-stream",
	stream() {
		const r = open();
		return new ReadableStream({
			start(controller) {
				r.on("data", (chunk) => controller.enqueue(new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength)));
				r.on("end", () => controller.close());
				r.on("error", (e) => controller.error(e));
				r.resume();
			},
			cancel() { r.destroy(); },
		});
	},
	async text() { return (await chunks()).toString("utf8"); },
	async json() { return JSON.parse((await chunks()).toString("utf8")); },
	async bytes() { const b = await chunks(); return new Uint8Array(b.buffer, b.byteOffset, b.byteLength); },
	async arrayBuffer() { const b = await chunks(); return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); },
};

// isTTY, columns/rows and the tty.WriteStream methods on process.stdout/stderr.
export function decorateOutput(stream, fd) {
	stream.fd = fd;
	stream.isTTY = n.isatty(fd);
	if (!stream.isTTY) return stream;
	Object.defineProperty(stream, "columns", { get: () => ttySize(fd)?.[0] ?? 80, configurable: true });
	Object.defineProperty(stream, "rows", { get: () => ttySize(fd)?.[1] ?? 24, configurable: true });
	stream.getWindowSize = () => [stream.columns, stream.rows];
	stream.hasColors = (count = 16) => count <= 256 && !("NO_COLOR" in (globalThis.process?.env ?? {}));
	stream.getColorDepth = () => (stream.hasColors(256) ? 8 : 1);
	stream.on ??= () => stream;
	stream.once ??= () => stream;
	stream.off ??= () => stream;
	stream.removeListener ??= () => stream;
	return stream;
}
