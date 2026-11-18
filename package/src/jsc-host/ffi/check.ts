// Checks the SDK's bun:ffi declarations at build time, for the JSC host that
// calls them through libffi (natives.c, dynamic.inc) from their declared types:
// every dlopen table (recorded from proc/native.ts by extract.ts) and JSCallback
// signature must use a type the host binds (bun-ffi.js's SUPPORTED_TYPES), and
// with --check-lib, each such library must export what the SDK declares. So
// drift breaks the build, not the app. Runs with Bun:
//   bun package/src/jsc-host/ffi/check.ts [--sdk <sdks/main>] [--check-lib <lib>]... [--extra decls.json]...
// --sdk: the SDK to read (default: this checkout's). --extra: more
// declarations to check, as extract-app.ts writes them
// ({"libraries":[{"path","symbols"}],"callbacks":[{"args","returns"}]}).
import { readFileSync } from "node:fs";
import { basename, join } from "node:path";
import { CALLBACK_MAX_ARGS, canonicalType, SUPPORTED_TYPES } from "./bun-ffi.js";

const here = import.meta.dir;
const flagValues = (name: string): string[] =>
	process.argv.flatMap((arg, i) => {
		const value = process.argv[i + 1];
		return arg === name && value ? [value] : [];
	});
const sdkDir = flagValues("--sdk")[0] ?? join(here, "../../sdks/main");
const checkLibs = flagValues("--check-lib");
const extras = flagValues("--extra");

type Decl = { args?: string[]; returns?: string };
const extracted = Bun.spawnSync([process.execPath, join(here, "extract.ts"), sdkDir], { stderr: "inherit" });
if (extracted.exitCode !== 0) process.exit(1);
const record = JSON.parse(extracted.stdout.toString()) as { libraries: { path: string; symbols: Record<string, Decl> }[] };
const callbacks: { where: string; sig: Decl }[] = [];
for (const extraPath of extras) {
	const extra = JSON.parse(readFileSync(extraPath, "utf8"));
	record.libraries.push(...(extra.libraries ?? []));
	for (const sig of extra.callbacks ?? []) callbacks.push({ where: extraPath, sig });
}

// JSCallback signatures: read from the source, since some are created lazily
// (tray, wgpu input) and never run during extraction.
for (const file of new Bun.Glob("**/*.ts").scanSync(sdkDir)) {
	if (/\.test\.ts$|(^|\/)__tests__\//.test(file)) continue;
	const source = readFileSync(join(sdkDir, file), "utf8");
	if (!source.includes("new JSCallback(")) continue;
	const lines = source.split("\n");
	const sites = lines.flatMap((line, i) => (line.includes("new JSCallback(") && !line.trim().startsWith("//") ? [i] : []));
	for (const [n, start] of sites.entries()) {
		const chunk = lines.slice(start, sites[n + 1] ?? lines.length).join("\n");
		const m = chunk.match(/args:\s*\[([^\]]*)\]\s*,\s*returns:\s*([^,\n}]+)/s);
		if (!m) {
			console.error("check: can't read the JSCallback signature at " + file + ":" + (start + 1));
			process.exit(1);
		}
		const norm = (t: string) => t.replace(/\/\/.*$/gm, "").trim().replace(/^FFIType\./, "").replace(/["']/g, "");
		callbacks.push({ where: file + ":" + (start + 1), sig: { args: (m[1] ?? "").split(",").map(norm).filter(Boolean), returns: norm(m[2] ?? "void") } });
	}
}

const supported = new Set<string>(SUPPORTED_TYPES);
const takes = (t: string, callback: boolean) => {
	const c = canonicalType(t);
	return c !== "void" && (supported.has(c) || (c === "buffer" && !callback));
};
const problems: string[] = [];
for (const lib of record.libraries) {
	for (const [name, d] of Object.entries(lib.symbols)) {
		for (const t of d.args ?? []) if (!takes(t, false)) problems.push(basename(lib.path) + ": " + name + " takes a " + t);
		if (!supported.has(canonicalType(d.returns ?? "void"))) problems.push(basename(lib.path) + ": " + name + " returns a " + d.returns);
	}
}
for (const { where, sig } of callbacks) {
	const args = sig.args ?? [];
	for (const t of args) if (!takes(t, true)) problems.push(where + ": a JSCallback takes a " + t);
	if (!supported.has(canonicalType(sig.returns ?? "void"))) problems.push(where + ": a JSCallback returns a " + sig.returns);
	if (args.length > CALLBACK_MAX_ARGS) problems.push(where + ": a JSCallback takes " + args.length + " arguments (at most " + CALLBACK_MAX_ARGS + ")");
}

for (const libPath of checkLibs) {
	const lib = record.libraries.find((l) => basename(l.path) === basename(libPath));
	if (!lib) continue;
	// Exports: Mach-O's global defined symbols; an ELF .so's dynamic table (its
	// .symtab may be stripped).
	const nm = Bun.spawnSync(process.platform === "darwin" ? ["nm", "-gU", libPath] : ["nm", "-D", "--defined-only", libPath]);
	const exported = new Set(nm.stdout.toString().split("\n").map((line: string) => line.trim().split(/\s+/).pop()?.replace(/^_/, "")));
	const missing = Object.keys(lib.symbols).filter((name) => !exported.has(name));
	if (missing.length) problems.push(basename(libPath) + " doesn't export " + missing.join(", "));
}

if (problems.length) {
	console.error("check: the JSC host can't call these bun:ffi declarations:\n  " + problems.join("\n  "));
	process.exit(1);
}
const symbols = record.libraries.reduce((n, l) => n + Object.keys(l.symbols).length, 0);
console.error("check: " + record.libraries.map((l) => basename(l.path)).join(", ") + ": " + symbols + " symbols and " + callbacks.length + " JSCallbacks use types the host binds");
