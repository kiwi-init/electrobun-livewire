// A bigint out of 64-bit range wraps, as JavaScriptCore's bigint C API does, on
// the macOS 14 path too (LIVEWIRE_FFI_NO_BIGINT_API=1 forces it).
import "bun:ffi";
console.log([2n ** 64n + 5n, -1n].map((x) => String(globalThis.__ffi.ptr(x, 0))).join(" "));
// expect: 5 18446744073709552000
