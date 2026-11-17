// bun:ffi on the Livewire JSC runtime. App bundles import this in place of
// Bun's module; the natives (__ffi) come from glue generated at build time, so
// only libraries and callback signatures known at build time work here.
const n = globalThis.__ffi;
if (!n) throw new Error("bun:ffi: the Livewire JSC host's ffi glue isn't installed");

const names = ["void", "bool", "char", "i8", "u8", "i16", "u16", "i32", "int", "u32", "i64", "i64_fast", "u64", "u64_fast", "f32", "f64", "float", "double", "ptr", "pointer", "cstring", "function", "callback", "buffer"];
export const FFIType = Object.fromEntries(names.map((name) => [name, name]));
export const suffix = globalThis.process?.platform === "linux" ? "so" : "dylib";

const canonical = { int: "i32", int32_t: "i32", uint32_t: "u32", pointer: "ptr", callback: "function", double: "f64", uint64_t: "u64" };
const canon = (t) => canonical[String(t)] ?? String(t);
const sigKey = (args, returns) => (args ?? []).map(canon).join(",") + "->" + canon(returns ?? "void");
const libraryKey = (path) => String(path).split("/").pop().replace(/^lib/, "").replace(/\.(dylib|so|dll)$/, "");

export function dlopen(path, symbols) {
	const key = libraryKey(path);
	const known = n.signatures(key);
	if (!known) throw new Error(`bun:ffi: ${key} has no generated glue in this build`);
	const wanted = Object.keys(symbols);
	for (const name of wanted) {
		const sig = sigKey(symbols[name].args, symbols[name].returns);
		if (known[name] !== sig) {
			throw new Error(`bun:ffi: ${key}.${name} is declared as ${sig} but the build generated ${known[name] ?? "nothing"}`);
		}
	}
	return { symbols: n.open(key, String(path), wanted), close() {} };
}

export class JSCallback {
	constructor(fn, definition = {}) {
		if (typeof fn !== "function") throw new TypeError("JSCallback: expected a function");
		this.threadsafe = !!definition.threadsafe;
		this.ptr = n.callbackCreate(sigKey(definition.args, definition.returns), fn, this.threadsafe);
	}
	close() {
		if (this.ptr) n.callbackClose(this.ptr);
		this.ptr = null;
	}
}

export class CString extends String {
	constructor(ptr, byteOffset = 0, byteLength) {
		super(ptr ? n.readCString(ptr, byteOffset, byteLength ?? -1) : "");
		this.ptr = ptr;
		this.byteOffset = byteOffset;
		this.byteLength = byteLength ?? this.length;
	}
}
n.setCStringClass(CString);

export const ptr = (view, byteOffset = 0) => n.ptr(view, byteOffset);
export const toArrayBuffer = (pointer, byteOffset = 0, byteLength) => n.toArrayBuffer(pointer, byteOffset, byteLength ?? -1);
export const toBuffer = (pointer, byteOffset = 0, byteLength) => new Uint8Array(toArrayBuffer(pointer, byteOffset, byteLength));
export const read = Object.fromEntries(
	["u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "f32", "f64", "ptr", "intptr"].map((kind) => [kind, (pointer, byteOffset = 0) => n.read(kind, pointer, byteOffset)]),
);
export function cc() {
	throw new Error("bun:ffi cc() isn't available on the JSC runtime");
}
export function linkSymbols() {
	throw new Error("bun:ffi linkSymbols() isn't available on the JSC runtime");
}
export const viewSource = () => "";
export default { FFIType, suffix, dlopen, JSCallback, CString, ptr, toArrayBuffer, toBuffer, read, cc, linkSymbols, viewSource };
