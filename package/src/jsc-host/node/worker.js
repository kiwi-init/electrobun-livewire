// Workers on the Livewire JSC runtime (worker.c runs each on its own thread
// and VM): the global Worker (Bun's web-style one), structuredClone, and in a
// worker its scope (self, postMessage, onmessage) and parentPort for
// node:worker_threads.
const rt = globalThis.__rt;

// --- structured clone ------------------------------------------------------------
// A value becomes JSON plus the ArrayBuffers it holds; worker.c copies the
// buffers across. Objects are tagged arrays, so shared and cyclic references,
// undefined, NaN, -0, bigints, Dates, RegExps, Maps, Sets, Errors and typed
// arrays survive the trip, as with the HTML structured clone.
const TYPED = {
	Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array,
	Float32Array, Float64Array, BigInt64Array, BigUint64Array, DataView,
};
const typedName = (v) => {
	for (const name in TYPED) if (v instanceof TYPED[name]) return name;
	return "Uint8Array";
};
const ERRORS = { Error, EvalError, RangeError, ReferenceError, SyntaxError, TypeError, URIError };
const cloneError = (what) => {
	const E = globalThis.DOMException;
	const message = `${what} could not be cloned.`;
	return E ? new E(message, "DataCloneError") : Object.assign(new Error(message), { name: "DataCloneError" });
};

export function serialize(value) {
	const buffers = [];
	const bufferIndex = new Map();
	const seen = new Map();
	let nextId = 0;
	const buffer = (ab) => {
		let i = bufferIndex.get(ab);
		if (i === undefined) { i = buffers.length; buffers.push(ab); bufferIndex.set(ab, i); }
		return i;
	};
	const enc = (v) => {
		switch (typeof v) {
			case "string":
			case "boolean":
				return v;
			case "number":
				return Number.isFinite(v) && !Object.is(v, -0) ? v : ["n", Object.is(v, -0) ? "-0" : String(v)];
			case "undefined":
				return ["u"];
			case "bigint":
				return ["b", v.toString()];
			case "function":
				throw cloneError(String(v).slice(0, 60));
			case "symbol":
				throw cloneError(String(v));
		}
		if (v === null) return null;
		if (seen.has(v)) return ["r", seen.get(v)];
		const id = nextId++;
		seen.set(v, id);
		if (Array.isArray(v)) {
			const out = ["A", id];
			for (let i = 0; i < v.length; i++) out.push(enc(v[i]));
			return out;
		}
		if (v instanceof ArrayBuffer) return ["B", id, buffer(v)];
		if (ArrayBuffer.isView(v)) {
			const name = typedName(v);
			return ["T", id, name, buffer(v.buffer), v.byteOffset, name === "DataView" ? v.byteLength : v.length];
		}
		if (v instanceof Date) return ["D", id, v.getTime()];
		if (v instanceof RegExp) return ["R", id, v.source, v.flags];
		if (v instanceof Map) {
			const out = ["M", id];
			for (const [k, x] of v) out.push(enc(k), enc(x));
			return out;
		}
		if (v instanceof Set) {
			const out = ["S", id];
			for (const x of v) out.push(enc(x));
			return out;
		}
		if (v instanceof Error) {
			const name = v.name in ERRORS ? v.name : "Error";
			return ["E", id, name, String(v.message), v.stack === undefined ? ["u"] : String(v.stack), "cause" in v ? enc(v.cause) : ["x"]];
		}
		if (v instanceof Boolean || v instanceof Number || v instanceof String) return ["P", id, enc(v.valueOf())];
		if (v instanceof Promise || v instanceof WeakMap || v instanceof WeakSet) throw cloneError("#<" + v.constructor.name + ">");
		const out = ["O", id];
		for (const k of Object.keys(v)) out.push(k, enc(v[k]));
		return out;
	};
	return { json: JSON.stringify(enc(value)), buffers };
}

