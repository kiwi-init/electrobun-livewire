// node:fs on the Livewire JSC runtime: the synchronous API over __node.fs,
// with promise and callback forms built on it.
import { Buffer } from "buffer";
import { wrap } from "./errors.js";

const n = globalThis.__node.fs;
export const constants = n.constants;
const C = constants;
const C_ENOENT = globalThis.__node.errnos.ENOENT;

const toPath = (p) => (typeof URL === "function" && p instanceof URL ? decodeURIComponent(p.pathname) : String(p));
const encoder = new TextEncoder();
const toBytes = (data, encoding) => {
	if (typeof data === "string") return encoding && encoding !== "utf8" && encoding !== "utf-8" ? Buffer.from(data, encoding) : encoder.encode(data);
	if (data instanceof ArrayBuffer) return new Uint8Array(data);
	if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
	return encoder.encode(String(data));
};
const encodingOf = (options) => (typeof options === "string" ? options : options?.encoding ?? null);

function flagsOf(flags = "r") {
	if (typeof flags === "number") return flags;
	const table = {
		r: C.O_RDONLY, "r+": C.O_RDWR, rs: C.O_RDONLY | C.O_SYNC, "rs+": C.O_RDWR | C.O_SYNC,
		w: C.O_WRONLY | C.O_CREAT | C.O_TRUNC, wx: C.O_WRONLY | C.O_CREAT | C.O_TRUNC | C.O_EXCL,
		"w+": C.O_RDWR | C.O_CREAT | C.O_TRUNC, "wx+": C.O_RDWR | C.O_CREAT | C.O_TRUNC | C.O_EXCL,
		a: C.O_WRONLY | C.O_CREAT | C.O_APPEND, ax: C.O_WRONLY | C.O_CREAT | C.O_APPEND | C.O_EXCL,
		"a+": C.O_RDWR | C.O_CREAT | C.O_APPEND, "ax+": C.O_RDWR | C.O_CREAT | C.O_APPEND | C.O_EXCL,
	};
	if (!(flags in table)) throw new TypeError("Unknown file open flag: " + flags);
	return table[flags] | C.O_CLOEXEC;
}

class Stats {
	constructor(raw) {
		Object.assign(this, raw);
		for (const k of ["atime", "mtime", "ctime", "birthtime"]) this[k] = new Date(raw[k + "Ms"]);
	}
	isFile() { return (this.mode & C.S_IFMT) === C.S_IFREG; }
	isDirectory() { return (this.mode & C.S_IFMT) === C.S_IFDIR; }
	isSymbolicLink() { return (this.mode & C.S_IFMT) === C.S_IFLNK; }
	isFIFO() { return (this.mode & C.S_IFMT) === C.S_IFIFO; }
	isSocket() { return (this.mode & C.S_IFMT) === C.S_IFSOCK; }
	isCharacterDevice() { return (this.mode & C.S_IFMT) === C.S_IFCHR; }
	isBlockDevice() { return (this.mode & C.S_IFMT) === C.S_IFBLK; }
}

