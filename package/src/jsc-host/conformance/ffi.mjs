import { dlopen, FFIType, JSCallback, CString, ptr, toArrayBuffer, read } from "bun:ffi";

const lib = dlopen(process.env.LIVEWIRE_TESTLIB, {
	tl_add: { args: [FFIType.i32, FFIType.i32], returns: FFIType.i32 },
	tl_mul: { args: [FFIType.f64, FFIType.f64], returns: FFIType.f64 },
	tl_big: { args: [], returns: FFIType.u64 },
	tl_u64_echo: { args: [FFIType.u64], returns: FFIType.u64 },
	tl_not: { args: [FFIType.bool], returns: FFIType.bool },
	tl_strlen: { args: [FFIType.cstring], returns: FFIType.u32 },
	tl_greet: { args: [FFIType.cstring], returns: FFIType.cstring },
	tl_null_string: { args: [], returns: FFIType.cstring },
	tl_echo_ptr: { args: [FFIType.ptr], returns: FFIType.ptr },
	tl_fill: { args: [FFIType.ptr, FFIType.u32], returns: FFIType.void },
	tl_write_pair: { args: [FFIType.ptr, FFIType.ptr], returns: FFIType.void },
	tl_call_sync: { args: [FFIType.function], returns: FFIType.void },
	tl_call_ret: { args: [FFIType.function], returns: FFIType.u32 },
	tl_call_str: { args: [FFIType.function], returns: FFIType.cstring },
	tl_call_thread: { args: [FFIType.function, FFIType.u32], returns: FFIType.void },
}).symbols;

const cstr = (s) => new TextEncoder().encode(s + "\0");
console.log("add", lib.tl_add(2, 3));
console.log("mul", lib.tl_mul(2.5, 3));
const big = lib.tl_big();
console.log("big", String(big), typeof big);
console.log("u64 echo", String(lib.tl_u64_echo(18446744073709551615n)));
console.log("not", lib.tl_not(true));
console.log("strlen", lib.tl_strlen(cstr("hello")));
const greeting = lib.tl_greet(cstr("jsc"));
console.log("greet", greeting.toString(), greeting instanceof CString ? "CString" : typeof greeting);
console.log("null string", lib.tl_null_string());
const buf = new Uint8Array(4);
lib.tl_fill(buf, 4);
console.log("fill", Array.from(buf).join(","));
const x = new Float64Array(1), y = new Float64Array(1);
lib.tl_write_pair(ptr(x), ptr(y));
console.log("pair", x[0], y[0]);
const p = ptr(buf);
console.log("ptr echo", lib.tl_echo_ptr(p) === p && read.u8(p, 1) === 3 && new Uint8Array(toArrayBuffer(p, 0, 4))[3] === 9 ? "ok" : "bad");

// Every number type, more arguments than registers, odd callback signatures.
const dyn = dlopen(process.env.LIVEWIRE_TESTLIB, {
	tl_dyn_mix: { args: ["i8", "u8", "i16", "u16", "i32", "u32", "i64", "f32", "f64", "cstring", "char", "uint16_t"], returns: "i64" },
	tl_dyn_i8: { args: ["int"], returns: "i8" },
	tl_dyn_u16: { args: ["i32"], returns: "u16" },
	tl_dyn_f32: { args: ["float"], returns: "f32" },
	tl_dyn_i64: { args: ["i64"], returns: "i64" },
	tl_dyn_u64: { args: ["u64"], returns: "u64_fast" },
	tl_dyn_bool: { args: ["u16"], returns: "bool" },
	tl_dyn_call_mix: { args: ["function"], returns: "f64" },
	tl_dyn_call_thread: { args: ["callback", "i16"], returns: "void" },
}).symbols;
const mixed = dyn.tl_dyn_mix(-1, 255, -300, 65535, -70000, 4000000000, -(2n ** 40n), 0.5, -0.25, cstr("four"), -2, 3);
console.log("dyn mix", String(mixed), typeof mixed);
console.log("dyn i8", dyn.tl_dyn_i8(200), "u16", dyn.tl_dyn_u16(-1), "f32", dyn.tl_dyn_f32(1.25), "bool", dyn.tl_dyn_bool(2), dyn.tl_dyn_bool(1));
console.log("dyn i64", String(dyn.tl_dyn_i64(-(2n ** 62n))), typeof dyn.tl_dyn_i64(5));
console.log("dyn u64_fast", dyn.tl_dyn_u64(42), typeof dyn.tl_dyn_u64(42), String(dyn.tl_dyn_u64(2n ** 63n)), typeof dyn.tl_dyn_u64(2n ** 63n));
const mixCb = new JSCallback((a, b, c, s) => { console.log("dyn cb args", typeof a, String(a), b, c, String(s)); return 10.5; }, { args: ["i64", "f32", "u8", "cstring"], returns: "f64" });
console.log("dyn cb", dyn.tl_dyn_call_mix(mixCb));
const redeclared = dlopen(process.env.LIVEWIRE_TESTLIB, { tl_add: { args: ["i32", "i32"], returns: "i8" } }).symbols;
console.log("redeclared binds as declared", redeclared.tl_add(100, 100));

const syncCb = new JSCallback((id, text) => console.log("sync cb", id, text), { args: ["u32", FFIType.cstring], returns: "void", threadsafe: true });
lib.tl_call_sync(syncCb.ptr);
console.log("after threadsafe call");
const retCb = new JSCallback((id) => id, { args: [FFIType.u32, FFIType.cstring], returns: FFIType.u32 });
console.log("ret cb", lib.tl_call_ret(retCb));
let keep;
const strCb = new JSCallback((n) => { keep = cstr("[str " + n + "]"); return ptr(keep); }, { args: [FFIType.u32], returns: FFIType.cstring });
console.log("str cb", lib.tl_call_str(strCb).toString());

const threadCb = new JSCallback(
	(id) => {
		console.log("thread cb", id);
		setTimeout(() => {
			threadCb.close();
			console.log("closed");
			clearTimeout(keepAlive);
		}, 0);
	},
	{ args: ["u32", FFIType.cstring], returns: "void", threadsafe: true },
);
lib.tl_call_thread(threadCb.ptr, 20);
const keepAlive = setTimeout(() => console.log("thread callback never came"), 2000);
