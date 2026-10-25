import { CString, type Pointer } from "bun:ffi";

/**
 * Bun 1.4 decodes `FFIType.cstring` callback arguments to JavaScript strings.
 * Cottontail and earlier Bun releases expose the same arguments as pointers.
 */
export type FFICStringCallbackValue = string | Pointer | null;

export function ffiCStringToString(value: FFICStringCallbackValue): string {
	if (typeof value === "string") return value;
	return value ? new CString(value).toString() : "";
}

const cStringEncoder = new TextEncoder();

/**
 * A NUL-terminated UTF-8 copy of a string for an `FFIType.cstring` argument.
 * It returns the bytes, not `ptr(bytes)`: Bun keeps a typed array passed to
 * FFI alive until the call returns, while a bare pointer number leaves the
 * bytes unreachable, so a GC set off by a later argument (a large preload,
 * say) can free or reuse them before native code reads them.
 */
export function toCString(jsString: string, addNullTerminator: boolean = true): Uint8Array<ArrayBuffer> {
	const appendWith = addNullTerminator && !jsString.endsWith("\0") ? "\0" : "";
	return cStringEncoder.encode(jsString + appendWith);
}
