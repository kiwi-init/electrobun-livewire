// node:url on the Livewire JSC runtime: the WHATWG URL from the prelude plus
// the file-path helpers.
export const URL = globalThis.URL;
export const URLSearchParams = globalThis.URLSearchParams;
export function fileURLToPath(url) {
	const u = typeof url === "string" ? new URL(url) : url;
	if (u.protocol !== "file:") throw new TypeError("The URL must be of scheme file");
	return decodeURIComponent(u.pathname);
}
export function pathToFileURL(path) {
	const encoded = String(path).split("/").map((s) => encodeURIComponent(s)).join("/");
	return new URL("file://" + (encoded.startsWith("/") ? "" : "/") + encoded);
}
export default { URL, URLSearchParams, fileURLToPath, pathToFileURL };
