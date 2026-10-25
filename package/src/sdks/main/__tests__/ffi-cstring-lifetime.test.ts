import { describe, expect, test } from "bun:test";
import { dlopen, FFIType } from "bun:ffi";
import { toCString } from "../proc/ffiCString";

const libc = dlopen(
	process.platform === "darwin" ? "/usr/lib/libSystem.B.dylib" : process.platform === "win32" ? "msvcrt.dll" : "libc.so.6",
	{
		strcmp: { args: [FFIType.cstring, FFIType.cstring], returns: FFIType.i32 },
		strlen: { args: [FFIType.cstring], returns: FFIType.u64 },
	},
);

const secretKey = Array.from({ length: 32 }, (_, i) => String((i * 37) % 256)).join(",");

// What createWebview does: one cstring argument, then a large one (a preload)
// whose allocation can set off a GC before the call is made.
function churnThenRead(expected: string): Uint8Array {
	for (let i = 0; i < 50; i++) toCString(`views://internal/${i}`);
	const preload = toCString("x".repeat(200_000));
	Bun.gc(true);
	for (let i = 0; i < 500; i++) toCString(`persist:default ${i}`);
	void preload;
	return toCString(expected);
}

describe("toCString", () => {
	test("is NUL-terminated UTF-8, and keeps an explicit terminator single", () => {
		expect([...toCString("aé")]).toEqual([0x61, 0xc3, 0xa9, 0]);
		expect([...toCString("a\0")]).toEqual([0x61, 0]);
		expect([...toCString("ab", false)]).toEqual([0x61, 0x62]);
		expect(Number(libc.symbols.strlen(toCString("views://internal/index.html")))).toBe(27);
	});

	test("an argument stays intact when a later argument's allocation sets off a GC before the call (T1102)", () => {
		let changed = 0;
		for (let n = 0; n < 100; n++) {
			if (libc.symbols.strcmp(toCString(secretKey), churnThenRead(secretKey)) !== 0) changed++;
		}
		expect(changed).toBe(0);
	});
});
