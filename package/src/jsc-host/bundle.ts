// Bundles an app's main process for the Livewire JSC runtime: one classic
// script (IIFE) with bun:ffi and the Node builtins the runtime provides
// pointed at its shims, and the runtime's globals (Buffer, Bun.file)
// evaluated before the app's code. Runs with Bun at build time.
//   bun package/src/jsc-host/bundle.ts <entry> <outfile> [--minify]
//     [--alias electrobun/bun=/path/to/sdks/main/index.ts ...]
//     [--preload-dir <dir with preload-full.js and preload-sandboxed.js>]
import type { OnLoadArgs, OnResolveArgs, PluginBuilder } from "bun";
import { join, resolve } from "node:path";

const here = import.meta.dir;
const flag = (name: string): string[] =>
	process.argv.flatMap((arg, i) => {
		const value = process.argv[i + 1];
		return arg === name && value ? [value] : [];
	});
const valueFlags = new Set(["--alias", "--preload-dir"]);
const [entry, outfile] = process.argv.slice(2).filter((a, i, all) => !a.startsWith("--") && !valueFlags.has(all[i - 1] ?? ""));
const aliases = Object.fromEntries(flag("--alias").map((pair) => pair.split("=") as [string, string]));
const preloadDir = flag("--preload-dir")[0];
if (!entry || !outfile) {
	console.error("usage: bundle.ts <entry> <outfile> [--minify]");
	process.exit(2);
}
const shims: Record<string, string> = {
	"bun:ffi": join(here, "ffi/bun-ffi.js"),
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
};
const shimFor = (specifier: string) => shims[specifier.replace(/^node:/, "")];
// For target "bun", builtins never reach a plugin's onResolve, so imports
// are rewritten in the source as it loads.
const importPattern = /(from\s*|import\s*\(\s*|import\s+|require\s*\(\s*)(["'])((?:node:)?[a-z_/]+|bun:ffi)\2/g;

const result = await Bun.build({
	entrypoints: [join(here, "node/entry.js")],
	target: "browser",
	format: "iife",
	minify: process.argv.includes("--minify"),
	define: { "process.env.NODE_ENV": JSON.stringify("production") },
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
				build.onResolve({ filter: /^(node:)?[a-z_/]+$|^bun:ffi$/ }, (args: OnResolveArgs) => {
					// A shim's own import of the builtin gets Bun's browser polyfill.
					if (Object.values(shims).includes(args.importer)) return undefined;
					const shim = shimFor(args.path);
					// Others (path, events, buffer, util, url) use Bun's browser fallbacks.
					return shim ? { path: shim } : undefined;
				});
				build.onLoad({ filter: /\.(ts|tsx|js|mjs)$/ }, async (args: OnLoadArgs) => {
					const source = await Bun.file(args.path).text();
					if (Object.values(shims).includes(args.path)) return { contents: source, loader: "js" };
					const contents = source.replace(importPattern, (match: string, lead: string, quote: string, specifier: string) => {
						const shim = shimFor(specifier);
						return shim ? lead + quote + shim + quote : match;
					});
					return { contents, loader: args.path.endsWith(".tsx") ? "tsx" : args.path.endsWith(".ts") ? "ts" : "js" };
				});
			},
		},
	],
});
if (!result.success) {
	console.error(result.logs.join("\n"));
	process.exit(1);
}
const output = result.outputs[0];
if (!output) {
	console.error("bundle.ts: the bundle produced no output");
	process.exit(1);
}
await Bun.write(outfile, await output.text());
