// Livewire: moves a Hutch-built app's main process from Bun to the system
// JavaScriptCore. Hutch builds the app as usual (mainProcess "bun"), then runs
// this postBuild hook from the devkit, before it signs and packages. The hook:
// - bundles the app's main entry for the JSC host (bundle.ts);
// - puts the devkit's prebuilt host at MacOS/main and removes Bun;
// - sets build.json's mainProcess to "jsc", which the launcher runs as a
//   native main process;
// - signs the host ad hoc with allow-jit, so dev builds JIT. When Hutch signs
//   the app, it signs the host again with the app's entitlements, so those
//   need allow-jit too.
// In electrobun.config.ts:
//   build: {
//     mainProcess: "bun",
//     bun: { entrypoint: "src/bun/index.ts" },
//     mac: { entitlements: { "com.apple.security.cs.allow-jit": true } },
//   },
//   scripts: { postBuild: ".hutch/devkit/api/jsc-host/post-build.ts" },
// LIVEWIRE_JSC_ENTRYPOINT overrides the entry read from the config.
import { chmodSync, copyFileSync, existsSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { ELECTROBUN_JAVASCRIPT_SDK_EXPORTS } from "../shared/native-devkit-manifest";

const here = import.meta.dir; // <devkit>/api/jsc-host
const devkitRoot = resolve(here, "../..");
const projectDir = process.cwd();

type AppLayout = { root: string; exeDir: string; resources: string };

function fail(message: string): never {
	console.error("livewire jsc: " + message);
	process.exit(1);
}

// The .app (macOS) or app folder (Linux) Hutch just assembled.
function findApp(buildDir: string): AppLayout | undefined {
	for (const name of readdirSync(buildDir)) {
		const root = join(buildDir, name);
		if (!statSync(root).isDirectory()) continue;
		const layouts: AppLayout[] = [
			{ root, exeDir: join(root, "Contents", "MacOS"), resources: join(root, "Contents", "Resources") },
			{ root, exeDir: join(root, "bin"), resources: join(root, "Resources") },
		];
		const found = layouts.find((l) => existsSync(join(l.resources, "build.json")) && existsSync(join(l.exeDir, "launcher")));
		if (found) return found;
	}
	return undefined;
}

async function entrypoint(): Promise<string> {
	const fromEnv = process.env["LIVEWIRE_JSC_ENTRYPOINT"];
	if (fromEnv) return fromEnv;
	const configPath = join(projectDir, "electrobun.config.ts");
	if (existsSync(configPath)) {
		const loaded: unknown = (await import(configPath)).default;
		const config: unknown = typeof loaded === "function" ? await loaded() : loaded;
		const entry = (config as { build?: { bun?: { entrypoint?: string } } } | undefined)?.build?.bun?.entrypoint;
		if (entry) return entry;
	}
	return "src/bun/index.ts";
}

const buildDir = process.env["ELECTROBUN_BUILD_DIR"] ?? fail("ELECTROBUN_BUILD_DIR isn't set: run this as a Hutch postBuild hook");
const os = process.env["ELECTROBUN_OS"] ?? "";
const app = findApp(buildDir) ?? fail("no app found in " + buildDir);
const buildJsonPath = join(app.resources, "build.json");
const buildJson = JSON.parse(readFileSync(buildJsonPath, "utf8")) as { mainProcess?: string; runtimeVersions?: Record<string, string> };
if (buildJson.mainProcess !== "bun") fail('build.mainProcess must be "bun" (the hook replaces Bun), not ' + JSON.stringify(buildJson.mainProcess));
const host = join(here, "main");
if (!existsSync(host)) fail("this devkit has no JSC host for " + os + " (" + host + ")");

// 1. The main process, bundled for the JSC host against this devkit's SDK.
const entry = await entrypoint();
const aliases = Object.entries(ELECTROBUN_JAVASCRIPT_SDK_EXPORTS).flatMap(([subpath, path]) => [
	"--alias",
	"electrobun" + subpath.slice(1) + "=" + join(devkitRoot, path),
]);
const bundled = Bun.spawnSync(
	[process.execPath, join(here, "bundle.ts"), entry, join(app.resources, "app", "bun", "index.js"), "--minify", ...aliases, "--preload-dir", app.resources],
	{ cwd: projectDir, stdout: "inherit", stderr: "inherit" },
);
if (bundled.exitCode !== 0) fail("bundling " + entry + " failed");

// 2. The host in place of Bun.
const main = join(app.exeDir, "main");
copyFileSync(host, main);
chmodSync(main, 0o755);
for (const path of [join(app.exeDir, "bun"), join(app.resources, "main.js")]) rmSync(path, { force: true });

// 3. The launcher runs MacOS/main for "jsc".
buildJson.mainProcess = "jsc";
if (buildJson.runtimeVersions) delete buildJson.runtimeVersions["bun"];
writeFileSync(buildJsonPath, JSON.stringify(buildJson));

// 4. JIT: JavaScriptCore runs about 7x slower without allow-jit (#344).
if (os === "macos") {
	const dir = mkdtempSync(join(tmpdir(), "livewire-jsc-"));
	const entitlements = join(dir, "jit.entitlements");
	writeFileSync(
		entitlements,
		'<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict><key>com.apple.security.cs.allow-jit</key><true/></dict></plist>\n',
	);
	const signed = Bun.spawnSync(["codesign", "--force", "-s", "-", "--entitlements", entitlements, main], { stdout: "inherit", stderr: "inherit" });
	rmSync(dir, { recursive: true, force: true });
	if (signed.exitCode !== 0) fail("codesign failed for " + main);
}
console.log("livewire jsc: " + app.root + " runs on the system JavaScriptCore (" + entry + ")");
