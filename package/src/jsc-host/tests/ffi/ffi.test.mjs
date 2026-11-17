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
// expect: sync cb 7 sync
// expect: thread cb 20 from thread 20
// expect: closed
// expect: mismatch rejected
// The bun:ffi shim over the generated glue, against tests/ffi/testlib.c.
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

const threadCb = new JSCallback(
	(id, text) => {
		console.log("thread cb", id, text);
		threadCb.close();
		console.log("closed");
		try {
			dlopen(process.env.LIVEWIRE_TESTLIB, { tl_add: { args: [FFIType.f64], returns: FFIType.i32 } });
		} catch (e) {
			console.log(String(e.message).includes("declared as f64->i32") ? "mismatch rejected" : e.message);
		}
		clearTimeout(keepAlive);
	},
	{ args: ["u32", FFIType.cstring], returns: "void", threadsafe: true },
);
lib.tl_call_thread(threadCb.ptr, 20);
const keepAlive = setTimeout(() => console.log("thread callback never came"), 2000);
