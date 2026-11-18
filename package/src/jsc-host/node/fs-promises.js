import { Buffer } from "buffer";
import fs from "./fs.js";
export const { stat, lstat, access, readFile, writeFile, appendFile, mkdir, rm, rmdir, unlink, rename, realpath, chmod, readdir, copyFile, symlink, link, readlink, utimes } = fs.promises;
export const mkdtemp = fs.promises.mkdtemp ?? (async (prefix, options) => fs.mkdtempSync(prefix, options));

// fs/promises' FileHandle, on the sync fd calls (each method settles at once).
export class FileHandle {
	constructor(fd) {
		this.fd = fd;
	}
	_fd() {
		if (this.fd < 0) {
			const err = new Error("file closed");
			err.code = "EBADF";
			throw err;
		}
		return this.fd;
	}
	async write(data, ...rest) {
		return { bytesWritten: fs.writeSync(this._fd(), data, ...rest), buffer: data };
	}
	async read(buffer, ...rest) {
		if (buffer === undefined || (buffer !== null && typeof buffer === "object" && !ArrayBuffer.isView(buffer))) {
			const options = buffer ?? {};
			buffer = options.buffer ?? Buffer.alloc(16384);
			rest = [options.offset ?? 0, options.length ?? buffer.byteLength - (options.offset ?? 0), options.position ?? null];
		}
		return { bytesRead: fs.readSync(this._fd(), buffer, ...rest), buffer };
	}
	async readFile(options) {
		return fs.readFileSync(this._fd(), options);
	}
	async writeFile(data, options) {
		return fs.writeFileSync(this._fd(), data, options);
	}
	async appendFile(data, options) {
		return fs.appendFileSync(this._fd(), data, options);
	}
	async stat(options) {
		return fs.fstatSync(this._fd(), options);
	}
	async sync() {
		fs.fsyncSync(this._fd());
	}
	async datasync() {
		fs.fsyncSync(this._fd());
	}
	async close() {
		if (this.fd < 0) return;
		const fd = this.fd;
		this.fd = -1;
		fs.closeSync(fd);
	}
	async [Symbol.asyncDispose]() {
		await this.close();
	}
}
export const open = async (path, flags = "r", mode = 0o666) => new FileHandle(fs.openSync(path, flags, mode));

export default { ...fs.promises, mkdtemp, open };
