// Node-shaped errors from the natives' errno errors.
const codes = {
	1: "EPERM", 2: "ENOENT", 4: "EINTR", 5: "EIO", 9: "EBADF", 13: "EACCES", 16: "EBUSY", 17: "EEXIST", 18: "EXDEV",
	20: "ENOTDIR", 21: "EISDIR", 22: "EINVAL", 24: "EMFILE", 28: "ENOSPC", 30: "EROFS", 32: "EPIPE", 35: "EAGAIN",
	62: "ELOOP", 63: "ENAMETOOLONG", 66: "ENOTEMPTY",
};
export function errnoCode(errno) {
	return codes[Math.abs(errno)] ?? "E" + Math.abs(errno);
}
export function nodeError(error) {
	if (!error || typeof error.errno !== "number") return error;
	const code = errnoCode(error.errno);
	const e = new Error(`${code}: ${error.message.toLowerCase()}, ${error.syscall}${error.path ? ` '${error.path}'` : ""}`);
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