export function deserialize(json, buffers) {
	const refs = [];
	const dec = (x) => {
		if (x === null || typeof x !== "object") return x;
		switch (x[0]) {
			case "n": return Number(x[1]);
			case "u": return undefined;
			case "b": return BigInt(x[1]);
			case "r": return refs[x[1]];
			case "A": {
				const a = (refs[x[1]] = []);
				for (let i = 2; i < x.length; i++) a.push(dec(x[i]));
				return a;
			}
			case "B": return (refs[x[1]] = buffers[x[2]]);
			case "T": return (refs[x[1]] = new TYPED[x[2]](buffers[x[3]], x[4], x[5]));
			case "D": return (refs[x[1]] = new Date(x[2]));
			case "R": return (refs[x[1]] = new RegExp(x[2], x[3]));
			case "M": {
				const m = (refs[x[1]] = new Map());
				for (let i = 2; i < x.length; i += 2) m.set(dec(x[i]), dec(x[i + 1]));
				return m;
			}
			case "S": {
				const s = (refs[x[1]] = new Set());
				for (let i = 2; i < x.length; i++) s.add(dec(x[i]));
				return s;
			}
			case "E": {
				const e = (refs[x[1]] = new ERRORS[x[2]](x[3]));
				const stack = dec(x[4]);
				if (stack === undefined) delete e.stack;
				else Object.defineProperty(e, "stack", { value: stack, writable: true, configurable: true, enumerable: false });
				if (!(Array.isArray(x[5]) && x[5][0] === "x")) Object.defineProperty(e, "cause", { value: dec(x[5]), writable: true, configurable: true, enumerable: false });
				return e;
			}
			case "P": return (refs[x[1]] = Object(dec(x[2])));
			case "O": {
				const o = (refs[x[1]] = {});
				for (let i = 2; i < x.length; i += 2) Object.defineProperty(o, x[i], { value: dec(x[i + 1]), writable: true, enumerable: true, configurable: true });
				return o;
			}
		}
		throw new Error("worker message: unknown tag " + x[0]);
	};
	return dec(JSON.parse(json));
}

export function structuredClone(value) {
	const { json, buffers } = serialize(value);
	return deserialize(json, buffers.map((b) => b.slice(0)));
}

// --- events ------------------------------------------------------------------------
// The slice of EventTarget a Worker needs: listeners (with once), on<type>
// handlers, and events as plain objects.
class Events {
	constructor() { this._listeners = new Map(); }
	addEventListener(type, fn, options) {
		if (typeof fn !== "function" && typeof fn?.handleEvent !== "function") return;
		const list = this._listeners.get(type) ?? [];
		if (list.some((l) => l.fn === fn)) return;
		list.push({ fn, once: !!(options && typeof options === "object" && options.once) });
		this._listeners.set(type, list);
		this._listenersChanged?.(type);
	}
	removeEventListener(type, fn) {
		const list = this._listeners.get(type);
		if (!list) return;
		const i = list.findIndex((l) => l.fn === fn);
		if (i >= 0) list.splice(i, 1);
		this._listenersChanged?.(type);
	}
	_count(type) { return (this._listeners.get(type)?.length ?? 0) + (typeof this["on" + type] === "function" ? 1 : 0); }
	_emit(type, fields) {
		const event = { type, target: this, currentTarget: this, defaultPrevented: false, preventDefault() { this.defaultPrevented = true; }, stopPropagation() {}, stopImmediatePropagation() {}, ...fields };
		const handler = this["on" + type];
		if (typeof handler === "function") handler.call(this, event);
		for (const l of [...(this._listeners.get(type) ?? [])]) {
			if (l.once) this.removeEventListener(type, l.fn);
			if (typeof l.fn === "function") l.fn.call(this, event); else l.fn.handleEvent(event);
		}
		return event;
	}
	dispatchEvent(event) { this._emit(event.type, event); return !event.defaultPrevented; }
}

