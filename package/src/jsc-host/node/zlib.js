// node:zlib lands with the updater work on the JSC runtime.
export function inflateSync() {
	throw new Error("zlib.inflateSync isn't available on the JSC runtime yet");
}
export default { inflateSync };
