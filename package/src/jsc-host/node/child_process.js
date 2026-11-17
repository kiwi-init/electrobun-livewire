// node:child_process on the Livewire JSC runtime: spawn, exec, execFile and
// their sync forms over posix_spawn (node.c). Behaviour follows Bun's, which
// apps run on today; conformance/spawn.mjs checks it. fork() and IPC aren't
// available.
import { EventEmitter } from "events";
import { Buffer } from "buffer";
import { FdReadable, FdWritable } from "./child-streams.js";
import { childEnv, errnoError, errnos, signalName, signalNumber, spawnError } from "./spawn-common.js";

const n = globalThis.__node;
const nextTick = (fn, ...args) => queueMicrotask(() => fn(...args));

function stdioKinds(stdio) {
	const list = typeof stdio === "string" ? [stdio, stdio, stdio] : Array.isArray(stdio) ? stdio.slice(0, 3) : [];
	while (list.length < 3) list.push(undefined);
	return list.map((s, i) => {
		if (s === undefined || s === null || s === "pipe" || s === "overlapped") return "pipe";
		if (s === "ignore" || s === "inherit") return s;
		if (typeof s === "number") return s;
		if (s && typeof s.fd === "number") return s.fd;
		if (s === "ipc") throw new Error("child_process: IPC isn't available on the JSC runtime");
		throw new TypeError(`The argument 'stdio[${i}]' is invalid. Received ${String(s)}`);
	});
}

// Node's normalizeSpawnArguments: the shell, argv0, env, stdio.
function normalize(file, args, options) {
	if (!Array.isArray(args)) {
		if (args !== undefined && args !== null && typeof args === "object") options = args;
		args = [];
	}
	options = { ...(options || {}) };
	file = String(file);
	args = args.map(String);
	if (options.shell) {
		const command = [file, ...args].join(" ");
		file = typeof options.shell === "string" ? options.shell : "/bin/sh";
		args = ["-c", command];
	}
	const env = childEnv(options.env || process.env);
	const cwd = options.cwd === undefined || options.cwd === null ? null : String(options.cwd instanceof URL ? options.cwd.pathname : options.cwd);
	return {
		file,
		args,
		argv: [options.argv0 === undefined ? file : String(options.argv0), ...args],
		env: env.pairs,
		searchPath: env.searchPath,
		cwd,
		stdio: stdioKinds(options.stdio),
		detached: !!options.detached,
		options,
	};
}

