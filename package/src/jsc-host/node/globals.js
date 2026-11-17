// Globals an app bundle gets on the Livewire JSC runtime, before its own
// code: Buffer, and the slice of Bun's API the SDK uses.
import { Buffer } from "buffer";
import { readFileSync, existsSync, statSync } from "./fs.js";
import { getRandomValues, randomUUID } from "./crypto.js";

// Defined, not assigned: the package typecheck reads this file (allowJs), and
// an assignment to globalThis.Bun or .Buffer would redeclare those globals' types.
const defineGlobal = (name, value) =>
	Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
defineGlobal("Buffer", Buffer);
globalThis.crypto ??= { getRandomValues, randomUUID };

const mime = {
	html: "text/html", htm: "text/html", js: "text/javascript", mjs: "text/javascript", css: "text/css", json: "application/json",
	svg: "image/svg+xml", png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", webp: "image/webp", ico: "image/x-icon",
	woff: "font/woff", woff2: "font/woff2", ttf: "font/ttf", otf: "font/otf", wasm: "application/wasm", txt: "text/plain", map: "application/json",
	mp3: "audio/mpeg", mp4: "video/mp4", webm: "video/webm", wav: "audio/wav", pdf: "application/pdf", xml: "application/xml",
};
class BunFile {
	constructor(path) {
		this.name = String(path);
		const ext = this.name.split(".").pop().toLowerCase();
		this.type = (mime[ext] ?? "application/octet-stream") + (/^(text\/|application\/(json|javascript|xml))/.test(mime[ext] ?? "") ? ";charset=utf-8" : "");
	}
	get size() { try { return statSync(this.name).size; } catch { return 0; } }
	async exists() { return existsSync(this.name); }
	async text() { return readFileSync(this.name, "utf8"); }
	async json() { return JSON.parse(readFileSync(this.name, "utf8")); }
	async arrayBuffer() { const b = readFileSync(this.name); return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); }
	async bytes() { return new Uint8Array(await this.arrayBuffer()); }
}
const missing = (name) => () => {
	throw new Error(`Bun.${name} isn't available on the JSC runtime yet`);
};
if (!("Bun" in globalThis)) defineGlobal("Bun", {
	file: (path) => new BunFile(path),
	spawn: missing("spawn"),
	spawnSync: missing("spawnSync"),
	which: missing("which"),
	env: globalThis["process"]?.env ?? {},
	version: "0.0.0-livewire-jsc",
});
