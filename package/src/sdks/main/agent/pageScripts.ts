// JavaScript sources the agent server injects into a view. Each one evaluates
// to a value that `wrapEval` posts back to the main process.

/**
 * True when `source` parses as a single expression (`document.title`), false
 * when it needs to run as a function body (`const a = 1; return a`).
 */
export function isExpression(source: string): boolean {
	const AsyncFunction = (async () => {}).constructor as FunctionConstructor;
	try {
		new AsyncFunction(`return (${source}\n);`);
		return true;
	} catch {
		return false;
	}
}

// WebKit's Error.stack leaves out the message; V8's (CEF) starts with it.
const ERROR_TEXT = `(error) => {
	if (!(error instanceof Error)) return String(error);
	const head = error.name + ": " + error.message;
	const stack = error.stack || "";
	return stack.includes(error.message) ? stack : stack ? head + "\\n" + stack : head;
}`;

// Turns any page value into something JSON can carry: elements become
// truncated outerHTML, NodeLists become arrays, cycles become "[Circular]".
const SERIALIZE = `(value) => {
	const MAX = 4000;
	const describe = (el) => {
		const html = el.outerHTML || "";
		return html.length > MAX ? html.slice(0, MAX) + "…" : html;
	};
	if (value === undefined) return null;
	if (typeof value === "function") return "[Function " + (value.name || "anonymous") + "]";
	if (typeof value === "bigint") return value.toString();
	if (typeof Element !== "undefined" && value instanceof Element) return describe(value);
	if (typeof Node !== "undefined" && value instanceof Node) return value.textContent;
	if (typeof NodeList !== "undefined" && (value instanceof NodeList || value instanceof HTMLCollection)) {
		return Array.from(value, (el) => (el instanceof Element ? describe(el) : el.textContent));
	}
	const seen = new WeakSet();
	try {
		return JSON.parse(JSON.stringify(value, (key, x) => {
			if (typeof x === "bigint") return x.toString();
			if (typeof x === "function") return "[Function]";
			if (x && typeof x === "object") {
				if (typeof Element !== "undefined" && x instanceof Element) return "<" + x.tagName.toLowerCase() + ">";
				if (seen.has(x)) return "[Circular]";
				seen.add(x);
			}
			return x;
		}) ?? "null");
	} catch {
		return String(value);
	}
}`;

/**
 * Wraps agent code so its (awaited) result or error is posted back over the
 * internal bridge as an `agentEvalResult` message carrying `token`.
 */
export function wrapEval(source: string, token: string): string {
	const body = isExpression(source) ? `return (${source}\n);` : source;
	return `(async () => {
	const token = ${JSON.stringify(token)};
	const serialize = ${SERIALIZE};
	const errorText = ${ERROR_TEXT};
	const post = (payload) => {
		const bridge = window.__electrobunInternalBridge;
		if (!bridge) return;
		bridge.postMessage(JSON.stringify([JSON.stringify({ type: "message", id: "agentEvalResult", payload })]));
	};
	try {
		const value = await (async () => {
${body}
		})();
		post({ token, ok: true, value: serialize(value) });
	} catch (error) {
		post({ token, ok: false, error: errorText(error) });
	}
})();`;
}

const FIND = `(selector) => {
	const el = document.querySelector(selector);
	if (!el) throw new Error("no element matches " + JSON.stringify(selector));
	return el;
}`;

const label = `(el) => (
	el.getAttribute("aria-label") ||
	(el.labels && el.labels[0] && el.labels[0].innerText) ||
	el.innerText ||
	el.getAttribute("placeholder") ||
	el.getAttribute("title") ||
	el.value ||
	""
).trim().replace(/\\s+/g, " ").slice(0, 120)`;

export function clickScript(selector: string): string {
	return `const el = (${FIND})(${JSON.stringify(selector)});
el.scrollIntoView({ block: "center", inline: "center" });
if (typeof el.focus === "function") el.focus();
const rect = el.getBoundingClientRect();
const init = { bubbles: true, cancelable: true, composed: true, clientX: rect.x + rect.width / 2, clientY: rect.y + rect.height / 2, button: 0 };
el.dispatchEvent(new PointerEvent("pointerdown", init));
el.dispatchEvent(new MouseEvent("mousedown", init));
el.dispatchEvent(new PointerEvent("pointerup", init));
el.dispatchEvent(new MouseEvent("mouseup", init));
el.click();
return { clicked: el.tagName.toLowerCase(), label: (${label})(el) };`;
}

