// Node-shaped errors from the natives' errno errors. Codes come from this
// OS's errno values (they differ between macOS and Linux); messages are
// libuv's, as Node and Bun print them.
const codes = Object.fromEntries(Object.entries(globalThis.__node.errnos).map(([name, value]) => [value, name]));
const messages = {
	EPERM: "operation not permitted", ENOENT: "no such file or directory", ESRCH: "no such process", EINTR: "interrupted system call",
	EIO: "i/o error", ENXIO: "no such device or address", E2BIG: "argument list too long", ENOEXEC: "exec format error",
	EBADF: "bad file descriptor", ECHILD: "no child processes", EAGAIN: "resource temporarily unavailable", ENOMEM: "not enough memory",
	EACCES: "permission denied", EFAULT: "bad address in system call argument", EBUSY: "resource busy or locked", EEXIST: "file already exists",
	EXDEV: "cross-device link not permitted", ENODEV: "no such device", ENOTDIR: "not a directory", EISDIR: "illegal operation on a directory",
	EINVAL: "invalid argument", ENFILE: "file table overflow", EMFILE: "too many open files", ENOTTY: "inappropriate ioctl for device",
	EFBIG: "file too large", ENOSPC: "no space left on device", ESPIPE: "invalid seek", EROFS: "read-only file system",
	EMLINK: "too many links", EPIPE: "broken pipe", ELOOP: "too many symbolic links encountered", ENAMETOOLONG: "name too long",
	ENOTEMPTY: "directory not empty", ENOSYS: "function not implemented", ETIMEDOUT: "connection timed out",
	ECONNREFUSED: "connection refused", ECONNRESET: "connection reset by peer", EADDRINUSE: "address already in use",
	ENOTSUP: "operation not supported on socket", ENOTCONN: "socket is not connected", ECANCELED: "operation canceled",
	ENOBUFS: "no buffer space available",
};
export function errnoCode(errno) {
	return codes[Math.abs(errno)] ?? "E" + Math.abs(errno);
}
export function nodeError(error) {
	if (!error || typeof error.errno !== "number" || error.code) return error;
	const code = errnoCode(error.errno);
	const text = messages[code] ?? error.message.toLowerCase();
	const e = new Error(`${code}: ${text}, ${error.syscall}${error.path ? ` '${error.path}'` : ""}`);
	e.code = code;
	e.errno = error.errno;
	e.syscall = error.syscall;
	if (error.path) e.path = error.path;
	return e;
}
export function wrap(fn) {
	return function (...args) {
		try {
			return fn.apply(this, args);
		} catch (error) {
			throw nodeError(error);
		}
	};
}