const statWith = (link) => (path, options) => {
	try {
		return new Stats(n.stat(toPath(path), link));
	} catch (error) {
		if (options?.throwIfNoEntry === false && error.errno === -C_ENOENT) return undefined;
		throw error;
	}
};
export const statSync = wrap(statWith(false));
export const lstatSync = wrap(statWith(true));
export const fstatSync = wrap((fd) => new Stats(n.fstat(fd)));
export const existsSync = (path) => {
	try { n.access(toPath(path), C.F_OK); return true; } catch { return false; }
};
export const accessSync = wrap((path, mode = C.F_OK) => n.access(toPath(path), mode));
export const openSync = wrap((path, flags = "r", mode = 0o666) => n.open(toPath(path), flagsOf(flags), mode));
export const closeSync = wrap((fd) => n.close(fd));
export const fsyncSync = wrap((fd) => n.fsync(fd));
export const readSync = wrap((fd, buffer, offsetOrOptions = 0, length, position = null) => {
	let offset = offsetOrOptions;
	if (typeof offsetOrOptions === "object" && offsetOrOptions !== null) ({ offset = 0, length, position = null } = offsetOrOptions);
	length ??= buffer.byteLength - offset;
	return n.read(fd, buffer, offset, length, position == null ? -1 : Number(position));
});
export const writeSync = wrap((fd, data, offsetOrPosition, lengthOrEncoding, position) => {
	if (typeof data === "string") {
		const bytes = toBytes(data, typeof lengthOrEncoding === "string" ? lengthOrEncoding : undefined);
		return n.write(fd, bytes, 0, bytes.length, offsetOrPosition == null ? -1 : Number(offsetOrPosition));
	}
	const offset = offsetOrPosition ?? 0;
	const length = lengthOrEncoding ?? data.byteLength - offset;
	return n.write(fd, data, offset, length, position == null ? -1 : Number(position));
});
export const readFileSync = wrap((path, options) => {
	const bytes = typeof path === "number" ? readAllFromFd(path) : n.readFile(toPath(path));
	const encoding = encodingOf(options);
	const buffer = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	return encoding ? buffer.toString(encoding) : buffer;
});
function readAllFromFd(fd) {
	const chunks = [];
	let total = 0;
	for (;;) {
		const chunk = new Uint8Array(65536);
		const got = n.read(fd, chunk, 0, chunk.length, -1);
		if (got === 0) break;
		chunks.push(chunk.subarray(0, got));
		total += got;
	}
	const out = new Uint8Array(total);
	let at = 0;
	for (const c of chunks) { out.set(c, at); at += c.length; }
	return out;
}
export const writeFileSync = wrap((path, data, options) => {
	const flags = flagsOf(typeof options === "object" && options?.flag ? options.flag : "w");
	const mode = typeof options === "object" && options?.mode != null ? options.mode : 0o666;
	const bytes = toBytes(data, encodingOf(options));
	if (typeof path === "number") return void n.write(path, bytes, 0, bytes.length, -1);
	n.writeFile(toPath(path), bytes, flags, mode);
});
export const appendFileSync = (path, data, options) =>
	writeFileSync(path, data, { ...(typeof options === "object" ? options : { encoding: options }), flag: "a" });
