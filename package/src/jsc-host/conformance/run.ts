// The JSC host conformance suite: each conformance/*.mjs runs on a reference
// runtime (Bun, or Node for "// oracle: node") and, bundled, on this OS's JSC
// host. The host passes when its output and exit code match the reference.
//   bun package/src/jsc-host/conformance/run.ts [out dir] [name filter]
import { mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";

const here = import.meta.dir;
const host = join(here, "..");
const out = process.argv[2] ?? mkdtempSync(join(tmpdir(), "jsc-conformance-"));
const only = process.argv[3];
const build = Bun.spawnSync(["sh", join(host, "build.sh"), out], { stdout: "inherit", stderr: "inherit" });
if (build.exitCode !== 0) process.exit(1);
mkdirSync(join(out, "conformance"), { recursive: true });

const libExt = process.platform === "darwin" ? "dylib" : "so";
const scratch = mkdtempSync(join(tmpdir(), "jsc-conf-"));
const fifo = join(scratch, "fifo");
if (Bun.spawnSync(["mkfifo", fifo]).exitCode !== 0) throw new Error("mkfifo failed");
const env = { ...process.env, LIVEWIRE_TESTLIB: join(out, "libjsctest." + libExt), LIVEWIRE_FIFO: fifo, LIVEWIRE_SCRATCH: scratch };

type Run = { code: number | null; text: string };
const run = (cmd: string[]): Run => {
	const r = Bun.spawnSync(cmd, { env, stdout: "pipe", stderr: "pipe", timeout: 15_000 });
	return { code: r.exitCode, text: (r.stdout.toString() + r.stderr.toString()).trimEnd() };
};

const tests = readdirSync(here).filter((f) => f.endsWith(".mjs") && (!only || f.includes(only))).sort();
let failed = 0;
for (const test of tests) {
	const path = join(here, test);
	const oracle = /^\/\/ oracle: node$/m.test(readFileSync(path, "utf8")) ? "node" : "bun";
	const want = run([oracle === "node" ? "node" : process.execPath, path]);
	const bundled = join(out, "conformance", basename(test, ".mjs") + ".js");
	const bundle = Bun.spawnSync([process.execPath, join(host, "bundle.ts"), path, bundled], { stderr: "pipe" });
	const got: Run = bundle.exitCode === 0 ? run([join(out, "jsc-run"), bundled]) : { code: null, text: "bundle failed: " + bundle.stderr.toString() };
	if (want.code === got.code && want.text === got.text) {
		console.log(`ok   ${test} (${want.text.split("\n").length} lines, vs ${oracle})`);
		continue;
	}
	failed++;
	console.log(`FAIL ${test} vs ${oracle}: exit ${got.code}, ${oracle} exit ${want.code}`);
	const a = want.text.split("\n"), b = got.text.split("\n");
	for (let i = 0; i < Math.max(a.length, b.length); i++) {
		if (a[i] === b[i]) continue;
		console.log(`  line ${i + 1}\n    ${oracle}: ${a[i] ?? "(none)"}\n    jsc:  ${b[i] ?? "(none)"}`);
	}
}
rmSync(scratch, { recursive: true, force: true });
console.log(failed ? `${failed} of ${tests.length} differ from the reference` : `all ${tests.length} match the reference`);
process.exit(failed ? 1 : 0);
