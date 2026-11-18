// Child processes: node:child_process (spawn, exec, execFile and the sync
// forms), Bun.spawn / Bun.spawnSync / Bun.which, and process.kill, in the
// shapes Channels' daemon and CLI use.
import { spawn, spawnSync, exec, execSync, execFile, execFileSync } from "node:child_process";
import { writeFileSync, chmodSync, mkdirSync, realpathSync } from "node:fs";
import { join } from "node:path";

const scratch = process.env.LIVEWIRE_SCRATCH;
const log = (...a) => console.log(...a);
const short = (s) => JSON.stringify(String(s));
const errShape = (e) => e && [e.code, e.errno !== undefined ? typeof e.errno : "-", e.syscall, e.path, e.killed, e.signal, e.status].map((v) => (v === undefined ? "-" : String(v))).join(" ");

// --- spawnSync --------------------------------------------------------------
{
	const r = spawnSync("sh", ["-c", "echo out; echo err >&2; exit 3"], { encoding: "utf8" });
	log("spawnSync", r.status, r.signal, short(r.stdout), short(r.stderr), r.output.length, r.output[0], typeof r.pid, r.error);
	const b = spawnSync("sh", ["-c", "printf abc"]);
	log("spawnSync buffers", Buffer.isBuffer(b.stdout), b.stdout.toString(), Buffer.isBuffer(b.stderr), b.stderr.length);
	const i = spawnSync("cat", { input: "piped in", encoding: "utf8" });
	log("spawnSync input", short(i.stdout), i.status);
	const t = spawnSync("sleep", ["5"], { timeout: 100 });
	log("spawnSync timeout", t.status, t.signal, t.error && t.error.code, t.error && t.error.syscall);
	const k = spawnSync("sleep", ["5"], { timeout: 100, killSignal: "SIGKILL" });
	log("spawnSync killSignal", k.status, k.signal);
	const n = spawnSync("no-such-program-livewire", ["a"]);
	log("spawnSync ENOENT", n.status, n.signal, n.pid, errShape(n.error), n.error && n.error.message, JSON.stringify(n.error && n.error.spawnargs));
	const c = spawnSync("pwd", { cwd: scratch, encoding: "utf8" });
	log("spawnSync cwd", c.stdout.trim() === realpathSync(scratch));
	const e = spawnSync("sh", ["-c", "echo $LW_A-$LW_B"], { env: { LW_A: "1", LW_B: "two", PATH: process.env.PATH }, encoding: "utf8" });
	log("spawnSync env", short(e.stdout));
	const big = spawnSync("sh", ["-c", "head -c 1000000 /dev/zero"]);
	log("spawnSync 1 MB", big.stdout.length);
	const max = spawnSync("sh", ["-c", "head -c 5000 /dev/zero"], { maxBuffer: 100 });
	log("spawnSync maxBuffer", max.error && max.error.code, max.stdout && max.stdout.length <= 5000);
	const sig = spawnSync("sh", ["-c", "kill -TERM $$"]);
	log("spawnSync signaled", sig.status, sig.signal);
	spawnSync("sh", ["-c", "echo inherited stdout"], { stdio: "inherit" });
	const ign = spawnSync("sh", ["-c", "echo gone; echo gone >&2"], { stdio: "ignore" });
	log("spawnSync ignore", ign.stdout, ign.stderr, ign.status);
	const bin = join(scratch, "bin");
	mkdirSync(bin, { recursive: true });
	writeFileSync(join(bin, "lw-only-here"), "#!/bin/sh\necho found via options.env.PATH\n");
	chmodSync(join(bin, "lw-only-here"), 0o755);
	const p = spawnSync("lw-only-here", { env: { PATH: bin + ":/usr/bin:/bin" }, encoding: "utf8" });
	log("spawnSync PATH", short(p.stdout), p.error && p.error.code);
}

