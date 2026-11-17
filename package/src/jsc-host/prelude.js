// Livewire JSC runtime prelude: the globals a Node/Bun-style main process
// expects, on top of the natives in __rt (runtime.c). Plain ES2020, evaluated
// once before the app's bundle.
(function (rt) {
	"use strict";
	const g = globalThis;

	// --- timers --------------------------------------------------------------
	class Timeout {
		constructor(id, repeat) { this._id = id; this._repeat = repeat; this._ref = true; }
		ref() { if (!this._ref) { this._ref = true; rt.refTimer(this._id, true); } return this; }
		unref() { if (this._ref) { this._ref = false; rt.refTimer(this._id, false); } return this; }
		hasRef() { return this._ref; }
		[Symbol.toPrimitive]() { return this._id; }
	}
	const timerId = (t) => (t instanceof Timeout ? t._id : Number(t) || 0);
	function schedule(fn, ms, args, repeat) {
		if (typeof fn !== "function") throw new TypeError("The callback must be a function");
		const id = rt.setTimer(args.length ? () => fn(...args) : fn, Number(ms) || 0, repeat);
		return new Timeout(id, repeat);
	}
	g.setTimeout = (fn, ms, ...args) => schedule(fn, ms, args, false);
	g.setInterval = (fn, ms, ...args) => schedule(fn, ms, args, true);
	g.setImmediate = (fn, ...args) => schedule(fn, 0, args, false);
	g.clearTimeout = g.clearInterval = g.clearImmediate = (t) => { if (t != null) rt.clearTimer(timerId(t)); };
	if (typeof g.queueMicrotask !== "function") {
		const resolved = Promise.resolve();
		g.queueMicrotask = (fn) => { resolved.then(fn).catch((e) => rt.dispatchEvent("uncaughtException", e)); };
	}

	// --- events on process ---------------------------------------------------
	const listeners = new Map();
	const signals = new Set(["SIGINT", "SIGTERM", "SIGHUP"]);
	const watched = new Set();
	function on(name, fn) {
		if (!listeners.has(name)) listeners.set(name, []);
		listeners.get(name).push(fn);
		if (signals.has(name) && !watched.has(name)) { watched.add(name); rt.watchSignal(name); }
		return process;
	}
	function off(name, fn) {
		const list = listeners.get(name);
		if (list) { const i = list.indexOf(fn); if (i >= 0) list.splice(i, 1); }
		return process;
	}
	// Called from C. Returns true when a listener handled the event.
	rt.dispatchEvent = function (name, ...args) {
		const list = listeners.get(name);
		if (!list || list.length === 0) {
			if (signals.has(name)) process.exit(130);
			return false;
		}
		for (const fn of list.slice()) {
			try { fn(...args); } catch (e) { if (name !== "uncaughtException") rt.dispatchEvent("uncaughtException", e); else throw e; }
		}
		return true;
	};

	// --- process ---------------------------------------------------------------
	let exitCode = 0;
	const process = {
		argv: rt.argv,
		env: rt.env,
		pid: rt.pid,
		platform: "darwin",
		arch: "arm64",
		versions: { livewire_jsc: "0" },
		release: { name: "livewire-jsc" },
		cwd: () => rt.env.PWD || "/",
		exit(code) { if (code !== undefined) exitCode = code; rt.exit(exitCode); },
		get exitCode() { return exitCode; },
		set exitCode(code) { exitCode = code; rt.setExitCode(code); },
		nextTick(fn, ...args) { queueMicrotask(() => fn(...args)); },
		hrtime: Object.assign((prev) => {
			const ns = BigInt(Math.round(rt.now() * 1e6));
			const s = Number(ns / 1000000000n), n = Number(ns % 1000000000n);
			if (!prev) return [s, n];
			let ds = s - prev[0], dn = n - prev[1];
			if (dn < 0) { ds -= 1; dn += 1e9; }
			return [ds, dn];
		}, { bigint: () => BigInt(Math.round(rt.now() * 1e6)) }),
		on, addListener: on, once(name, fn) { const w = (...a) => { off(name, w); fn(...a); }; return on(name, w); },
		off, removeListener: off, removeAllListeners(name) { if (name) listeners.delete(name); else listeners.clear(); return process; },
		emit(name, ...args) { return rt.dispatchEvent(name, ...args); },
		listenerCount(name) { return (listeners.get(name) || []).length; },
		stdout: { write(s) { rt.write(1, String(s)); return true; }, isTTY: false },
		stderr: { write(s) { rt.write(2, String(s)); return true; }, isTTY: false },
	};
	g.process = process;
	g.performance = g.performance || { now: () => rt.now(), timeOrigin: Date.now() - rt.now() };

	// --- console ----------------------------------------------------------------
	function inspect(value, depth, seen) {
		if (typeof value === "string") return depth === 0 ? value : JSON.stringify(value);
		if (typeof value === "bigint") return value + "n";
		if (typeof value === "function") return "[Function " + (value.name || "anonymous") + "]";
		if (typeof value === "symbol") return value.toString();
		if (value === null || typeof value !== "object") return String(value);
		if (value instanceof Error) return value.stack ? value.name + ": " + value.message + "\n" + value.stack : String(value);
		if (seen.has(value)) return "[Circular]";
		if (depth > 2) return Array.isArray(value) ? "[Array]" : "[Object]";
		seen.add(value);
		let out;
		if (Array.isArray(value)) out = "[ " + value.map((v) => inspect(v, depth + 1, seen)).join(", ") + " ]";
		else if (ArrayBuffer.isView(value)) out = value.constructor.name + "(" + value.length + ") [ " + Array.prototype.slice.call(value, 0, 32).join(", ") + (value.length > 32 ? ", ..." : "") + " ]";
		else if (value instanceof Map) out = "Map(" + value.size + ") { " + [...value].map(([k, v]) => inspect(k, depth + 1, seen) + " => " + inspect(v, depth + 1, seen)).join(", ") + " }";
		else if (value instanceof Set) out = "Set(" + value.size + ") { " + [...value].map((v) => inspect(v, depth + 1, seen)).join(", ") + " }";
		else {
			const keys = Object.keys(value);
			out = keys.length ? "{ " + keys.map((k) => k + ": " + inspect(value[k], depth + 1, seen)).join(", ") + " }" : "{}";
		}
		seen.delete(value);
		return out;
	}
	function format(args) {
		return args.map((a) => inspect(a, 0, new Set())).join(" ") + "\n";
	}
	const consoleObject = {
		log: (...a) => rt.write(1, format(a)),
		info: (...a) => rt.write(1, format(a)),
		debug: (...a) => rt.write(1, format(a)),
		warn: (...a) => rt.write(2, format(a)),
		error: (...a) => rt.write(2, format(a)),
		trace: (...a) => rt.write(2, format(a) + new Error().stack + "\n"),
		assert: (ok, ...a) => { if (!ok) rt.write(2, "Assertion failed: " + format(a)); },
		time() {}, timeEnd() {}, table: (...a) => rt.write(1, format(a)),
	};
	g.console = consoleObject;

	// --- TextEncoder / TextDecoder (UTF-8) -----------------------------------
	if (typeof g.TextEncoder !== "function") {
		g.TextEncoder = class TextEncoder {
			get encoding() { return "utf-8"; }
			encode(input = "") {
				const s = String(input);
				const out = new Uint8Array(s.length * 3);
				let o = 0;
				for (let i = 0; i < s.length; i++) {
					let c = s.charCodeAt(i);
					if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
						const d = s.charCodeAt(i + 1);
						if (d >= 0xdc00 && d <= 0xdfff) { c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00); i++; }
						else c = 0xfffd;
					} else if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
					if (c < 0x80) out[o++] = c;
					else if (c < 0x800) { out[o++] = 0xc0 | (c >> 6); out[o++] = 0x80 | (c & 63); }
					else if (c < 0x10000) { out[o++] = 0xe0 | (c >> 12); out[o++] = 0x80 | ((c >> 6) & 63); out[o++] = 0x80 | (c & 63); }
					else { out[o++] = 0xf0 | (c >> 18); out[o++] = 0x80 | ((c >> 12) & 63); out[o++] = 0x80 | ((c >> 6) & 63); out[o++] = 0x80 | (c & 63); }
				}
				return out.slice(0, o);
			}
			encodeInto(input, dest) { const b = this.encode(input); const n = Math.min(b.length, dest.length); dest.set(b.subarray(0, n)); return { read: input.length, written: n }; }
		};
	}
	if (typeof g.TextDecoder !== "function") {
		g.TextDecoder = class TextDecoder {
			constructor(label = "utf-8") {
				const l = String(label).toLowerCase();
				if (l !== "utf-8" && l !== "utf8") throw new RangeError("TextDecoder: only utf-8 is supported");
			}
			get encoding() { return "utf-8"; }
			decode(input) {
				if (input == null) return "";
				const b = input instanceof Uint8Array ? input : ArrayBuffer.isView(input) ? new Uint8Array(input.buffer, input.byteOffset, input.byteLength) : new Uint8Array(input);
				let out = "", chunk = [];
				for (let i = 0; i < b.length; ) {
					const c = b[i];
					let cp = 0xfffd, n = 1;
					if (c < 0x80) cp = c;
					else if (c >= 0xc2 && c < 0xe0 && i + 1 < b.length && (b[i + 1] & 0xc0) === 0x80) { cp = ((c & 31) << 6) | (b[i + 1] & 63); n = 2; }
					else if (c >= 0xe0 && c < 0xf0 && i + 2 < b.length && (b[i + 1] & 0xc0) === 0x80 && (b[i + 2] & 0xc0) === 0x80) { cp = ((c & 15) << 12) | ((b[i + 1] & 63) << 6) | (b[i + 2] & 63); n = 3; if (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd; }
					else if (c >= 0xf0 && c < 0xf5 && i + 3 < b.length && (b[i + 1] & 0xc0) === 0x80 && (b[i + 2] & 0xc0) === 0x80 && (b[i + 3] & 0xc0) === 0x80) { cp = ((c & 7) << 18) | ((b[i + 1] & 63) << 12) | ((b[i + 2] & 63) << 6) | (b[i + 3] & 63); n = 4; if (cp < 0x10000 || cp > 0x10ffff) cp = 0xfffd; }
					i += n;
					if (cp > 0xffff) { cp -= 0x10000; chunk.push(0xd800 + (cp >> 10), 0xdc00 + (cp & 1023)); }
					else chunk.push(cp);
					if (chunk.length > 8192) { out += String.fromCharCode.apply(null, chunk); chunk = []; }
				}
				return out + String.fromCharCode.apply(null, chunk);
			}
		};
	}
	g.global = g;
	g.self = g.self || g;
})(globalThis.__rt);
