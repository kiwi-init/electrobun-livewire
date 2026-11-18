// Bun.TOML.parse: TOML 1.0 to plain objects (dates stay strings).
export function parse(source) {
	const text = String(source);
	let i = 0;
	const root = {};
	let table = root;
	const defined = new Set();
	const fail = (message) => {
		const line = text.slice(0, i).split("\n").length;
		throw new SyntaxError("TOML: " + message + " at line " + line);
	};
	const ws = () => { while (text[i] === " " || text[i] === "\t") i++; };
	const skip = () => {
		for (;;) {
			ws();
			if (text[i] === "#") while (i < text.length && text[i] !== "\n") i++;
			if (text[i] === "\n" || text[i] === "\r") i++;
			else return;
		}
	};
	const eol = () => {
		ws();
		if (text[i] === "#") while (i < text.length && text[i] !== "\n") i++;
		if (text[i] === "\r") i++;
		if (i < text.length && text[i] !== "\n") fail("expected the end of the line");
		i++;
	};
	const escape = () => {
		const c = text[i++];
		const simple = { b: "\b", t: "\t", n: "\n", f: "\f", r: "\r", '"': '"', "\\": "\\", e: "\x1b" };
		if (c in simple) return simple[c];
		if (c === "u" || c === "U") {
			const len = c === "u" ? 4 : 8;
			const cp = parseInt(text.slice(i, i + len), 16);
			i += len;
			return String.fromCodePoint(cp);
		}
		fail("bad escape \\" + c);
	};
	const string = () => {
		if (text.startsWith('"""', i)) {
			i += 3;
			if (text[i] === "\r") i++;
			if (text[i] === "\n") i++;
			let out = "";
			for (;;) {
				if (i >= text.length) fail("unterminated string");
				if (text.startsWith('"""', i)) {
					let end = i + 3;
					while (text[end] === '"' && end - i < 5) end++;
					out += text.slice(i + 3, end);
					i = end;
					return out;
				}
				const c = text[i++];
				if (c === "\\") {
					if (/[ \t\r\n]/.test(text[i])) {
						while (/[ \t\r\n]/.test(text[i])) i++;
					} else out += escape();
				} else out += c;
			}
		}
		if (text.startsWith("'''", i)) {
			i += 3;
			if (text[i] === "\r") i++;
			if (text[i] === "\n") i++;
			const end = text.indexOf("'''", i);
			if (end < 0) fail("unterminated string");
			let stop = end + 3;
			while (text[stop] === "'" && stop - end < 5) stop++;
			const out = text.slice(i, stop - 3);
			i = stop;
			return out;
		}
		if (text[i] === "'") {
			const end = text.indexOf("'", i + 1);
			if (end < 0) fail("unterminated string");
			const out = text.slice(i + 1, end);
			i = end + 1;
			return out;
		}
		i++;
		let out = "";
		for (;;) {
			const c = text[i++];
			if (c === undefined || c === "\n") fail("unterminated string");
			if (c === '"') return out;
			out += c === "\\" ? escape() : c;
		}
	};
	const key = () => {
		const parts = [];
		for (;;) {
			ws();
			if (text[i] === '"' || text[i] === "'") parts.push(string());
			else {
				const m = /^[A-Za-z0-9_-]+/.exec(text.slice(i, i + 256));
				if (!m) fail("expected a key");
				parts.push(m[0]);
				i += m[0].length;
			}
			ws();
			if (text[i] !== ".") return parts;
			i++;
		}
	};
	const value = () => {
		ws();
		const c = text[i];
		if (c === '"' || c === "'") return string();
		if (c === "[") {
			i++;
			const out = [];
			for (;;) {
				skip();
				if (text[i] === "]") { i++; return out; }
				out.push(value());
				skip();
				if (text[i] === ",") i++;
				else if (text[i] !== "]") fail("expected , or ]");
			}
		}
		if (c === "{") {
			i++;
			const out = {};
			ws();
			if (text[i] === "}") { i++; return out; }
			for (;;) {
				const path = key();
				if (text[i++] !== "=") fail("expected =");
				assign(out, path, value());
				ws();
				if (text[i] === "}") { i++; return out; }
				if (text[i++] !== ",") fail("expected , or }");
			}
		}
		const m = /^[^ \t\r\n,\]}#]+(?: [0-9]{2}:[0-9]{2}[^ \t\r\n,\]}#]*)?/.exec(text.slice(i, i + 128));
		if (!m) fail("expected a value");
		const word = m[0];
		i += word.length;
		if (word === "true") return true;
		if (word === "false") return false;
		if (/^[+-]?(inf|nan)$/.test(word)) return word.includes("nan") ? NaN : word[0] === "-" ? -Infinity : Infinity;
		if (/^\d{4}-\d{2}-\d{2}|^\d{2}:\d{2}/.test(word)) return word;
		const plain = word.replace(/_/g, "");
		if (/^[+-]?0x[0-9a-f]+$/i.test(plain)) return parseInt(plain, 16);
		if (/^[+-]?0o[0-7]+$/i.test(plain)) return parseInt(plain.replace(/0o/i, ""), 8);
		if (/^[+-]?0b[01]+$/i.test(plain)) return parseInt(plain.replace(/0b/i, ""), 2);
		if (/^[+-]?(\d+)(\.\d+)?([eE][+-]?\d+)?$/.test(plain)) return Number(plain);
		fail("bad value " + word);
	};
	const assign = (target, path, v) => {
		let t = target;
		for (const part of path.slice(0, -1)) {
			if (t[part] === undefined) t[part] = {};
			else if (typeof t[part] !== "object" || Array.isArray(t[part])) fail("key " + part + " is already a value");
			t = t[part];
		}
		const last = path[path.length - 1];
		if (Object.prototype.hasOwnProperty.call(t, last)) fail("duplicate key " + last);
		t[last] = v;
	};
	const walk = (path, arrayLast) => {
		let t = root;
		path.forEach((part, n) => {
			const isLast = n === path.length - 1;
			if (isLast && arrayLast) {
				if (t[part] === undefined) t[part] = [];
				if (!Array.isArray(t[part])) fail("key " + part + " is not an array of tables");
				const next = {};
				t[part].push(next);
				t = next;
				return;
			}
			if (t[part] === undefined) t[part] = {};
			let next = t[part];
			if (Array.isArray(next)) next = next[next.length - 1];
			if (typeof next !== "object" || next === null) fail("key " + part + " is already a value");
			t = next;
		});
		return t;
	};
	for (;;) {
		skip();
		if (i >= text.length) return root;
		if (text[i] === "[") {
			const array = text[i + 1] === "[";
			i += array ? 2 : 1;
			const path = key();
			if (text[i++] !== "]" || (array && text[i++] !== "]")) fail("expected ]");
			const name = JSON.stringify(path);
			if (!array) {
				if (defined.has(name)) fail("table " + path.join(".") + " defined twice");
				defined.add(name);
			}
			table = walk(path, array);
			eol();
			continue;
		}
		const path = key();
		if (text[i++] !== "=") fail("expected =");
		assign(table, path, value());
		eol();
	}
}
export default { parse };
