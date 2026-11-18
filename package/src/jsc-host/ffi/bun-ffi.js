// bun:ffi on the Livewire JSC runtime. App bundles import this in place of
// Bun's module. A symbol the build generated glue for (the SDK's libraries,
// natives in __ffi from ffi/gen.ts) is called through that glue; any other
// symbol, and a JSCallback signature without generated trampolines, goes
// through libffi (ffi/dynamic.inc), bound at run time from its declared types.
const missing = () => {
	throw new Error("bun:ffi: the Livewire JSC host's ffi glue isn't installed");
};
// Lazily, so the build tools (ffi/extract-app.ts) can read the types under Bun.
const n = globalThis.__ffi ?? new Proxy({}, { get: missing });

// Bun's FFIType names, each to itself (bundles use them as strings).
const names = [
	"void", "bool", "char", "i8", "u8", "i16", "u16", "i32", "int", "c_int", "u32", "c_uint", "i64", "i64_fast", "isize", "u64", "u64_fast", "usize",
	"f32", "f64", "float", "double", "ptr", "pointer", "void*", "char*", "cstring", "function", "callback", "fn", "buffer",
	"int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t", "napi_env", "napi_value",
];
export const FFIType = Object.fromEntries(names.map((name) => [name, name]));
export const suffix = globalThis.process?.platform === "linux" ? "so" : "dylib";

// One name per type: what the glue's signature keys and the libffi natives use.
// A number is Bun's own FFIType value.
const byNumber = ["i8", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f64", "f32", "bool", "ptr", "void", "cstring", "i64_fast", "u64_fast", "function", "napi_env", "napi_value", "buffer"];
const aliases = {
	char: "i8", int8_t: "i8", uint8_t: "u8", int16_t: "i16", uint16_t: "u16", int: "i32", c_int: "i32", int32_t: "i32", c_uint: "u32", uint32_t: "u32",
	int64_t: "i64", isize: "i64", uint64_t: "u64", usize: "u64", float: "f32", double: "f64", pointer: "ptr", "void*": "ptr", "char*": "ptr",
	callback: "function", fn: "function",
};
export const canonicalType = (t) => (typeof t === "number" ? (byNumber[t] ?? String(t)) : (aliases[String(t)] ?? String(t)));
// The types the host calls and returns; an argument may also be a buffer (its
// bytes' address). A JSCallback takes at most 8 arguments.
export const SUPPORTED_TYPES = ["void", "bool", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "i64_fast", "u64_fast", "f32", "f64", "ptr", "cstring", "function"];
export const CALLBACK_MAX_ARGS = 8;
const argType = (t) => (canonicalType(t) === "buffer" ? "ptr" : canonicalType(t));

const sigKey = (args, returns) => (args ?? []).map(argType).join(",") + "->" + canonicalType(returns ?? "void");
// An argument declared cstring is a pointer in Bun (its C type is the same),
// and the glue's cstring slots take pointers: the two match as arguments.
const looseKey = (sig) => {
	const [args, returns] = String(sig).split("->");
	return args.split(",").map((t) => (t === "cstring" ? "ptr" : t)).join(",") + "->" + returns;
};
const libraryKey = (path) => String(path).split("/").pop().replace(/^lib/, "").replace(/\.(dylib|so|dll)$/, "");

export function dlopen(path, symbols) {
	const known = n.signatures(libraryKey(path)) ?? {};
	const wanted = Object.keys(symbols);
	const generated = wanted.filter((name) => known[name] !== undefined && looseKey(known[name]) === looseKey(sigKey(symbols[name].args, symbols[name].returns)));
	const opened = generated.length ? n.open(libraryKey(path), String(path), generated) : {};
	const out = {};
	for (const name of wanted) {
		const { args, returns } = symbols[name] ?? {};
		out[name] = opened[name] ?? n.bind(String(path), name, (args ?? []).map(argType), canonicalType(returns ?? "void"));
	}
	return { symbols: out, close() {} };
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
