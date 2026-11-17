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
	// From JavaScriptCore at a microtask checkpoint: with no listener, an
	// unhandled rejection ends the process with 1, as in Node and Bun.
	rt.onUnhandledRejection = function (promise, reason) {
		if (!rt.dispatchEvent("unhandledRejection", reason, promise)) rt.fatal("Uncaught (in promise) ", reason);
	};
	rt.dispatchEvent = function (name, ...args) {
		const list = listeners.get(name);
		if (!list || list.length === 0) {
			if (signals.has(name)) process.exit(128 + { SIGHUP: 1, SIGINT: 2, SIGTERM: 15 }[name]);
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
		argv0: rt.argv[0] ?? "",
		execPath: rt.execPath ?? rt.argv[0] ?? "",
		env: rt.env,
		pid: rt.pid,
		platform: "darwin",
		arch: rt.arch,
		versions: { livewire_jsc: "0" },
		release: { name: "livewire-jsc" },
		cwd: () => rt.cwd(),
		chdir: (dir) => rt.chdir(String(dir)),
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
			constructor(label = "utf-8", options = {}) {
				const l = String(label).toLowerCase();
				if (l !== "utf-8" && l !== "utf8") throw new RangeError("TextDecoder: only utf-8 is supported");
				this.ignoreBOM = !!options.ignoreBOM;
				this.fatal = false;
				this._pending = null; // an incomplete sequence held over by {stream: true}
				this._started = false;
			}
			get encoding() { return "utf-8"; }
			decode(input, options = {}) {
				let b = input == null ? new Uint8Array(0) : input instanceof Uint8Array ? input : ArrayBuffer.isView(input) ? new Uint8Array(input.buffer, input.byteOffset, input.byteLength) : new Uint8Array(input);
				if (this._pending) {
					const joined = new Uint8Array(this._pending.length + b.length);
					joined.set(this._pending);
					joined.set(b, this._pending.length);
					b = joined;
					this._pending = null;
				}
				if (options.stream) {
					// Hold back a trailing sequence that the next chunk completes.
					for (let back = 1; back <= 3 && back <= b.length; back++) {
						const c = b[b.length - back];
						if ((c & 0xc0) === 0x80) continue;
						const need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
						if (need > back) { this._pending = b.slice(b.length - back); b = b.subarray(0, b.length - back); }
						break;
					}
				}
				let text = this._decode(b);
				if (!this._started && text.length > 0) {
					this._started = true;
					if (!this.ignoreBOM && text.charCodeAt(0) === 0xfeff) text = text.slice(1);
				}
				if (!options.stream) this._started = false;
				return text;
			}
			_decode(b) {
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
// --- URL / URLSearchParams (JSC alone has neither) -----------------------
	if (typeof g.URLSearchParams !== "function") {
		const enc = (s) => encodeURIComponent(s).replace(/%20/g, "+").replace(/[!'()~]/g, (c) => "%" + c.charCodeAt(0).toString(16).toUpperCase());
		const dec = (s) => { try { return decodeURIComponent(s.replace(/\+/g, " ")); } catch { return s; } };
		g.URLSearchParams = class URLSearchParams {
			constructor(init = "") {
				this._list = [];
				if (typeof init === "string") {
					for (const part of init.replace(/^\?/, "").split("&")) {
						if (!part) continue;
						const i = part.indexOf("=");
						this._list.push(i < 0 ? [dec(part), ""] : [dec(part.slice(0, i)), dec(part.slice(i + 1))]);
					}
				} else if (init && typeof init[Symbol.iterator] === "function") {
					for (const [k, v] of init) this._list.push([String(k), String(v)]);
				} else if (init && typeof init === "object") {
					for (const k of Object.keys(init)) this._list.push([k, String(init[k])]);
				}
			}
			_changed() { if (this._url) this._url._search = this._list.length ? "?" + this.toString() : ""; }
			append(k, v) { this._list.push([String(k), String(v)]); this._changed(); }
			delete(k) { this._list = this._list.filter(([n]) => n !== k); this._changed(); }
			get(k) { const e = this._list.find(([n]) => n === k); return e ? e[1] : null; }
			getAll(k) { return this._list.filter(([n]) => n === k).map(([, v]) => v); }
			has(k) { return this._list.some(([n]) => n === k); }
			set(k, v) {
				const i = this._list.findIndex(([n]) => n === k);
				if (i < 0) this._list.push([String(k), String(v)]);
				else { this._list[i][1] = String(v); this._list = this._list.filter(([n], j) => n !== k || j === i); }
				this._changed();
			}
			sort() { this._list.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)); this._changed(); }
			forEach(fn, thisArg) { for (const [k, v] of this._list) fn.call(thisArg, v, k, this); }
			keys() { return this._list.map(([k]) => k)[Symbol.iterator](); }
			values() { return this._list.map(([, v]) => v)[Symbol.iterator](); }
			entries() { return this._list.map((e) => [e[0], e[1]])[Symbol.iterator](); }
			[Symbol.iterator]() { return this.entries(); }
			get size() { return this._list.length; }
			toString() { return this._list.map(([k, v]) => enc(k) + "=" + enc(v)).join("&"); }
		};
	}
	if (typeof g.URL !== "function") {
		const special = { "http:": "80", "https:": "443", "ws:": "80", "wss:": "443", "ftp:": "21", "file:": "" };
		const normalizePath = (path) => {
			const out = [];
			for (const seg of path.split("/")) {
				if (seg === "..") { if (out.length > 1) out.pop(); }
				else if (seg !== ".") out.push(seg);
			}
			const joined = out.join("/");
			return (path.endsWith("/.") || path.endsWith("/..")) && !joined.endsWith("/") ? joined + "/" : joined;
		};
		const pattern = /^([a-zA-Z][a-zA-Z0-9+.-]*:)(?:\/\/(?:([^:@\/?#]*)(?::([^@\/?#]*))?@)?(\[[^\]]*\]|[^:\/?#]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/;
		class URL {
			constructor(input, base) {
				input = String(input).trim();
				let m = input.match(pattern);
				if (!m || (m && base !== undefined && !/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(input))) m = null;
				if (!m) {
					if (base === undefined) throw new TypeError("Invalid URL: " + input);
					const b = base instanceof URL ? base : new URL(base);
					let path, search = "", hash = "";
					const h = input.indexOf("#");
					if (h >= 0) { hash = input.slice(h); input = input.slice(0, h); }
					const q = input.indexOf("?");
					if (q >= 0) { search = input.slice(q); input = input.slice(0, q); }
					if (input.startsWith("//")) return new URL(b.protocol + input + search + hash);
					if (input.startsWith("/")) path = input;
					else if (input === "") { path = b.pathname; if (!search && q < 0) search = b.search; }
					else path = b.pathname.slice(0, b.pathname.lastIndexOf("/") + 1) + input;
					m = (b.protocol + (b._hasAuthority ? "//" + (b.username ? b.username + (b.password ? ":" + b.password : "") + "@" : "") + b.host : "") + normalizePath(path) + search + hash).match(pattern);
				}
				this._protocol = m[1].toLowerCase();
				this._hasAuthority = m[4] !== undefined || input.slice(m[1].length).startsWith("//");
				this.username = m[2] || "";
				this.password = m[3] || "";
				this._hostname = (m[4] || "").toLowerCase();
				this._port = m[5] && special[this._protocol] !== m[5] ? m[5] : "";
				let path = m[6] || "";
				if (this._hasAuthority && path === "" && this._protocol in special) path = "/";
				this._pathname = this._hasAuthority ? normalizePath(path) : path;
				this._search = m[7] && m[7] !== "?" ? m[7] : "";
				this._hash = m[8] && m[8] !== "#" ? m[8] : "";
				this._params = new g.URLSearchParams(this._search);
				this._params._url = this;
			}
			get protocol() { return this._protocol; }
			set protocol(v) { this._protocol = String(v).replace(/:?$/, ":").toLowerCase(); }
			get hostname() { return this._hostname; }
			set hostname(v) { this._hostname = String(v).toLowerCase(); }
			get port() { return this._port; }
			set port(v) { this._port = String(v); }
			get host() { return this._hostname + (this._port ? ":" + this._port : ""); }
			set host(v) { const [h, p] = String(v).split(":"); this._hostname = h.toLowerCase(); this._port = p || ""; }
			get pathname() { return this._pathname; }
			set pathname(v) { v = String(v); this._pathname = this._hasAuthority && !v.startsWith("/") ? "/" + v : v; }
			get search() { return this._search; }
			set search(v) { v = String(v); this._search = v && v !== "?" ? (v.startsWith("?") ? v : "?" + v) : ""; this._params = new g.URLSearchParams(this._search); this._params._url = this; }
			get searchParams() { return this._params; }
			get hash() { return this._hash; }
			set hash(v) { v = String(v); this._hash = v && v !== "#" ? (v.startsWith("#") ? v : "#" + v) : ""; }
			get origin() { return this._protocol in special && this._protocol !== "file:" ? this._protocol + "//" + this.host : "null"; }
			get href() {
				const auth = this.username ? this.username + (this.password ? ":" + this.password : "") + "@" : "";
				return this._protocol + (this._hasAuthority ? "//" + auth + this.host : "") + this._pathname + this._search + this._hash;
			}
			set href(v) { Object.assign(this, new URL(v)); }
			toString() { return this.href; }
			toJSON() { return this.href; }
			static canParse(input, base) { try { new URL(input, base); return true; } catch { return false; } }
		}
		g.URL = URL;
	}
	g.global = g;
	g.self = g.self || g;
})(globalThis.__rt);