// --- the parent's side ---------------------------------------------------------------
function workerPath(url) {
	let s = url instanceof URL ? url.href : String(url);
	if (s.startsWith("file:")) s = decodeURIComponent(new URL(s).pathname);
	if (s.startsWith("/")) return s;
	const cwd = typeof process !== "undefined" ? process.cwd() : "/";
	return (cwd.endsWith("/") ? cwd : cwd + "/") + s.replace(/^\.\//, "");
}

function errorFrom(json) {
	let info;
	try { info = JSON.parse(json); } catch { info = { message: json }; }
	const error = new (ERRORS[info.name] ?? Error)(info.message ?? "");
	if (info.name && !(info.name in ERRORS)) error.name = info.name;
	if (info.stack) Object.defineProperty(error, "stack", { value: info.stack, writable: true, configurable: true });
	return error;
}

export class Worker extends Events {
	constructor(url, options = {}) {
		super();
		const init = serialize({ workerData: options?.workerData, name: options?.name ?? "", argv: options?.argv, env: options?.env });
		this._exited = false;
		this._ref = true;
		this._id = rt.workerSpawn(workerPath(url), (type, json, buffers) => this._event(type, json, buffers), init.json, init.buffers);
		this.threadId = this._id;
		rt.refLoop(true); // a running Worker keeps the parent's loop open, as in Bun
		this.onmessage = null;
		this.onerror = null;
		this.onmessageerror = null;
	}
	postMessage(data, transfer) {
		void transfer; // copied, not transferred
		if (this._exited) return;
		const { json, buffers } = serialize(data);
		rt.workerPost(this._id, json, buffers);
	}
	terminate() { if (!this._exited) rt.workerTerminate(this._id); }
	ref() { if (!this._ref && !this._exited) { this._ref = true; rt.refLoop(true); } return this; }
	unref() { if (this._ref && !this._exited) { this._ref = false; rt.refLoop(false); } return this; }
	_event(type, json, buffers) {
		if (type === "message") {
			this._emit("message", { data: deserialize(json, buffers), ports: [] });
		} else if (type === "error") {
			const error = errorFrom(json);
			const event = this._emit("error", { error, message: error.message, filename: "", lineno: 0, colno: 0 });
			// With nobody listening, it's reported in the parent, as Bun prints it.
			if (!this._count("error") && !event.defaultPrevented && !this._quiet) console.error(error);
		} else if (type === "exit") {
			this._exited = true;
			if (this._ref) { this._ref = false; rt.refLoop(false); }
			this._emit("close", { code: Number(json) });
		}
	}
}

// --- inside a worker ------------------------------------------------------------------
export const isMainThread = !rt.isWorker;
export const threadId = rt.isWorker ? rt.workerId : 0;
let init = null;
if (rt.isWorker) {
	const pair = rt.workerInit();
	init = pair ? deserialize(pair[0], pair[1]) : {};
}
export const workerData = init?.workerData ?? null;

// parentPort: Node's EventEmitter shape (on("message", data)) and the
// EventTarget one (addEventListener("message", event)); the worker scope's
// onmessage and addEventListener deliver through it too. Messages wait until
// something listens, and a listener holds the worker's loop open.
class ParentPort extends Events {
	constructor() {
		super();
		this._node = new Map();
		this._queue = [];
		this._refd = false;
		this._unrefd = false;
		this._closed = false;
	}
	_listenersChanged() { this._sync(); }
	_hasListeners() {
		const scopeHandler = typeof globalThis.onmessage === "function";
		return this._count("message") > 0 || (this._node.get("message")?.length ?? 0) > 0 || scopeHandler || scopeEvents._count("message") > 0;
	}
	_sync() {
		const want = this._hasListeners() && !this._unrefd && !this._closed;
		if (want !== this._refd) { this._refd = want; rt.refLoop(want); }
		if (this._hasListeners()) for (const data of this._queue.splice(0)) this._deliver(data);
	}
	_deliver(data) {
		if (!this._hasListeners()) { this._queue.push(data); return; }
		for (const fn of [...(this._node.get("message") ?? [])]) fn.call(this, data);
		this._emit("message", { data, ports: [] });
		const event = { type: "message", data, ports: [], target: globalThis, currentTarget: globalThis };
		if (typeof globalThis.onmessage === "function") globalThis.onmessage(event);
		scopeEvents._emit("message", { data, ports: [] });
	}
	on(name, fn) {
		const list = this._node.get(name) ?? [];
		list.push(fn);
		this._node.set(name, list);
		this._sync();
		return this;
	}
	addListener(name, fn) { return this.on(name, fn); }
	once(name, fn) {
		const wrapped = (...args) => { this.off(name, wrapped); fn.apply(this, args); };
		return this.on(name, wrapped);
	}
	off(name, fn) {
		const list = this._node.get(name);
		const i = list ? list.indexOf(fn) : -1;
		if (i >= 0) list.splice(i, 1);
		this._sync();
		return this;
	}
	removeListener(name, fn) { return this.off(name, fn); }
	removeAllListeners(name) { if (name) this._node.delete(name); else this._node.clear(); this._sync(); return this; }
	emit(name, ...args) { for (const fn of [...(this._node.get(name) ?? [])]) fn.apply(this, args); return true; }
	postMessage(data, transfer) {
		void transfer;
		const { json, buffers } = serialize(data);
		rt.parentPost(json, buffers);
	}
	start() {}
	ref() { this._unrefd = false; this._sync(); return this; }
	unref() { this._unrefd = true; this._sync(); return this; }
	close() { this._closed = true; this._sync(); }
}
const scopeEvents = new Events();
export const parentPort = rt.isWorker ? new ParentPort() : null;

if (rt.isWorker) {
	scopeEvents._listenersChanged = () => parentPort._sync();
	rt.onParentMessage = (json, buffers) => parentPort._deliver(deserialize(json, buffers));
	const define = (name, value) => Object.defineProperty(globalThis, name, { value, writable: true, configurable: true });
	define("self", globalThis);
	define("name", init?.name ?? "");
	define("postMessage", (data, transfer) => parentPort.postMessage(data, transfer));
	define("addEventListener", (type, fn, options) => scopeEvents.addEventListener(type, fn, options));
	define("removeEventListener", (type, fn) => scopeEvents.removeEventListener(type, fn));
	define("close", () => process.exit(0));
	let onmessage = null;
	Object.defineProperty(globalThis, "onmessage", {
		get: () => onmessage,
		set: (fn) => { onmessage = typeof fn === "function" ? fn : null; parentPort._sync(); },
		configurable: true,
	});
}