export function typeScript(selector: string, text: string, submit: boolean): string {
	return `const el = (${FIND})(${JSON.stringify(selector)});
el.scrollIntoView({ block: "center" });
el.focus();
const text = ${JSON.stringify(text)};
if (el.isContentEditable) {
	el.textContent = text;
} else {
	// Use the prototype setter so frameworks that track the value (React) see the change.
	const proto = Object.getPrototypeOf(el);
	const setter = Object.getOwnPropertyDescriptor(proto, "value")?.set;
	if (setter) setter.call(el, text); else el.value = text;
}
el.dispatchEvent(new InputEvent("input", { bubbles: true, composed: true, data: text, inputType: "insertText" }));
el.dispatchEvent(new Event("change", { bubbles: true }));
if (${submit}) {
	const key = { key: "Enter", code: "Enter", keyCode: 13, which: 13, bubbles: true, cancelable: true };
	const proceed = el.dispatchEvent(new KeyboardEvent("keydown", key));
	el.dispatchEvent(new KeyboardEvent("keyup", key));
	if (proceed && el.form) el.form.requestSubmit ? el.form.requestSubmit() : el.form.submit();
}
return { typed: text.length, into: el.tagName.toLowerCase() };`;
}

export function textScript(selector: string | undefined, maxChars: number): string {
	const target = selector
		? `(${FIND})(${JSON.stringify(selector)})`
		: "document.body";
	return `const text = (${target}).innerText || "";
return text.length > ${maxChars} ? text.slice(0, ${maxChars}) + "\\n…[truncated]" : text;`;
}

export function waitForScript(
	selector: string,
	timeoutMs: number,
	visible: boolean,
): string {
	return `const selector = ${JSON.stringify(selector)};
const deadline = Date.now() + ${timeoutMs};
const ready = () => {
	const el = document.querySelector(selector);
	if (!el) return null;
	if (${visible}) {
		const rect = el.getBoundingClientRect();
		const style = getComputedStyle(el);
		if (rect.width === 0 || rect.height === 0 || style.visibility === "hidden" || style.display === "none") return null;
	}
	return el;
};
while (Date.now() < deadline) {
	const el = ready();
	if (el) return { found: true, tag: el.tagName.toLowerCase() };
	await new Promise((resolve) => setTimeout(resolve, 100));
}
throw new Error("timed out after ${timeoutMs}ms waiting for " + JSON.stringify(selector));`;
}

/**
 * Lists what an agent can act on: links, buttons, inputs and other controls,
 * each with a selector it can pass back to click/type.
 */
export function outlineScript(limit: number): string {
	return `const label = ${label};
const selectorFor = (el) => {
	if (el.id && document.querySelectorAll("#" + CSS.escape(el.id)).length === 1) return "#" + CSS.escape(el.id);
	const testId = el.getAttribute("data-testid");
	if (testId) return "[data-testid=" + JSON.stringify(testId) + "]";
	const parts = [];
	for (let node = el; node && node.nodeType === 1 && node !== document.documentElement; node = node.parentElement) {
		let part = node.tagName.toLowerCase();
		const parent = node.parentElement;
		if (parent) {
			const same = Array.from(parent.children).filter((child) => child.tagName === node.tagName);
			if (same.length > 1) part += ":nth-of-type(" + (same.indexOf(node) + 1) + ")";
		}
		parts.unshift(part);
		if (node.id) { parts[0] = "#" + CSS.escape(node.id); break; }
	}
	return parts.join(" > ");
};
const controls = document.querySelectorAll('a[href], button, input, select, textarea, summary, [role=button], [role=link], [role=tab], [role=menuitem], [role=checkbox], [contenteditable=""], [contenteditable=true], [onclick]');
const out = [];
for (const el of controls) {
	const rect = el.getBoundingClientRect();
	if (rect.width === 0 || rect.height === 0) continue;
	out.push({
		selector: selectorFor(el),
		role: el.getAttribute("role") || el.tagName.toLowerCase() + (el.type ? ":" + el.type : ""),
		label: label(el),
		disabled: Boolean(el.disabled || el.getAttribute("aria-disabled") === "true"),
	});
	if (out.length >= ${limit}) break;
}
return { url: location.href, title: document.title, controls: out };`;
}

/**
 * Installs (once) a buffer of console output and uncaught errors, then returns
 * and clears what was buffered since the last call.
 */
export function consoleScript(): string {
	return `if (!window.__livewireAgentConsole) {
	const buffer = [];
	const errorText = ${ERROR_TEXT};
	const push = (level, args) => {
		buffer.push({ level, at: Date.now(), text: args.map((a) => {
			if (a instanceof Error) return errorText(a);
			if (typeof a === "string") return a;
			try { return JSON.stringify(a); } catch { return String(a); }
		}).join(" ").slice(0, 4000) });
		if (buffer.length > 500) buffer.shift();
	};
	for (const level of ["log", "info", "warn", "error", "debug"]) {
		const original = console[level];
		console[level] = function (...args) { push(level, args); return original.apply(this, args); };
	}
	window.addEventListener("error", (event) => push("uncaught", [event.error || event.message]));
	window.addEventListener("unhandledrejection", (event) => push("unhandledrejection", [event.reason]));
	window.__livewireAgentConsole = buffer;
	return { installed: true, entries: [] };
}
const entries = window.__livewireAgentConsole.splice(0);
return { installed: false, entries };`;
}