export class ChildProcess extends EventEmitter {
	constructor() {
		super();
		this.pid = undefined;
		this.exitCode = null;
		this.signalCode = null;
		this.killed = false;
		this.connected = false;
		this.stdin = null;
		this.stdout = null;
		this.stderr = null;
		this.stdio = [null, null, null];
		this.spawnfile = undefined;
		this.spawnargs = [];
		this._exited = false;
		this._closesNeeded = 1;
		this._closesGot = 0;
	}
	_spawn(s) {
		this.spawnfile = s.file;
		this.spawnargs = [s.file, ...s.args];
		const r = n.spawn(s.file, s.argv, s.env, s.cwd, s.stdio, s.detached, s.searchPath, (code, signo) => this._onExit(code, signo));
		if (typeof r === "number") {
			const error = spawnError(r, "spawn " + s.file, s.file, s.args);
			this._exited = true;
			nextTick(() => {
				this.emit("error", error);
				this.emit("close", r, undefined);
			});
			return;
		}
		const [pid, stdinFd, stdoutFd, stderrFd] = r;
		this.pid = pid;
		if (stdinFd >= 0) this.stdin = new FdWritable(stdinFd);
		for (const [key, fd] of [["stdout", stdoutFd], ["stderr", stderrFd]]) {
			if (fd < 0) continue;
			const stream = (this[key] = new FdReadable(fd));
			this._closesNeeded++;
			stream.on("close", () => this._maybeClose());
		}
		this.stdio = [this.stdin, this.stdout, this.stderr];
		const { timeout, killSignal, signal } = s.options;
		if (timeout > 0) {
			const timer = setTimeout(() => this.kill(killSignal), timeout);
			this.once("exit", () => clearTimeout(timer));
		}
		if (signal) {
			const abort = () => {
				this.kill(killSignal);
				const error = new Error("The operation was aborted");
				error.name = "AbortError";
				error.code = "ABORT_ERR";
				error.cause = signal.reason;
				this.emit("error", error);
			};
			if (signal.aborted) nextTick(abort);
			else if (typeof signal.addEventListener === "function") {
				signal.addEventListener("abort", abort, { once: true });
				this.once("exit", () => signal.removeEventListener("abort", abort));
			}
		}
		nextTick(() => this.emit("spawn"));
	}
	_onExit(code, signo) {
		this._exited = true;
		this.exitCode = code;
		this.signalCode = signo ? signalName(signo) : null;
		if (this.stdin) this.stdin.destroy();
		this.emit("exit", this.exitCode, this.signalCode);
		// Output nobody reads is drained, so 'close' still comes (Node's flushStdio).
		nextTick(() => {
			for (const stream of [this.stdout, this.stderr]) {
				if (stream && !stream.destroyed && stream.readableFlowing === null && stream.listenerCount("data") === 0) stream.resume();
			}
		});
		this._maybeClose();
	}
	_maybeClose() {
		if (++this._closesGot === this._closesNeeded) this.emit("close", this.exitCode, this.signalCode);
	}
	kill(signal) {
		const signo = signalNumber(signal);
		if (this.pid === undefined || this._exited) return false;
		const r = n.kill(this.pid, signo);
		if (r === 0) {
			this.killed = true;
			return true;
		}
		if (r !== -errnos.ESRCH) this.emit("error", errnoError(r, "kill"));
		return false;
	}
	ref() {
		if (this.pid !== undefined) n.childRef(this.pid, true);
	}
	unref() {
		if (this.pid !== undefined) n.childRef(this.pid, false);
	}
	disconnect() {}
	send() {
		throw new Error("child_process: IPC isn't available on the JSC runtime");
	}
}

export function spawn(file, args, options) {
	const child = new ChildProcess();
	child._spawn(normalize(file, args, options));
	return child;
}

const bufferOrString = (bytes, encoding) => (encoding && encoding !== "buffer" ? bytes.toString(encoding) : bytes);

export function spawnSync(file, args, options) {
	const s = normalize(file, args, options);
	const o = s.options;
	let input = o.input ?? null;
	if (typeof input === "string") input = Buffer.from(input, o.encoding && o.encoding !== "buffer" ? o.encoding : "utf8");
	else if (input && ArrayBuffer.isView(input)) input = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
	const r = n.spawnSync(s.file, s.argv, s.env, s.cwd, s.stdio, s.detached, s.searchPath, input,
		Number(o.timeout) || 0, signalNumber(o.killSignal), o.maxBuffer === undefined ? 1024 * 1024 : Number(o.maxBuffer));
	if (r.error) {
		return { pid: 0, output: null, stdout: null, stderr: null, status: null, signal: null, error: spawnError(r.error, "spawnSync " + s.file, s.file, s.args) };
	}
	const stdout = r.stdout ? bufferOrString(Buffer.from(r.stdout.buffer, r.stdout.byteOffset, r.stdout.byteLength), o.encoding) : null;
	const stderr = r.stderr ? bufferOrString(Buffer.from(r.stderr.buffer, r.stderr.byteOffset, r.stderr.byteLength), o.encoding) : null;
	const result = { pid: r.pid, output: [null, stdout, stderr], stdout, stderr, status: r.status, signal: r.signal ? signalName(r.signal) : null, error: undefined };
	if (r.timedOut || r.overflow) {
		const code = r.timedOut ? "ETIMEDOUT" : "ENOBUFS";
		const error = errnoError(-errnos[code], "spawnSync " + s.file);
		error.path = s.file;
		error.spawnargs = s.args;
		result.error = error;
	}
	return result;
}

