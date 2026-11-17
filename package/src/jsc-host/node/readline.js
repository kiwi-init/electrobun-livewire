// node:readline for line input from a stream (a child's stdout, a pipe): lines
// end at \n, \r\n or a lone \r, as in Node for input that isn't a terminal.
import { EventEmitter } from "events";

export class Interface extends EventEmitter {
	constructor(input, output, completer, terminal) {
		super();
		const options = input && typeof input.on === "function" ? { input, output, completer, terminal } : (input ?? {});
		this.input = options.input;
		this.output = options.output;
		this.terminal = false;
		this.line = "";
		this.closed = false;
		this._prompt = options.prompt ?? "> ";
		this._decoder = new TextDecoder();
		this._pending = "";
		this._afterReturn = false;
		this._questions = [];
		this._onData = (chunk) => this._take(typeof chunk === "string" ? chunk : this._decoder.decode(chunk, { stream: true }));
		this._onEnd = () => {
			this._take(this._decoder.decode());
			if (this._pending) this._emitLine(this._pending);
			this._pending = "";
			this.close();
		};
		if (this.input) {
			this.input.on("data", this._onData);
			this.input.on("end", this._onEnd);
			this.input.resume?.();
		}
	}
	_take(text) {
		let start = 0;
		for (let i = 0; i < text.length; i++) {
			const c = text[i];
			if (c === "\n" && this._afterReturn) {
				this._afterReturn = false;
				start = i + 1;
				continue;
			}
			this._afterReturn = false;
			if (c !== "\r" && c !== "\n") continue;
			const line = this._pending + text.slice(start, i);
			this._pending = "";
			start = i + 1;
			this._afterReturn = c === "\r";
			this._emitLine(line);
			if (this.closed) return;
		}
		this._pending += text.slice(start);
	}
	_emitLine(line) {
		const answer = this._questions.shift();
		if (answer) answer(line);
		else this.emit("line", line);
	}
	setPrompt(prompt) { this._prompt = prompt; }
	getPrompt() { return this._prompt; }
	prompt() { this.output?.write(this._prompt); }
	question(query, options, callback) {
		if (typeof options === "function") callback = options;
		this.output?.write(query);
		this._questions.push(callback);
	}
	write(data) { this._take(String(data)); }
	pause() { this.input?.pause?.(); this.emit("pause"); return this; }
	resume() { this.input?.resume?.(); this.emit("resume"); return this; }
	close() {
		if (this.closed) return;
		this.closed = true;
		this.input?.off?.("data", this._onData);
		this.input?.off?.("end", this._onEnd);
		this.input?.pause?.();
		this.emit("close");
	}
	[Symbol.asyncIterator]() {
		const queue = [], waiting = [];
		let done = this.closed;
		const onLine = (line) => (waiting.length ? waiting.shift()({ value: line, done: false }) : queue.push(line));
		const onClose = () => {
			done = true;
			while (waiting.length) waiting.shift()({ value: undefined, done: true });
		};
		this.on("line", onLine);
		this.on("close", onClose);
		const stop = () => {
			this.off("line", onLine);
			this.off("close", onClose);
			this.close();
			return Promise.resolve({ value: undefined, done: true });
		};
		return {
			next: () => queue.length ? Promise.resolve({ value: queue.shift(), done: false })
				: done ? Promise.resolve({ value: undefined, done: true })
				: new Promise((resolve) => waiting.push(resolve)),
			return: stop,
			[Symbol.asyncIterator]() { return this; },
		};
	}
}

export const createInterface = (input, output, completer, terminal) => new Interface(input, output, completer, terminal);

const ok = (callback) => { if (callback) queueMicrotask(callback); return true; };
export const clearLine = (stream, dir, callback) => { stream?.write(dir < 0 ? "\x1b[1K" : dir > 0 ? "\x1b[0K" : "\x1b[2K"); return ok(callback); };
export const clearScreenDown = (stream, callback) => { stream?.write("\x1b[0J"); return ok(callback); };
export const cursorTo = (stream, x, y, callback) => {
	if (typeof y === "function") [y, callback] = [undefined, y];
	stream?.write(y === undefined ? `\x1b[${x + 1}G` : `\x1b[${y + 1};${x + 1}H`);
	return ok(callback);
};
export const moveCursor = (stream, dx, dy, callback) => {
	let out = "";
	if (dx < 0) out += `\x1b[${-dx}D`; else if (dx > 0) out += `\x1b[${dx}C`;
	if (dy < 0) out += `\x1b[${-dy}A`; else if (dy > 0) out += `\x1b[${dy}B`;
	if (out) stream?.write(out);
	return ok(callback);
};
export const emitKeypressEvents = () => {};

export default { Interface, createInterface, clearLine, clearScreenDown, cursorTo, moveCursor, emitKeypressEvents };
