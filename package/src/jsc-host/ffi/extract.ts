// Records the SDK's bun:ffi declarations (proc/native.ts) as JSON: run with
// Bun at build time. check.ts checks these against the types the JSC host
// binds (and the core libraries' exports), so drift fails the build.
//   bun package/src/jsc-host/ffi/extract.ts [sdks/main dir] > ffi-decls.json
import type { OnLoadArgs, PluginBuilder } from "bun";
import { join } from "node:path";

const here = import.meta.dir;
const result = await Bun.build({
	entrypoints: [join(process.argv[2] ?? join(here, "../../sdks/main"), "proc/native.ts")],
	target: "bun",
	format: "esm",
	plugins: [
		{
			name: "ffi-recorder",
			setup(build: PluginBuilder) {
				// bun:ffi is a builtin for target "bun", so onResolve never sees it:
				// point the SDK's imports at the recorder in the source instead.
				build.onLoad({ filter: /sdks\/main\/.*\.ts$/ }, async (args: OnLoadArgs) => ({
					contents: (await Bun.file(args.path).text()).replaceAll('"bun:ffi"', JSON.stringify(join(here, "recorder.js"))),
					loader: "ts",
				}));
				build.onResolve({ filter: /preload\/\.generated\/compiled$/ }, () => ({ path: "stub-preload", namespace: "stub" }));
				build.onLoad({ filter: /.*/, namespace: "stub" }, () => ({
					contents: "export const preloadScript = ''; export const preloadScriptSandboxed = ''; export const appEnginePageScript = '';",
					loader: "js",
				}));
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
	console.error("extract: the bundle produced no output");
	process.exit(1);
}
const bundled = await output.text();
if (process.env["DEBUG"]) console.error("recorder in bundle:", bundled.includes("__ffiRecord"));
const bundle = join(process.env["TMPDIR"] || "/tmp", `ffi-extract-${process.pid}.js`);
await Bun.write(bundle, bundled);
process.env["ELECTROBUN_FFI_EXTRACT"] = "1";
try {
	await import(bundle);
} catch (error) {
	// Top-level code past the declarations may throw on the stubs; the record is
	// complete by then, and an empty record fails below.
	if (process.env["DEBUG"]) console.error(error);
}
const record = (globalThis as { __ffiRecord?: { libraries: unknown[]; callbacks: unknown[] } }).__ffiRecord;
if (!record || record.libraries.length === 0) {
	console.error("extract: no dlopen declarations recorded");
	process.exit(1);
}
process.stdout.write(JSON.stringify(record, null, 1));
process.exit(0);
