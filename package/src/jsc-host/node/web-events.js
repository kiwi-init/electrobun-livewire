// The DOM event pieces the system JavaScriptCore lacks and fetch/WebSocket
// need: Event, EventTarget, DOMException, AbortController/AbortSignal, and
// MessageEvent, CloseEvent and ErrorEvent. Behaviour follows Bun's.

const codes = {
	IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5, NoModificationAllowedError: 7,
	NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12, InvalidModificationError: 13, NamespaceError: 14,
	InvalidAccessError: 15, TypeMismatchError: 17, SecurityError: 18, NetworkError: 19, AbortError: 20, URLMismatchError: 21,
	QuotaExceededError: 22, TimeoutError: 23, InvalidNodeTypeError: 24, DataCloneError: 25,
};
export class DOMException extends Error {
	constructor(message = "", name = "Error") {
		super(String(message));
		Object.defineProperty(this, "name", { value: String(typeof name === "object" && name ? name.name ?? "Error" : name), writable: true, configurable: true });
	}
	get code() {
		return codes[this.name] ?? 0;
	}
}
for (const [name, code] of Object.entries(codes)) {
	const constant = name.replace(/Error$/, "").replace(/([a-z])([A-Z])/g, "$1_$2").toUpperCase() + "_ERR";
	Object.defineProperty(DOMException, constant, { value: code, enumerable: true });
}

const now = () => (globalThis.performance?.now ? globalThis.performance.now() : Date.now());
export class Event {
	constructor(type, init = {}) {
		if (arguments.length === 0) throw new TypeError("Not enough arguments");
		this.type = String(type);
		this.bubbles = !!init.bubbles;
		this.cancelable = !!init.cancelable;
		this.composed = !!init.composed;
		this.defaultPrevented = false;
		this.isTrusted = false;
		this.timeStamp = now();
		this.target = null;
		this.currentTarget = null;
		this.eventPhase = 0;
		this._stopped = false;
	}
	get srcElement() {
		return this.target;
	}
	composedPath() {
		return this.currentTarget ? [this.currentTarget] : [];
	}
	preventDefault() {
		if (this.cancelable) this.defaultPrevented = true;
	}
	stopPropagation() {}
	stopImmediatePropagation() {
		this._stopped = true;
	}
}
Object.assign(Event, { NONE: 0, CAPTURING_PHASE: 1, AT_TARGET: 2, BUBBLING_PHASE: 3 });

const listenersOf = (target) => target._listeners ?? (Object.defineProperty(target, "_listeners", { value: new Map() }), target._listeners);
const captureOf = (options) => (typeof options === "boolean" ? options : !!options?.capture);

export class EventTarget {
	addEventListener(type, listener, options) {
		if (!listener) return;
		const capture = captureOf(options);
		const signal = typeof options === "object" ? options?.signal : undefined;
		if (signal?.aborted) return;
		const map = listenersOf(this);
		const list = map.get(type) ?? [];
		if (list.some((l) => l.listener === listener && l.capture === capture)) return;
		list.push({ listener, capture, once: typeof options === "object" && !!options?.once, removed: false });
		map.set(type, list);
		if (signal) signal.addEventListener("abort", () => this.removeEventListener(type, listener, { capture }), { once: true });
	}
	removeEventListener(type, listener, options) {
		const list = listenersOf(this).get(type);
		if (!list) return;
		const capture = captureOf(options);
		const at = list.findIndex((l) => l.listener === listener && l.capture === capture);
		if (at >= 0) {
			list[at].removed = true;
			list.splice(at, 1);
		}
	}
	dispatchEvent(event) {
		if (!(event instanceof Event)) throw new TypeError("The 1st argument must be an instance of Event");
		event.target = this;
		event.currentTarget = this;
		event.eventPhase = 2;
		for (const entry of [...(listenersOf(this).get(event.type) ?? [])]) {
			if (entry.removed) continue;
			if (entry.once) this.removeEventListener(event.type, entry.listener, { capture: entry.capture });
			try {
				if (typeof entry.listener === "function") entry.listener.call(this, event);
				else entry.listener.handleEvent(event);
			} catch (error) {
				// A throwing listener doesn't stop the others; it's reported as uncaught.
				queueMicrotask(() => {
					throw error;
				});
			}
			if (event._stopped) break;
		}
		event.currentTarget = null;
		event.eventPhase = 0;
		return !event.defaultPrevented;
	}
}

// on<type> properties: the handler is a listener, added where it was set.
export function eventHandlers(proto, ...types) {
	for (const type of types) {
		Object.defineProperty(proto, "on" + type, {
			configurable: true,
			enumerable: true,
			get() {
				return this["_on" + type] ?? null;
			},
			set(fn) {
				const old = this["_on" + type + "Listener"];
				if (old) this.removeEventListener(type, old);
				this["_on" + type] = typeof fn === "function" ? fn : null;
				if (typeof fn !== "function") return void (this["_on" + type + "Listener"] = null);
				const listener = (event) => this["_on" + type]?.call(this, event);
				this["_on" + type + "Listener"] = listener;
				this.addEventListener(type, listener);
			},
		});
	}
}

export class MessageEvent extends Event {
	constructor(type, init = {}) {
		super(type, init);
		this.data = init.data ?? null;
		this.origin = init.origin ?? "";
		this.lastEventId = init.lastEventId ?? "";
		this.source = init.source ?? null;
		this.ports = init.ports ?? [];
	}
}
export class CloseEvent extends Event {
	constructor(type, init = {}) {
		super(type, init);
		this.code = init.code ?? 0;
		this.reason = init.reason ?? "";
		this.wasClean = !!init.wasClean;
	}
}
export class ErrorEvent extends Event {
	constructor(type, init = {}) {
		super(type, init);
		this.message = init.message ?? "";
		this.filename = init.filename ?? "";
		this.lineno = init.lineno ?? 0;
		this.colno = init.colno ?? 0;
		this.error = init.error;
	}
}

const internal = Symbol("internal");
function abortSignal(signal, reason) {
	if (signal.aborted) return;
	signal._aborted = true;
	signal._reason = reason === undefined ? new DOMException("The operation was aborted.", "AbortError") : reason;
	signal.dispatchEvent(new Event("abort"));
}
export class AbortSignal extends EventTarget {
	constructor(key) {
		if (key !== internal) throw new TypeError("Illegal constructor");
		super();
		this._aborted = false;
		this._reason = undefined;
	}
	get aborted() {
		return this._aborted;
	}
	get reason() {
		return this._reason;
	}
	throwIfAborted() {
		if (this._aborted) throw this._reason;
	}
	static abort(reason) {
		const signal = new AbortSignal(internal);
		abortSignal(signal, reason);
		return signal;
	}
	static timeout(ms) {
		const signal = new AbortSignal(internal);
		const timer = setTimeout(() => abortSignal(signal, new DOMException("The operation timed out.", "TimeoutError")), ms);
		// As in Bun and Node, a pending timeout doesn't keep the process alive.
		timer?.unref?.();
		return signal;
	}
	static any(signals) {
		const signal = new AbortSignal(internal);
		for (const source of signals) {
			if (source.aborted) {
				abortSignal(signal, source.reason);
				return signal;
			}
		}
		for (const source of signals) source.addEventListener("abort", () => abortSignal(signal, source.reason), { once: true });
		return signal;
	}
}
eventHandlers(AbortSignal.prototype, "abort");

export class AbortController {
	constructor() {
		this._signal = new AbortSignal(internal);
	}
	get signal() {
		return this._signal;
	}
	abort(reason) {
		abortSignal(this._signal, reason);
	}
}