// Node's checkExecSyncError.
function execSyncError(ret, cmd) {
	if (ret.error) return Object.assign(ret.error, ret, { error: undefined });
	if (ret.status === 0) return null;
	let message = "Command failed: " + cmd;
	if (ret.stderr && ret.stderr.length > 0) message += "\n" + ret.stderr.toString();
	return Object.assign(new Error(message), ret);
}
function execSyncCommon(file, args, options, cmd) {
	const inheritStderr = !options || !options.stdio;
	const ret = spawnSync(file, args, options);
	if (inheritStderr && ret.stderr) process.stderr.write(ret.stderr.toString());
	const error = execSyncError(ret, cmd);
	if (error) {
		delete error.error;
		throw error;
	}
	return ret.stdout;
}
export function execFileSync(file, args, options) {
	if (!Array.isArray(args)) [options, args] = [args, []];
	return execSyncCommon(file, args, options, [file, ...args].join(" "));
}
export function execSync(command, options) {
	return execSyncCommon(command, [], { ...(options || {}), shell: (options && options.shell) || true }, command);
}

export function execFile(file, args, options, callback) {
	const rest = [args, options, callback];
	callback = rest.find((a) => typeof a === "function");
	args = Array.isArray(rest[0]) ? rest[0] : [];
	options = rest.find((a) => a && typeof a === "object" && !Array.isArray(a)) || {};
	const encoding = options.encoding === undefined ? "utf8" : options.encoding;
	const maxBuffer = options.maxBuffer === undefined ? 1024 * 1024 : options.maxBuffer;
	const cmd = [file, ...args].join(" ");
	const child = spawn(file, args, { ...options, timeout: undefined });
	const out = { stdout: [], stderr: [] };
	const length = { stdout: 0, stderr: 0 };
	let error = null, exited = false, killed = false, timer = null;
	const kill = () => {
		killed = true;
		try {
			child.kill(options.killSignal);
		} catch (e) {
			error = e;
			done();
		}
	};
	if (options.timeout > 0) timer = setTimeout(kill, options.timeout);
	const text = (key) => {
		const bytes = Buffer.concat(out[key]);
		return encoding && encoding !== "buffer" ? bytes.toString(encoding) : bytes;
	};
	function done(code, signal) {
		if (exited) return;
		exited = true;
		if (timer) clearTimeout(timer);
		if (!callback) return;
		const stdout = text("stdout"), stderr = text("stderr");
		if (!error && code === 0 && signal === null) return callback(null, stdout, stderr);
		if (!error) {
			error = new Error("Command failed: " + cmd + "\n" + stderr);
			error.code = code;
			error.killed = child.killed || killed;
			error.signal = signal;
		}
		error.cmd = cmd;
		callback(error, stdout, stderr);
	}
	for (const key of ["stdout", "stderr"]) {
		child[key]?.on("data", (chunk) => {
			if (error) return;
			length[key] += chunk.length;
			if (maxBuffer !== Infinity && length[key] > maxBuffer) {
				out[key].push(chunk.subarray(0, chunk.length - (length[key] - maxBuffer)));
				error = new RangeError(key + " maxBuffer length exceeded");
				error.code = "ERR_CHILD_PROCESS_STDIO_MAXBUFFER";
				kill();
			} else out[key].push(chunk);
		});
	}
	child.on("close", done);
	child.on("error", (e) => {
		error = e;
		child.stdout?.destroy();
		child.stderr?.destroy();
		done();
	});
	return child;
}
export function exec(command, options, callback) {
	if (typeof options === "function") [callback, options] = [options, undefined];
	return execFile(command, [], { ...(options || {}), shell: (options && options.shell) || true }, callback);
}

export function fork() {
	throw new Error("child_process.fork isn't available on the JSC runtime");
}
export default { ChildProcess, spawn, spawnSync, exec, execSync, execFile, execFileSync, fork };