// --- execFileSync / execSync -------------------------------------------------
{
	log("execFileSync", short(execFileSync("printf", ["%s-%s", "a", "b"], { encoding: "utf8" })));
	log("execFileSync buffer", Buffer.isBuffer(execFileSync("printf", ["x"])));
	try {
		execFileSync("sh", ["-c", "echo partial; exit 4"], { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] });
	} catch (e) {
		log("execFileSync throws", e.status, e.signal, short(e.stdout), e.message.split("\n")[0]);
	}
	try {
		execFileSync("no-such-program-livewire", [], { stdio: ["ignore", "pipe", "ignore"] });
	} catch (e) {
		log("execFileSync ENOENT", errShape(e));
	}
	try {
		execFileSync("sleep", ["5"], { timeout: 100 });
	} catch (e) {
		log("execFileSync timeout", e.code, e.signal, e.status);
	}
	log("execSync", short(execSync("echo $LW_X | tr a-z A-Z", { env: { LW_X: "shell", PATH: process.env.PATH }, encoding: "utf8" })));
	log("execSync input", short(execSync("cat", { input: Buffer.from("bytes in") }).toString()));
}

// --- process.kill --------------------------------------------------------------
{
	log("process.kill self 0", process.kill(process.pid, 0));
	try {
		process.kill(2147483646, "SIGTERM");
	} catch (e) {
		log("process.kill ESRCH", e.code, e.syscall, e.errno !== undefined);
	}
	try {
		process.kill(process.pid, "SIGNOPE");
	} catch (e) {
		log("process.kill bad signal", e.code);
	}
}

// --- async: node:child_process, then Bun.spawn ----------------------------------
const steps = [];
const step = (name, fn) => steps.push([name, fn]);
const events = (child) => {
	const seen = [];
	child.on("spawn", () => seen.push("spawn"));
	child.on("exit", (code, signal) => seen.push(`exit ${code} ${signal}`));
	child.on("error", (e) => seen.push("error " + e.code));
	return new Promise((resolve) => child.on("close", (code, signal) => { seen.push(`close ${code} ${signal}`); resolve(seen); }));
};

