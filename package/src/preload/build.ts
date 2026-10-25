// Standalone build script for the preload.
// Normally this is run as part of "hutch build.ts", but you can run it directly:
//   hutch src/preload/build.ts

import { join, dirname } from "path";
import { writeFileSync, mkdirSync } from "fs";

async function buildPreload() {
	const preloadDir = dirname(import.meta.path);
	const outputDir = join(preloadDir, ".generated");
	const outputPath = join(outputDir, "compiled.ts");

	mkdirSync(outputDir, { recursive: true });

	// Build full preload (trusted webviews)
	const fullPreloadEntry = join(preloadDir, "index.ts");
	const fullResult = await Bun.build({
		entrypoints: [fullPreloadEntry],
		target: "browser",
		format: "esm",
		minify: false,
	});

	if (!fullResult.success) {
		console.error("Full preload build failed:", fullResult.logs);
		throw new Error("Failed to build full preload script");
	}

	// Build sandboxed preload (untrusted webviews)
	const sandboxedPreloadEntry = join(preloadDir, "index-sandboxed.ts");
	const sandboxedResult = await Bun.build({
		entrypoints: [sandboxedPreloadEntry],
		target: "browser",
		format: "esm",
		minify: false,
	});

	if (!sandboxedResult.success) {
		console.error("Sandboxed preload build failed:", sandboxedResult.logs);
		throw new Error("Failed to build sandboxed preload script");
	}

	// AppEngine's window.engine facade, injected as the custom preload of the
	// views AppEngine boots UIs in.
	const appEngineResult = await Bun.build({
		entrypoints: [join(preloadDir, "..", "browser", "appengine", "page.ts")],
		target: "browser",
		format: "esm",
		minify: false,
	});

	if (!appEngineResult.success) {
		console.error("AppEngine page build failed:", appEngineResult.logs);
		throw new Error("Failed to build the AppEngine page script");
	}

	// The compatible bundler does not currently support IIFE output, so wrap
	// the ESM bundle manually to keep preload globals scoped for injection.
	const fullPreloadJs = `(function(){${await fullResult.outputs[0]!.text()}})();`;
	const sandboxedPreloadJs = `(function(){${await sandboxedResult.outputs[0]!.text()}})();`;
	const appEnginePageJs = `(function(){${await appEngineResult.outputs[0]!.text()}})();`;

const outputContent = `// Auto-generated file. Do not edit directly.
// Run "hutch build.ts" from the package folder to regenerate.

// Full preload for trusted webviews (RPC, encryption, drag regions, webview tags)
export const preloadScript = ${JSON.stringify(fullPreloadJs)};

// Minimal preload for sandboxed/untrusted webviews (lifecycle events only, no RPC)
export const preloadScriptSandboxed = ${JSON.stringify(sandboxedPreloadJs)};

// AppEngine's window.engine facade (custom preload of AppEngine's views)
export const appEnginePageScript = ${JSON.stringify(appEnginePageJs)};
`;

	writeFileSync(outputPath, outputContent);
	console.log(`Preload scripts compiled to ${outputPath} (full + sandboxed + appengine)`);
}

buildPreload().catch((err) => {
	console.error("Failed to build preload:", err);
	process.exit(1);
});
