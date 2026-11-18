// Bundles an app's main process for the Livewire JSC runtime: one classic
// script (IIFE) with bun:ffi and the Node builtins the runtime provides
// pointed at its shims, and the runtime's globals (Buffer, Bun.file)
// evaluated before the app's code. Runs with Bun at build time.
//   bun package/src/jsc-host/bundle.ts <entry> <outfile> [--minify]
//     [--alias electrobun/bun=/path/to/sdks/main/index.ts ...]
//     [--preload-dir <dir with preload-full.js and preload-sandboxed.js>]
//     [--define NAME=VALUE ...]  (as Bun.build's define: VALUE is code)
import type { OnLoadArgs, OnResolveArgs, PluginBuilder } from "bun";
import { join, resolve } from "node:path";

const here = import.meta.dir;
const flag = (name: string): string[] =>
	process.argv.flatMap((arg, i) => {
		const value = process.argv[i + 1];
		return arg === name && value ? [value] : [];
	});
const valueFlags = new Set(["--alias", "--preload-dir", "--define"]);
const [entry, outfile] = process.argv.slice(2).filter((a, i, all) => !a.startsWith("--") && !valueFlags.has(all[i - 1] ?? ""));
const aliases = Object.fromEntries(flag("--alias").map((pair) => pair.split("=") as [string, string]));
const preloadDir = flag("--preload-dir")[0];
const defines = Object.fromEntries(flag("--define").map((pair) => [pair.slice(0, pair.indexOf("=")), pair.slice(pair.indexOf("=") + 1)]));
if (!entry || !outfile) {
	console.error("usage: bundle.ts <entry> <outfile> [--minify]");
	process.exit(2);
}
const shims: Record<string, string> = {
	"bun:ffi": join(here, "ffi/bun-ffi.js"),
	"bun:sqlite": join(here, "node/sqlite.js"),
	fs: join(here, "node/fs.js"),
	"fs/promises": join(here, "node/fs-promises.js"),
	os: join(here, "node/os.js"),
	crypto: join(here, "node/crypto.js"),
	net: join(here, "node/net.js"),
	child_process: join(here, "node/child_process.js"),
	path: join(here, "node/path.js"),
	zlib: join(here, "node/zlib.js"),
	readline: join(here, "node/readline.js"),
	"stream/promises": join(here, "node/stream-promises.js"),
	url: join(here, "node/url.js"),
	worker_threads: join(here, "node/worker_threads.js"),
	tty: join(here, "node/tty.js"),
	dns: join(here, "node/dns-callback.js"),
	"dns/promises": join(here, "node/dns.js"),
	process: join(here, "node/process.js"),
	util: join(here, "node/util.js"),
};
const shimFor = (specifier: string) => shims[specifier.replace(/^node:/, "")];
// Bun's own bundles (target "bun") require builtins from CommonJS code with
// import.meta.require, which a script can't do at run time; each literal one
// becomes a static import of the shim (or Bun's browser fallback).
function staticRequires(source: string): string {
	const aliases = [...source.matchAll(/\b([A-Za-z_$][\w$]*)\s*=\s*import\.meta\.require\b/g)].map((m) => m[1]!);
	const callee = ["import\\.meta\\.require", ...aliases.map((a) => a.replace(/\$/g, "\\$"))].join("|");
	const calls = new RegExp("(?<![\\w$.])(?:" + callee + ")\\(\\s*([\"'])((?:node:)?[a-z_/]+)\\1\\s*\\)", "g");
	const imports = new Map<string, string>();
	const rewritten = source.replace(calls, (_match: string, _quote: string, specifier: string) => {
		const name = specifier.replace(/^node:/, "");
		if (!imports.has(name)) imports.set(name, "__livewireRequire" + imports.size);
		const id = imports.get(name)!;
		return "(" + id + ".default ?? " + id + ")";
	});
	if (!imports.size) return source;
	const head = [...imports].map(([name, id]) => "import * as " + id + " from " + JSON.stringify(shimFor(name) ?? name) + ";").join("\n");
	const bang = rewritten.startsWith("#!") ? rewritten.indexOf("\n") + 1 : 0;
	return rewritten.slice(0, bang) + head + "\n" + rewritten.slice(bang);
}

