// The engine numbers on macOS and Linux: the same main process (workload.js,
// as in the Windows spike) on Bun and on this OS's JSC host. Launch to the
// first JS line, idle memory, size, workload timings and native-call cost.
//   bun package/test-apps/engine-numbers/measure.ts [out dir]
import { mkdirSync, readFileSync, realpathSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const here = import.meta.dir;
const hostDir = join(here, "../../src/jsc-host");
const out = process.argv[2] ?? join(tmpdir(), "engine-numbers");
mkdirSync(out, { recursive: true });
if (Bun.spawnSync(["sh", join(hostDir, "build.sh"), out], { stdout: "inherit", stderr: "inherit" }).exitCode !== 0) process.exit(1);
const lib = join(out, "libjsctest." + (process.platform === "darwin" ? "dylib" : "so"));
const env = { ...process.env, LIVEWIRE_TESTLIB: lib };

const workload = readFileSync(join(here, "workload.js"), "utf8");
const adapter = `const lib = dlopen(process.env.LIVEWIRE_TESTLIB, { tl_add: { args: ["i32", "i32"], returns: "i32" } });
const add = lib.symbols.tl_add;
const host = { echo: (n) => add(n, 0), now: () => performance.now(), print: (s) => console.log(s) };
host.print("BOOT");
const result = IDLE ? {} : workload(host);
host.print("READY " + JSON.stringify(result));
setTimeout(() => {}, 6000);
`;
const mains: Record<string, string[]> = {};
for (const mode of ["idle", "work"]) {
	const source = `import { dlopen } from "bun:ffi";\nconst IDLE = ${mode === "idle"};\n${workload}\n${adapter}`;
	const mjs = join(out, `main-${mode}.mjs`);
	writeFileSync(mjs, source);
	const bundled = join(out, `main-${mode}.jsc.js`);
	if (Bun.spawnSync([process.execPath, join(hostDir, "bundle.ts"), mjs, bundled, "--minify"], { stderr: "inherit" }).exitCode !== 0) process.exit(1);
	mains[`bun-${mode}`] = [process.execPath, mjs];
	mains[`jsc-${mode}`] = [join(out, "jsc-run"), bundled];
}

function memory(pid: number) {
	if (process.platform === "darwin") {
		const fp = Bun.spawnSync(["footprint", String(pid)]).stdout.toString();
		const m = fp.match(/Footprint:\s+([\d.]+)\s*(KB|MB|GB)/);
		const footprint = m ? Number(m[1]) * { KB: 1 / 1024, MB: 1, GB: 1024 }[m[2] as "KB"] : NaN;
		const rss = Number(Bun.spawnSync(["ps", "-o", "rss=", "-p", String(pid)]).stdout.toString().trim()) / 1024;
		return { private_mb: footprint, dirty_mb: footprint, rss_mb: rss };
	}
	const rollup = readFileSync(`/proc/${pid}/smaps_rollup`, "utf8");
	const kb = (k: string) => Number(rollup.match(new RegExp("^" + k + ":\\s+(\\d+)", "m"))?.[1] ?? NaN);
	return { private_mb: (kb("Private_Clean") + kb("Private_Dirty")) / 1024, dirty_mb: kb("Private_Dirty") / 1024, rss_mb: kb("Rss") / 1024 };
}

async function runOnce(cmd: string[]) {
	const t0 = performance.now();
	const p = Bun.spawn(cmd, { env, stdout: "pipe", stderr: "inherit" });
	const reader = p.stdout.getReader();
	const decoder = new TextDecoder();
	let text = "", boot = NaN, result: Record<string, number | string> = {};
	outer: for (;;) {
		const { value, done } = await reader.read();
		if (done) break;
		text += decoder.decode(value, { stream: true });
		let nl: number;
		while ((nl = text.indexOf("\n")) >= 0) {
			const line = text.slice(0, nl);
			text = text.slice(nl + 1);
			if (line === "BOOT" && Number.isNaN(boot)) boot = performance.now() - t0;
			if (line.startsWith("READY ")) { result = JSON.parse(line.slice(6)); break outer; }
		}
	}
	await Bun.sleep(2500);
	const mem = memory(p.pid);
	p.kill(9);
	await p.exited;
	return { boot, ...mem, result };
}
const median = (xs: number[]) => {
	const v = xs.filter((x) => Number.isFinite(x)).sort((a, b) => a - b);
	return v.length ? Math.round(v[Math.floor((v.length - 1) / 2)] * 100) / 100 : NaN;
};

const sizeMb = (p: string) => Math.round((statSync(realpathSync(p)).size / 1048576) * 100) / 100;
const rows: Record<string, Record<string, number | string>> = {};
for (const engine of ["bun", "jsc"]) {
	await runOnce(mains[`${engine}-idle`]);
	const idle = [];
	for (let i = 0; i < 5; i++) idle.push(await runOnce(mains[`${engine}-idle`]));
	const work = [];
	for (let i = 0; i < 3; i++) work.push(await runOnce(mains[`${engine}-work`]));
	const row: Record<string, number | string> = {
		shipped_mb: sizeMb(mains[`${engine}-idle`][0]),
		launch_ms: median(idle.map((r) => r.boot)),
		idle_private_mb: median(idle.map((r) => r.private_mb)),
		idle_dirty_mb: median(idle.map((r) => r.dirty_mb)),
		idle_rss_mb: median(idle.map((r) => r.rss_mb)),
		after_work_dirty_mb: median(work.map((r) => r.dirty_mb)),
	};
	for (const k of ["json_ms", "loop_ms", "string_ms", "objects_ms", "native_call_us"]) row[k] = median(work.map((r) => Number(r.result[k])));
	row.check = [...new Set(work.map((r) => String(r.result.check)))].join(" | ");
	rows[engine] = row;
	console.error(engine, JSON.stringify(row));
}

const os = process.platform === "darwin"
	? `macOS ${Bun.spawnSync(["sw_vers", "-productVersion"]).stdout.toString().trim()} ${process.arch}`
	: (readFileSync("/etc/os-release", "utf8").match(/^PRETTY_NAME="?([^"\n]+)/m)?.[1] ?? "Linux") + " " + process.arch;
