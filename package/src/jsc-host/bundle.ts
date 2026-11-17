// Bundles an app's main process for the Livewire JSC runtime: one classic
// script (IIFE) with bun:ffi and the Node builtins the runtime provides
// pointed at its shims, and the runtime's globals (Buffer, Bun.file)
// evaluated before the app's code. Runs with Bun at build time.
//   bun package/src/jsc-host/bundle.ts <entry> <outfile> [--minify]
import type { OnLoadArgs, OnResolveArgs, PluginBuilder } from "bun";
import { join, resolve } from "node:path";

const here = import.meta.dir;
const [entry, outfile] = process.argv.slice(2).filter((a) => !a.startsWith("--"));
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
				build.onResolve({ filter: /^(node:)?[a-z_/]+$|^bun:ffi$/ }, (args: OnResolveArgs) => {
					const shim = shimFor(args.path);
					// Others (path, events, buffer, util, url) use Bun's browser fallbacks.
					return shim ? { path: shim } : undefined;
				});
				build.onLoad({ filter: /\.(ts|tsx|js|mjs)$/ }, async (args: OnLoadArgs) => {
					const source = await Bun.file(args.path).text();
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
