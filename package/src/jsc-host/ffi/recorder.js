// Stand-in for bun:ffi that records what the SDK declares instead of loading
// anything: every dlopen symbol table and every JSCallback signature.
const rec = (globalThis.__ffiRecord ??= { libraries: [], callbacks: [] });
const names = ["void", "bool", "char", "i8", "u8", "i16", "u16", "i32", "int", "u32", "i64", "i64_fast", "u64", "u64_fast", "f32", "f64", "float", "double", "ptr", "pointer", "cstring", "function", "callback", "napi_env", "napi_value", "buffer", "uint8_t", "int32_t", "uint32_t", "int64_t", "uint64_t", "usize"];
export const FFIType = Object.fromEntries(names.map((n) => [n, n]));
export const suffix = "dylib";
const where = () => (new Error().stack || "").split("\n").find((l) => l.includes("native.ts")) || "";
export function dlopen(path, symbols) {
	rec.libraries.push({ path: String(path), symbols: JSON.parse(JSON.stringify(symbols)) });
	const fns = {};
	for (const name of Object.keys(symbols)) fns[name] = () => 0;
	return { symbols: fns, close() {} };
}
export class JSCallback {
	constructor(fn, def) {
		rec.callbacks.push({ name: fn && fn.name, def: JSON.parse(JSON.stringify(def || {})), at: where() });
		this.ptr = rec.callbacks.length;
		this.threadsafe = !!(def && def.threadsafe);
	}
	close() {}
}
export class CString extends String { constructor(p) { super(""); this.ptr = p; } }
export const ptr = () => 1;
export const toArrayBuffer = () => new ArrayBuffer(0);
export const toBuffer = () => new Uint8Array(0);
export const read = new Proxy({}, { get: () => () => 0 });
export const viewSource = () => "";
export function cc() { throw new Error("cc is not supported"); }
export function linkSymbols(s) { return { symbols: s, close() {} }; }
