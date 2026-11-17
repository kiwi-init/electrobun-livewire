// What node:child_process and Bun.spawn share on the Livewire JSC runtime:
// signals, the child's environment, and spawn errors in Bun's shape.
import { errnoCode } from "./errors.js";

const n = globalThis.__node;
export const errnos = n.errnos;
export const signals = n.signals;
const signalNames = Object.fromEntries(Object.entries(signals).map(([name, value]) => [value, name]));
export const signalName = (signo) => signalNames[signo] ?? null;

// A signal's number from its name or number (0 included), as process.kill takes it.
export function signalNumber(signal, fallback = "SIGTERM") {
	const s = signal === undefined || signal === null ? fallback : signal;
	if (typeof s === "number" && (s === 0 || signalNames[s])) return s;
	if (typeof s === "string" && Object.hasOwn(signals, s)) return signals[s];
	const error = new TypeError(`Unknown signal: ${s}`);
	error.code = "ERR_UNKNOWN_SIGNAL";
	throw error;
}

// "NAME=value" pairs for the child, and the PATH that finds its program.
export function childEnv(env) {
	const pairs = [];
	for (const [key, value] of Object.entries(env)) if (value !== undefined) pairs.push(key + "=" + value);
	return { pairs, searchPath: typeof env.PATH === "string" ? env.PATH : null };
}

// An errno error: Bun's message for a program not found, Node's otherwise.
export function spawnError(errno, syscall, file, spawnargs) {
	const code = errnoCode(errno);
	const error = new Error(code === "ENOENT" ? `Executable not found in $PATH: "${file}"` : `${syscall} ${code}`);
	error.errno = errno;
	error.code = code;
	if (syscall) error.syscall = syscall;
	error.path = file;
	if (spawnargs) error.spawnargs = spawnargs;
	return error;
}
export function errnoError(errno, syscall) {
	const code = errnoCode(errno);
	const error = new Error(`${syscall} ${code}`);
	error.errno = errno;
	error.code = code;
	error.syscall = syscall;
	return error;
}

// process.kill(pid, signal = "SIGTERM")
export function kill(pid, signal) {
	const r = n.kill(Number(pid), signalNumber(signal));
	if (r < 0) throw errnoError(r, "kill");
	return true;
}