const build = (format: "iife" | "esm") => Bun.build({
	entrypoints: [join(here, "node/entry.js")],
	target: "browser",
	format,
	minify: process.argv.includes("--minify"),
	define: { "process.env.NODE_ENV": JSON.stringify("production"), ...defines },
	plugins: [
		{
			name: "livewire-jsc",
			setup(build: PluginBuilder) {
				build.onResolve({ filter: /^livewire-jsc-app$/ }, () => ({ path: resolve(entry) }));
				for (const [name, target] of Object.entries(aliases)) {
					const escaped = name.replace(/[.*+?^${}()|[\]\\/]/g, "\\$&");
					build.onResolve({ filter: new RegExp("^" + escaped + "$") }, () => ({ path: resolve(target) }));
				}
				// The SDK's generated preload module, from a release's preload files.
				build.onResolve({ filter: /preload\/\.generated\/compiled(\.ts)?$/ }, () => ({ path: "compiled", namespace: "livewire-preload" }));
				build.onLoad({ filter: /.*/, namespace: "livewire-preload" }, async () => {
					if (!preloadDir) throw new Error("bundle.ts: the SDK needs --preload-dir");
					const read = (name: string) => Bun.file(join(preloadDir, name)).text();
					return {
						contents: `export const preloadScript = ${JSON.stringify(await read("preload-full.js"))};
export const preloadScriptSandboxed = ${JSON.stringify(await read("preload-sandboxed.js"))};
export const appEnginePageScript = "";`,
						loader: "js",
					};
				});
				build.onResolve({ filter: /^(node:)?[a-z_/]+$|^bun:(ffi|sqlite)$/ }, (args: OnResolveArgs) => {
					// A shim's own import of the builtin gets Bun's browser polyfill.
					if (Object.values(shims).includes(args.importer)) return undefined;
					const shim = shimFor(args.path);
					// Others (path, events, buffer, util, url) use Bun's browser fallbacks.
					return shim ? { path: shim } : undefined;
				});
				build.onLoad({ filter: /\.(ts|tsx|js|mjs)$/ }, async (args: OnLoadArgs) => {
					const source = await Bun.file(args.path).text();
					if (Object.values(shims).includes(args.path)) return { contents: source, loader: "js" };
					// Builtins reach onResolve above; source text (a template string
					// holding code, say) is left as written.
					const contents = source
						// A browser build makes import.meta.main false; the app's entry is
						// the main module here, as when Bun runs it.
						.replace(/\bimport\.meta\.main\b/g, args.path === resolve(entry) ? "true" : "false");
					return { contents: staticRequires(contents), loader: args.path.endsWith(".tsx") ? "tsx" : args.path.endsWith(".ts") ? "ts" : "js" };
				});
			},
		},
	],
});
// A classic script can't await at its top level, so an app (or worker) that
// does is bundled as one ES module, with nothing imported, and run as the body
// of an async function: the same order as a module's top-level await. Its
// failure is an unhandled rejection, which ends the process with 1 as Bun's
// does.
const attempt = (format: "iife" | "esm") => build(format).catch((e: { errors?: unknown[] }) => ({ success: false, logs: e?.errors ?? [e], outputs: [] }));
let result = await attempt("iife");
const topLevelAwait = !result.success && result.logs.some((log) => String(log).includes('"await" can only be used inside an "async" function'));
if (topLevelAwait) result = await attempt("esm");
if (!result.success) {
	console.error(result.logs.join("\n"));
	process.exit(1);
}
const output = result.outputs[0];
if (!output) {
	console.error("bundle.ts: the bundle produced no output");
	process.exit(1);
}
let code = await output.text();
// A module's import.meta, for the bundle's file (Bun.main): a script has none,
// so references to it (Bun's own bundles use import.meta.dir and .url) read
// this object instead.
const usesImportMeta = /\bimport\.meta\b/.test(code);
const meta = `const __livewireImportMeta = (() => {
	const path = globalThis.Bun?.main ?? "";
	const dir = path.slice(0, path.lastIndexOf("/")) || "/";
	const require = (id) => { throw new Error("import.meta.require(" + JSON.stringify(id) + ") isn't available on the JSC host"); };
	return { url: "file://" + path, path, filename: path, dir, dirname: dir, file: path.slice(dir.length + 1), main: true, require, resolve: (s) => s, env: globalThis.process?.env ?? {} };
})();\n`;
if (usesImportMeta) code = code.replace(/\bimport\.meta\b/g, "__livewireImportMeta");
if (topLevelAwait) code = "(async () => {\n" + code.replace(/^\s*export\s*\{\s*\};?\s*$/m, "") + "\n})();\n";
if (usesImportMeta) code = meta + code;
await Bun.write(outfile, code);