export const mkdirSync = wrap((path, options) => {
	const recursive = typeof options === "object" && !!options?.recursive;
	const mode = typeof options === "object" ? options?.mode ?? 0o777 : options ?? 0o777;
	if (!recursive) return void n.mkdir(toPath(path), mode, false);
	let first = toPath(path);
	while (!existsSync(first)) {
		const parent = first.replace(/\/+[^/]+\/*$/, "") || "/";
		if (parent === first || existsSync(parent)) break;
		first = parent;
	}
	const created = existsSync(first) ? undefined : first;
	n.mkdir(toPath(path), mode, true);
	return created;
});
export const rmSync = wrap((path, options) => {
	const p = toPath(path);
	let st;
	try { st = n.stat(p, true); }
	catch (error) {
		if (options?.force && error.errno === -C_ENOENT) return;
		throw error;
	}
	if ((st.mode & C.S_IFMT) === C.S_IFDIR && !options?.recursive) {
		const e = new Error(`Path is a directory: rm returned EISDIR (is a directory) ${p}`);
		Object.assign(e, { code: "ERR_FS_EISDIR", errno: globalThis.__node.errnos.EISDIR, syscall: "rm", path: p });
		throw e;
	}
	n.rm(p, !!options?.recursive, !!options?.force);
});
export const rmdirSync = wrap((path, options) => (options?.recursive ? n.rm(toPath(path), true, false) : n.rmdir(toPath(path))));
export const unlinkSync = wrap((path) => n.unlink(toPath(path)));
export const renameSync = wrap((from, to) => n.rename(toPath(from), toPath(to)));
export const realpathSync = Object.assign(wrap((path) => n.realpath(toPath(path))), { native: wrap((path) => n.realpath(toPath(path))) });
export const chmodSync = wrap((path, mode) => n.chmod(toPath(path), typeof mode === "string" ? parseInt(mode, 8) : mode));
export const readdirSync = wrap((path, options) => {
	const names = n.readdir(toPath(path)).sort();
	if (!options?.withFileTypes) return names;
	return names.map((name) => {
		const st = new Stats(n.stat(toPath(path) + "/" + name, true));
		return Object.assign(st, { name, parentPath: toPath(path), path: toPath(path) });
	});
});
export const copyFileSync = wrap((from, to) => writeFileSync(to, readFileSync(from)));
export const mkdtempSync = wrap((prefix) => {
	const chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	const bytes = new Uint8Array(6);
	for (let attempt = 0; ; attempt++) {
		globalThis.__node.randomFill(bytes);
		const path = toPath(prefix) + Array.from(bytes, (b) => chars[b % chars.length]).join("");
		try {
			n.mkdir(path, 0o700, false);
			return path;
		} catch (error) {
			if (error.errno !== -globalThis.__node.errnos.EEXIST || attempt > 100) throw error;
		}
	}
});
const notYet = (name) => () => {
	throw new Error(`fs.${name} isn't available on the JSC runtime yet`);
};
export const fchmodSync = notYet("fchmodSync");
export const opendirSync = notYet("opendirSync");
export const symlinkSync = wrap((target, path) => n.symlink(toPath(target), toPath(path)));
export const linkSync = wrap((from, to) => n.link(toPath(from), toPath(to)));
export const readlinkSync = wrap((path) => n.readlink(toPath(path)));
const toSeconds = (t) => (t instanceof Date ? t.getTime() / 1000 : typeof t === "string" ? Number(t) : t);
export const utimesSync = wrap((path, atime, mtime) => n.utimes(toPath(path), toSeconds(atime), toSeconds(mtime)));
export function read(fd, buffer, offset, length, position, callback) {
	queueMicrotask(() => {
		let bytesRead;
		try { bytesRead = readSync(fd, buffer, offset, length, position); } catch (e) { return callback(e); }
		callback(null, bytesRead, buffer);
	});
}

// Promise and callback forms: the sync calls, deferred a tick.
const asPromise = (fn) => (...args) => new Promise((resolve, reject) => queueMicrotask(() => { try { resolve(fn(...args)); } catch (e) { reject(e); } }));
const asCallback = (fn) => (...args) => {
	const cb = typeof args[args.length - 1] === "function" ? args.pop() : () => {};
	queueMicrotask(() => { let r; try { r = fn(...args); } catch (e) { return cb(e); } cb(null, r); });
};
const syncApi = { stat: statSync, lstat: lstatSync, access: accessSync, readFile: readFileSync, writeFile: writeFileSync, appendFile: appendFileSync, mkdir: mkdirSync, rm: rmSync, rmdir: rmdirSync, unlink: unlinkSync, rename: renameSync, realpath: realpathSync, chmod: chmodSync, readdir: readdirSync, copyFile: copyFileSync, symlink: symlinkSync, link: linkSync, readlink: readlinkSync, utimes: utimesSync };
export const promises = Object.fromEntries(Object.entries(syncApi).map(([k, fn]) => [k, asPromise(fn)]));
export const { stat, lstat, access, readFile, writeFile, appendFile, mkdir, rm, rmdir, unlink, rename, realpath, chmod, readdir, copyFile, symlink, link, readlink, utimes } = Object.fromEntries(Object.entries(syncApi).map(([k, fn]) => [k, asCallback(fn)]));
export const exists = (path, cb) => queueMicrotask(() => cb(existsSync(path)));
export function watch() {
	throw new Error("fs.watch isn't available on the JSC runtime yet");
}
export function createReadStream() {
	throw new Error("fs.createReadStream isn't available on the JSC runtime yet");
}
export function createWriteStream() {
	throw new Error("fs.createWriteStream isn't available on the JSC runtime yet");
}
export default {
	constants, statSync, lstatSync, fstatSync, existsSync, accessSync, openSync, closeSync, fsyncSync, readSync, writeSync,
	readFileSync, writeFileSync, appendFileSync, mkdirSync, rmSync, rmdirSync, unlinkSync, renameSync, realpathSync, chmodSync,
	readdirSync, copyFileSync, mkdtempSync, fchmodSync, opendirSync, symlinkSync, linkSync, readlinkSync, utimesSync, read, promises, stat, lstat, access, readFile, writeFile, appendFile, mkdir, rm, rmdir, unlink, rename,
	realpath, chmod, readdir, copyFile, symlink, link, readlink, utimes, exists, watch, createReadStream, createWriteStream, Stats,
};
export { Stats };
