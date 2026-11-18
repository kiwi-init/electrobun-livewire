// expect: add 5
// expect: mul 7.5
// expect: big 1099511627781 bigint
// expect: u64 echo 18446744073709551615
// expect: not false
// expect: strlen 5
// expect: greet hello, jsc string
// expect: null string null
// expect: fill 0,3,6,9
// expect: pair 1.5 -2.25
// expect: ptr echo ok
// expect: after threadsafe call
// expect: ret cb 1
// expect: str cb [str 3]
// expect: str via thread [str 9] fast
// expect: dyn mix -1095511334283 bigint
// expect: dyn i8 -56 u16 65535 f32 2.5 bool true false
// expect: dyn i64 -4611686018427387905 bigint
// expect: dyn u64_fast 42 number 9223372036854775808 bigint
// expect: dyn cb args bigint -5 1.5 200 mix
// expect: dyn cb 21
// expect: dyn cb via thread 106 fast
// expect: callback past 16 slots 7 sync
// expect: redeclared binds as declared -56
// expect: sync cb 7 sync
// expect: thread cb 20 from thread 20
// expect: closed
// expect: dyn thread cb 7 0.25
// The bun:ffi shim over the generated glue, against tests/ffi/testlib.c, and
// over libffi for what the glue doesn't have (the tl_dyn_* symbols, which
// tests/ffi/decls.json leaves out, as an app's own library).
import { dlopen, FFIType, JSCallback, CString, ptr, toArrayBuffer, read } from "../../ffi/bun-ffi.js";

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
	tl_str_via_thread: { args: [FFIType.function], returns: FFIType.cstring },
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

const syncCb = new JSCallback((id, text) => console.log("sync cb", id, text), { args: ["u32", FFIType.cstring], returns: "void", threadsafe: true });
lib.tl_call_sync(syncCb.ptr);
console.log("after threadsafe call");
const retCb = new JSCallback((id) => id, { args: [FFIType.u32, FFIType.cstring], returns: FFIType.u32, threadsafe: true });
console.log("ret cb", lib.tl_call_ret(retCb));
let keep;
const strCb = new JSCallback((n) => { keep = cstr("[str " + n + "]"); return ptr(keep); }, { args: [FFIType.u32], returns: FFIType.cstring });
console.log("str cb", lib.tl_call_str(strCb).toString());
// The callback runs while JS waits in tl_str_via_thread, as under Bun: no 3 s
// wait for a JS thread that is waiting for it (T416).
const started = performance.now();
const viaThread = lib.tl_str_via_thread(strCb).toString();
console.log("str via thread", viaThread, performance.now() - started < 1000 ? "fast" : "slow");

const dyn = dlopen(process.env.LIVEWIRE_TESTLIB, {
	tl_dyn_mix: { args: ["i8", "u8", "i16", "u16", "i32", "u32", "i64", "f32", "f64", "cstring", "char", "uint16_t"], returns: "i64" },
	tl_dyn_i8: { args: ["int"], returns: "i8" },
	tl_dyn_u16: { args: ["i32"], returns: "u16" },
	tl_dyn_f32: { args: ["float"], returns: "f32" },
	tl_dyn_i64: { args: ["i64"], returns: "i64" },
	tl_dyn_u64: { args: ["u64"], returns: "u64_fast" },
	tl_dyn_bool: { args: ["u16"], returns: "bool" },
	tl_dyn_call_mix: { args: ["function"], returns: "f64" },
	tl_dyn_i8_via_thread: { args: ["function"], returns: "i32" },
	tl_dyn_call_thread: { args: ["callback", "i16"], returns: "void" },
}).symbols;
const mixed = dyn.tl_dyn_mix(-1, 255, -300, 65535, -70000, 4000000000, -(2n ** 40n), 0.5, -0.25, cstr("four"), -2, 3);
console.log("dyn mix", String(mixed), typeof mixed);
console.log("dyn i8", dyn.tl_dyn_i8(200), "u16", dyn.tl_dyn_u16(-1), "f32", dyn.tl_dyn_f32(1.25), "bool", dyn.tl_dyn_bool(2), dyn.tl_dyn_bool(1));
console.log("dyn i64", String(dyn.tl_dyn_i64(-(2n ** 62n))), typeof dyn.tl_dyn_i64(5));
console.log("dyn u64_fast", dyn.tl_dyn_u64(42), typeof dyn.tl_dyn_u64(42), String(dyn.tl_dyn_u64(2n ** 63n)), typeof dyn.tl_dyn_u64(2n ** 63n));
// Callbacks of signatures the glue has no trampolines for: libffi closures.
const mixCb = new JSCallback((a, b, c, s) => { console.log("dyn cb args", typeof a, String(a), b, c, String(s)); return 10.5; }, { args: ["i64", "f32", "u8", "cstring"], returns: "f64" });
console.log("dyn cb", dyn.tl_dyn_call_mix(mixCb));
const i8Cb = new JSCallback((v) => v * 50, { args: ["i8"], returns: "i8" });
const i8Started = performance.now();
const i8Out = dyn.tl_dyn_i8_via_thread(i8Cb.ptr);
console.log("dyn cb via thread", i8Out, performance.now() - i8Started < 1000 ? "fast" : "slow");
// A signature's 16 generated slots taken: the rest are libffi closures.
const many = Array.from({ length: 20 }, (_, i) => new JSCallback((id, text) => console.log("callback past 16 slots", id, text), { args: ["u32", "cstring"], returns: "void" }));
lib.tl_call_sync(many[19].ptr);
for (const cb of many) cb.close();
// A symbol the glue has under another signature is bound as declared, as Bun does.
const redeclared = dlopen(process.env.LIVEWIRE_TESTLIB, { tl_add: { args: ["i32", "i32"], returns: "i8" } }).symbols;
console.log("redeclared binds as declared", redeclared.tl_add(100, 100));

const threadCb = new JSCallback(
	(id, text) => {
		console.log("thread cb", id, text);
		threadCb.close();
		console.log("closed");
		dyn.tl_dyn_call_thread(pairCb.ptr, 7);
	},
	{ args: ["u32", FFIType.cstring], returns: "void", threadsafe: true },
);
const pairCb = new JSCallback(
	(v, x) => {
		console.log("dyn thread cb", v, x);
		pairCb.close();
		clearTimeout(keepAlive);
	},
	{ args: ["i16", "f64"], returns: "void", threadsafe: true },
);
lib.tl_call_thread(threadCb.ptr, 20);
const keepAlive = setTimeout(() => console.log("thread callback never came"), 2000);