step("spawn pipes", async () => {
	const child = spawn("sh", ["-c", "cat; echo err >&2; exit 2"]);
	let out = "", err = "";
	child.stdout.setEncoding("utf8");
	child.stdout.on("data", (d) => (out += d));
	child.stderr.on("data", (d) => (err += d.toString()));
	const done = events(child);
	log("spawn", typeof child.pid, child.exitCode, child.signalCode, child.killed, child.stdio.length, child.stdin === child.stdio[0]);
	child.stdin.write("line 1\n");
	child.stdin.end("line 2\n");
	log("spawn events", (await done).join(", "));
	log("spawn output", short(out), short(err), child.exitCode, child.signalCode);
});
step("spawn big stdin and stdout", async () => {
	const child = spawn("cat");
	let n = 0;
	child.stdout.on("data", (d) => (n += d.length));
	const done = events(child);
	child.stdin.end(Buffer.alloc(3 * 1024 * 1024, 97));
	await done;
	log("spawn 3 MB round trip", n);
});
step("spawn ENOENT", async () => {
	const child = spawn("no-such-program-livewire", ["x"]);
	let error;
	child.on("error", (e) => (error = e));
	log("spawn ENOENT pid", child.pid);
	const seen = await events(child);
	log("spawn ENOENT events", seen.join(", "), errShape(error), error.message, JSON.stringify(error.spawnargs));
});
step("kill", async () => {
	const child = spawn("sleep", ["5"]);
	const done = events(child);
	log("kill returns", child.kill(), child.killed);
	log("kill events", (await done).join(", "), child.exitCode, child.signalCode);
	const k9 = spawn("sleep", ["5"]);
	const d9 = events(k9);
	k9.kill("SIGKILL");
	log("kill SIGKILL", (await d9).join(", "));
});
step("stdio ignore and inherit", async () => {
	const a = spawn("sh", ["-c", "echo to-ignore"], { stdio: "ignore" });
	log("ignore stdio", a.stdin, a.stdout, a.stderr);
	await events(a);
	const b = spawn("sh", ["-c", "echo inherited async"], { stdio: ["ignore", "inherit", "inherit"] });
	await events(b);
	const c = spawn("sh", ["-c", "cat"], { stdio: ["pipe", "pipe", "ignore"] });
	let out = "";
	c.stdout.on("data", (d) => (out += d));
	c.stdin.end("mixed");
	await events(c);
	log("mixed stdio", short(out), c.stderr);
});
step("shell", async () => {
	const child = spawn("echo $((6 * 7)) | tr 4 X", { shell: true });
	let out = "";
	child.stdout.on("data", (d) => (out += d));
	await events(child);
	log("spawn shell", short(out));
});
step("timeout option", async () => {
	const child = spawn("sleep", ["5"], { timeout: 100 });
	log("spawn timeout", (await events(child)).join(", "), child.killed);
});
step("execFile", async () => {
	await new Promise((resolve) => execFile("sh", ["-c", "echo o; echo e >&2"], (err, stdout, stderr) => { log("execFile ok", err, short(stdout), short(stderr)); resolve(); }));
	await new Promise((resolve) => execFile("sh", ["-c", "echo o; exit 5"], (err, stdout) => { log("execFile exit", err.code, err.killed, err.signal, short(stdout), err.message.split("\n")[0]); resolve(); }));
	await new Promise((resolve) => execFile("sleep", ["5"], { timeout: 100 }, (err) => { log("execFile timeout", err.code, err.killed, err.signal); resolve(); }));
	await new Promise((resolve) => execFile("no-such-program-livewire", ["y"], (err, stdout, stderr) => { log("execFile ENOENT", errShape(err), short(stdout), short(stderr)); resolve(); }));
	await new Promise((resolve) => execFile("sh", ["-c", "printf 0123456789"], { maxBuffer: 4 }, (err, stdout) => { log("execFile maxBuffer", err.code, err.message, short(stdout)); resolve(); }));
	await new Promise((resolve) => execFile("printf", ["\\001\\002"], { encoding: "buffer" }, (err, stdout) => { log("execFile buffer", err, Buffer.isBuffer(stdout), [...stdout].join(",")); resolve(); }));
	const child = execFile("cat", (err, stdout) => { log("execFile stdin", err, short(stdout)); });
	child.stdin.end("fed");
	await new Promise((resolve) => child.on("close", resolve));
});
step("exec", async () => {
	await new Promise((resolve) => exec("echo $LW_Y; exit 0", { env: { LW_Y: "from env", PATH: process.env.PATH } }, (err, stdout) => { log("exec", err, short(stdout)); resolve(); }));
	await new Promise((resolve) => exec("exit 7", (err) => { log("exec exit", err.code, err.cmd); resolve(); }));
});
step("detached unref", async () => {
	const child = spawn("sleep", ["5"], { detached: true, stdio: "ignore" });
	child.unref();
	log("detached pid", typeof child.pid);
	// The unref'd child no longer keeps the process alive: a timer does, until its exit.
	const alive = setTimeout(() => log("no exit event"), 5000);
	const exit = new Promise((resolve) => child.on("exit", (code, signal) => resolve(`${code} ${signal}`)));
	process.kill(-child.pid, "SIGKILL");
	log("detached exit", await exit);
	clearTimeout(alive);
});
step("Bun.spawn", async () => {
	const p = Bun.spawn(["sh", "-c", "cat; echo err >&2; exit 6"], { stdin: "pipe", stdout: "pipe", stderr: "pipe" });
	log("Bun.spawn", typeof p.pid, p.exitCode, p.signalCode, p.killed);
	p.stdin.write("bun ");
	p.stdin.write(new TextEncoder().encode("stdin"));
	p.stdin.end();
	const [out, err, code] = await Promise.all([new Response(p.stdout).text(), new Response(p.stderr).text(), p.exited]);
	log("Bun.spawn result", short(out), short(err), code, p.exitCode, p.signalCode);
	const d = Bun.spawn(["sh", "-c", "echo defaults"]);
	log("Bun.spawn defaults", short(await new Response(d.stdout).text()), d.stdin, d.stderr, await d.exited);
	const k = Bun.spawn(["sleep", "5"], { stdout: "ignore" });
	k.kill();
	log("Bun.spawn kill", await k.exited, k.exitCode, k.signalCode, k.killed);
	const k9 = Bun.spawn({ cmd: ["sleep", "5"], stdout: "ignore" });
	k9.kill("SIGKILL");
	log("Bun.spawn SIGKILL", await k9.exited, k9.signalCode);
	let o;
	const onExit = new Promise((resolve) => {
		o = Bun.spawn(["sh", "-c", "exit 9"], { stdout: "ignore", onExit: (proc, exitCode, signalCode, error) => resolve([proc === o, exitCode, signalCode, error]) });
	});
	log("Bun.spawn onExit", JSON.stringify(await onExit), await o.exited);
	const t = Bun.spawn(["sleep", "5"], { stdout: "ignore", timeout: 100, killSignal: "SIGKILL" });
	log("Bun.spawn timeout", await t.exited, t.signalCode);
	const c = Bun.spawn(["pwd"], { cwd: scratch, env: { PATH: process.env.PATH } });
	log("Bun.spawn cwd", (await new Response(c.stdout).text()).trim() === realpathSync(scratch));
	let chunks = 0, bytes = 0;
	const r = Bun.spawn(["sh", "-c", "head -c 300000 /dev/zero"]);
	for await (const chunk of r.stdout) { chunks++; bytes += chunk.length; }
	log("Bun.spawn stream", chunks > 0, bytes, chunks > 0 && true);
	try {
		Bun.spawn(["no-such-program-livewire"]);
		log("Bun.spawn ENOENT did not throw");
	} catch (e) {
		log("Bun.spawn ENOENT", e.code, e.errno !== undefined, e.syscall, e.path);
	}
});
step("Bun.spawnSync", async () => {
	const r = Bun.spawnSync(["sh", "-c", "echo out; echo err >&2; exit 1"]);
	log("Bun.spawnSync", Buffer.isBuffer(r.stdout), short(r.stdout), short(r.stderr), r.exitCode, r.success, r.signalCode, typeof r.pid);
	const i = Bun.spawnSync(["cat"], { stdin: Buffer.from("in"), stdout: "pipe" });
	log("Bun.spawnSync stdin", short(i.stdout));
	const t = Bun.spawnSync(["sleep", "5"], { timeout: 100 });
	log("Bun.spawnSync timeout", t.exitCode, t.signalCode, t.success, t.exitedDueToTimeout);
	const ig = Bun.spawnSync(["sh", "-c", "echo x"], { stdout: "ignore", stderr: "ignore" });
	log("Bun.spawnSync ignore", ig.stdout, ig.stderr, ig.exitCode);
	const o = Bun.spawnSync({ cmd: ["sh", "-c", "printf obj"], env: { PATH: process.env.PATH } });
	log("Bun.spawnSync object", short(o.stdout));
	try {
		Bun.spawnSync(["no-such-program-livewire"]);
		log("Bun.spawnSync ENOENT did not throw");
	} catch (e) {
		log("Bun.spawnSync ENOENT", e.code, e.syscall, e.path);
	}
});
// A file of that name on PATH that isn't executable: Bun reports ENOENT (Node
// gives EACCES), and Bun.which skips it.
step("not executable on PATH", async () => {
	const dir = join(scratch, "noexec");
	mkdirSync(dir, { recursive: true });
	writeFileSync(join(dir, "lw-noexec"), "#!/bin/sh\necho ran\n");
	chmodSync(join(dir, "lw-noexec"), 0o644);
	const r = spawnSync("lw-noexec", [], { env: { PATH: dir } });
	log("spawnSync not executable", r.status, errShape(r.error));
	const error = await new Promise((resolve) => spawn("lw-noexec", [], { env: { PATH: dir } }).on("error", resolve));
	log("spawn not executable", errShape(error));
	log("Bun.which not executable", Bun.which("lw-noexec", { PATH: dir }));
});
step("Bun.which", async () => {
	log("Bun.which", Bun.which("sh") === realpathSync(Bun.which("sh")) || Bun.which("sh").endsWith("/sh"), Bun.which("no-such-program-livewire"), Bun.which("lw-only-here", { PATH: join(scratch, "bin") }) === join(scratch, "bin", "lw-only-here"));
});

(async () => {
	for (const [, fn] of steps) await fn();
	log("done");
})();