const labels: [string, string][] = [
	["shipped_mb", "Runtime shipped in the app (MB)"],
	["launch_ms", "Launch to first JS line (ms, median of 5)"],
	...(process.platform === "darwin"
		? ([["idle_dirty_mb", "Idle memory, phys footprint (MB)"]] as [string, string][])
		: ([["idle_dirty_mb", "Idle memory, private dirty (MB)"], ["idle_private_mb", "Idle private incl. library pages (MB)"]] as [string, string][])),
	["idle_rss_mb", "Idle RSS (MB)"],
	["after_work_dirty_mb", "Memory after the workload, no GC yet (MB)"],
	["json_ms", "JSON: 1.4 MB parse x10 (ms)"],
	["loop_ms", "Loop: 5M iterations (ms)"],
	["string_ms", "Strings and regex (ms)"],
	["objects_ms", "Map with 200k objects (ms)"],
	["native_call_us", "Native call, synchronous (µs)"],
];
const md = [
	`### Engine numbers on ${os}: Bun vs the system JavaScriptCore`,
	"",
	"| | Bun (today) | JSC host |",
	"|---|---|---|",
	...labels.map(([k, label]) => `| ${label} | ${rows.bun[k]} | ${rows.jsc[k]} |`),
	"",
	`Bun ${Bun.version}. The JSC host is \`jsc-run\`; the engine is the OS's (${process.platform === "darwin" ? "JavaScriptCore.framework" : "libjavascriptcoregtk, which WebKitGTK needs anyway"}), so it isn't shipped.`,
	`Same results on both: ${rows.bun.check === rows.jsc.check} (${rows.jsc.check})`,
].join("\n");
writeFileSync(join(out, "results.md"), md + "\n");
writeFileSync(join(out, "results.json"), JSON.stringify({ os, rows }, null, 1));
console.log(md);
