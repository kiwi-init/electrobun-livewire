// Bun.spawn, Bun.spawnSync and Bun.which on the Livewire JSC runtime, over
// the same natives as node:child_process. Shapes follow Bun's (see
// conformance/spawn.mjs).
import { Buffer } from "buffer";
import { FdWritable } from "./child-streams.js";
import { childEnv, signalName, signalNumber, spawnError } from "./spawn-common.js";

const n = globalThis.__node;

function command(cmdOrOptions, options) {
	const o = Array.isArray(cmdOrOptions) ? { ...(options || {}), cmd: cmdOrOptions } : { ...(cmdOrOptions || {}), ...(options || {}) };
	if (!Array.isArray(o.cmd) || o.cmd.length === 0) throw new TypeError("cmd must be a non-empty array of strings");
	o.cmd = o.cmd.map(String);
	return o;
}
const isData = (v) => v instanceof ArrayBuffer || ArrayBuffer.isView(v);
const dataBytes = (v) => (typeof v === "string" ? Buffer.from(v) : v instanceof ArrayBuffer ? new Uint8Array(v) : new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
// Bun's stdio values: "pipe", "inherit", "ignore", null (ignore), an fd, or
// (stdin) bytes to write.
function kinds(o, defaults) {
	const given = Array.isArray(o.stdio) ? o.stdio : [o.stdin, o.stdout, o.stderr];
	return [0, 1, 2].map((i) => {
		const v = given[i] === undefined ? defaults[i] : given[i];
		if (v === null || v === "ignore") return "ignore";
		if (v === "pipe" || v === "inherit" || typeof v === "number") return v;
		if (i === 0 && isData(v)) return "pipe";
		return defaults[i];
	});
}

function readableFromFd(fd) {
	let watch = 0;
	return new globalThis.ReadableStream({
		start(controller) {
			watch = n.watchFd(fd, (value) => {
				if (value === null) {
					n.unwatchFd(watch, true);
					controller.close();
				} else if (typeof value === "number") {
					n.unwatchFd(watch, true);
					controller.error(new Error("read failed: " + value));
				} else controller.enqueue(value);
			});
		},
		cancel() {
			n.unwatchFd(watch, true);
		},
	});
}

// Bun's FileSink for a "pipe" stdin: write() returns the bytes taken.
class FileSink {
	constructor(fd) {
		this._out = new FdWritable(fd);
		this._out.on("error", () => {});
		this._written = 0;
	}
	write(chunk) {
		const bytes = dataBytes(chunk);
		this._out.write(bytes);
		this._written += bytes.length;
		return bytes.length;
	}
	flush() {
		return 0;
	}
	end() {
		this._out.end();
		return this._written;
	}
	ref() {}
	unref() {}
}

export function spawn(cmdOrOptions, maybeOptions) {
	const o = command(cmdOrOptions, maybeOptions);
	const env = childEnv(o.env || process.env);
	const stdio = kinds(o, ["ignore", "pipe", "inherit"]);
	let resolveExited;
	const proc = {
		pid: undefined,
		exitCode: null,
		signalCode: null,
		killed: false,
		stdin: undefined,
		stdout: undefined,
		stderr: undefined,
		exited: new Promise((resolve) => (resolveExited = resolve)),
		kill(signal) {
			if (proc.exitCode !== null || proc.signalCode !== null) return;
			if (n.kill(proc.pid, signalNumber(signal)) === 0) proc.killed = true;
		},
		ref() {
			n.childRef(proc.pid, true);
		},
		unref() {
			n.childRef(proc.pid, false);
		},
	};
	let timer = null;
	const r = n.spawn(o.cmd[0], o.cmd, env.pairs, o.cwd === undefined ? null : String(o.cwd), stdio, !!o.detached, env.searchPath, (code, signo) => {
		if (timer) clearTimeout(timer);
		proc.exitCode = code;
		proc.signalCode = signo ? signalName(signo) : null;
		if (typeof o.onExit === "function") o.onExit(proc, proc.exitCode, proc.signalCode, null);
		resolveExited(code === null ? 128 + signo : code);
	});
	if (typeof r === "number") throw spawnError(r, undefined, o.cmd[0]);
	const [pid, stdinFd, stdoutFd, stderrFd] = r;
	proc.pid = pid;
	if (stdinFd >= 0) {
		const sink = new FileSink(stdinFd);
		const given = Array.isArray(o.stdio) ? o.stdio[0] : o.stdin;
		if (isData(given)) {
			sink.write(given);
			sink.end();
		} else proc.stdin = sink;
	}
	if (stdoutFd >= 0) proc.stdout = readableFromFd(stdoutFd);
	if (stderrFd >= 0) proc.stderr = readableFromFd(stderrFd);
	if (o.timeout > 0) timer = setTimeout(() => proc.kill(o.killSignal), o.timeout);
	if (o.signal) {
		const abort = () => proc.kill(o.killSignal);
		if (o.signal.aborted) queueMicrotask(abort);
		else if (typeof o.signal.addEventListener === "function") o.signal.addEventListener("abort", abort, { once: true });
	}
	return proc;
}

export function spawnSync(cmdOrOptions, maybeOptions) {
	const o = command(cmdOrOptions, maybeOptions);
	const env = childEnv(o.env || process.env);
	const stdio = kinds(o, ["ignore", "pipe", "pipe"]);
	const given = Array.isArray(o.stdio) ? o.stdio[0] : o.stdin;
	const input = isData(given) ? dataBytes(given) : null;
	const r = n.spawnSync(o.cmd[0], o.cmd, env.pairs, o.cwd === undefined ? null : String(o.cwd), stdio, !!o.detached, env.searchPath, input,
		Number(o.timeout) || 0, signalNumber(o.killSignal), o.maxBuffer === undefined ? 0 : Number(o.maxBuffer));
	if (r.error) throw spawnError(r.error, undefined, o.cmd[0]);
	const buffer = (bytes) => (bytes ? Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength) : undefined);
	const result = { exitCode: r.status, stdout: buffer(r.stdout), stderr: buffer(r.stderr), success: r.status === 0, resourceUsage: undefined, pid: r.pid };
	if (r.signal) result.signalCode = signalName(r.signal);
	if (r.timedOut) result.exitedDueToTimeout = true;
	return result;
}

export function which(name, options = {}) {
	const path = options.PATH ?? process.env.PATH ?? null;
	return n.which(String(name), path === null ? null : String(path));
}
