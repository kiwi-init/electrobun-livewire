import * as fs from "node:fs";
import { readFile, stat as statAsync, writeFile as writeFileAsync, readdir as readdirAsync, mkdir as mkdirAsync, rm as rmAsync } from "node:fs/promises";
import { join } from "node:path";

const root = join(process.env.LIVEWIRE_SCRATCH, "fs-" + process.pid);
const show = (p) => String(p).split(root).join("<root>");
const log = (...a) => console.log(...a.map(show));
const err = (label, fn) => {
	try { fn(); log(label, "no error"); }
	catch (e) { log(label, e.code, e.syscall, e.errno, typeof e.path, e instanceof Error, e.message); }
};

const t = (label, fn) => {
	try { const v = fn(); log(label, ...(Array.isArray(v) ? v : [v])); }
	catch (e) { log(label, "threw", e.code, e.message); }
};
fs.mkdirSync(root);
t("mkdir recursive returns", () => [fs.mkdirSync(join(root, "a/b/c"), { recursive: true })]);
t("mkdir recursive again", () => [fs.mkdirSync(join(root, "a/b/c"), { recursive: true })]);
err("mkdir exists", () => fs.mkdirSync(join(root, "a")));
err("mkdir no parent", () => fs.mkdirSync(join(root, "x/y")));

const f = join(root, "a/file.txt");
t("write", () => { fs.writeFileSync(f, "héllo"); return []; });
t("read utf8", () => [fs.readFileSync(f, "utf8"), fs.readFileSync(f, { encoding: "utf8" })]);
const raw = fs.readFileSync(f);
t("read buffer", () => [Buffer.isBuffer(raw), raw.length, raw.toString("hex")]);
t("appendFile", () => { fs.appendFileSync(f, " world"); return []; });
t("write flag a", () => { fs.writeFileSync(f, "!", { flag: "a" }); return []; });
t("append", () => [fs.readFileSync(f, "utf8")]);
t("write Uint8Array", () => { fs.writeFileSync(f, new Uint8Array([104, 105])); return []; });
t("write bytes", () => [fs.readFileSync(f, "utf8")]);
t("read latin1/base64", () => [fs.readFileSync(f, "latin1"), fs.readFileSync(f, "base64")]);

const st = fs.statSync(f);
t("stat", () => [st.isFile(), st.isDirectory(), st.isSymbolicLink(), st.size, (st.mode & 0o777).toString(8), typeof st.mtimeMs, st.mtime instanceof Date, st.mtime.getTime() === Math.floor(st.mtimeMs) || Math.abs(st.mtime.getTime() - st.mtimeMs) < 1]);
t("chmodSync", () => { fs.chmodSync(f, 0o600); return []; });
t("chmod", () => [(fs.statSync(f).mode & 0o777).toString(8)]);
t("utimesSync", () => { fs.utimesSync(f, 1000, 2000); return []; });
t("utimes", () => [fs.statSync(f).atimeMs, fs.statSync(f).mtimeMs]);
t("stat dir", () => [fs.statSync(join(root, "a")).isDirectory()]);
err("stat missing", () => fs.statSync(join(root, "missing")));
t("stat throwIfNoEntry", () => [fs.statSync(join(root, "missing"), { throwIfNoEntry: false })]);
err("read missing", () => fs.readFileSync(join(root, "missing")));
err("read dir", () => fs.readFileSync(join(root, "a")));
err("open in a file", () => fs.openSync(join(f, "x"), "r"));
err("rmdir not empty", () => fs.rmdirSync(join(root, "a")));
t("exists", () => [fs.existsSync(f), fs.existsSync(join(root, "missing"))]);
err("access missing", () => fs.accessSync(join(root, "missing")));
t("accessSync R_OK|W_OK", () => { fs.accessSync(f, fs.constants.R_OK | fs.constants.W_OK); return []; });

t("symlinkSync", () => { fs.symlinkSync(f, join(root, "link")); return []; });
t("lstat link", () => [fs.lstatSync(join(root, "link")).isSymbolicLink(), fs.statSync(join(root, "link")).isFile()]);
t("readlink", () => [fs.readlinkSync(join(root, "link"))]);
t("realpath", () => [fs.realpathSync(join(root, "link")) === fs.realpathSync(f)]);
t("linkSync", () => { fs.linkSync(f, join(root, "hard")); return []; });
t("nlink", () => [fs.statSync(f).nlink]);

t("write z.txt", () => { fs.writeFileSync(join(root, "a/z.txt"), "z"); return []; });
t("readdir", () => [fs.readdirSync(join(root, "a")).sort().join(",")]);
t("readdir types", () => [fs.readdirSync(join(root, "a"), { withFileTypes: true }).map((d) => d.name + ":" + (d.isDirectory() ? "dir" : d.isFile() ? "file" : "other")).sort().join(",")]);
err("readdir missing", () => fs.readdirSync(join(root, "missing")));

t("renameSync", () => { fs.renameSync(join(root, "a/z.txt"), join(root, "a/y.txt")); return []; });
t("rename", () => [fs.existsSync(join(root, "a/z.txt")), fs.readFileSync(join(root, "a/y.txt"), "utf8")]);
t("copyFileSync", () => { fs.copyFileSync(join(root, "a/y.txt"), join(root, "a/copy.txt")); return []; });
t("copy", () => [fs.readFileSync(join(root, "a/copy.txt"), "utf8")]);
t("unlinkSync", () => { fs.unlinkSync(join(root, "a/copy.txt")); return []; });
err("unlink missing", () => fs.unlinkSync(join(root, "a/copy.txt")));

const fd = fs.openSync(join(root, "fd.bin"), "w+");
t("fd", () => [typeof fd, fd > 2]);
t("write returns", () => [fs.writeSync(fd, "abcdef"), fs.writeSync(fd, Buffer.from("XY"), 0, 2, 1)]);
const buf = Buffer.alloc(8);
t("read returns", () => [fs.readSync(fd, buf, 0, 8, 0), buf.toString("utf8", 0, 6)]);
t("fstat size", () => [fs.fstatSync(fd).size]);
t("fsyncSync", () => { fs.fsyncSync(fd); return []; });
t("closeSync", () => { fs.closeSync(fd); return []; });
err("close twice", () => fs.closeSync(fd));
const tmp = fs.mkdtempSync(join(root, "tmp-"));
t("mkdtemp", () => [show(tmp).replace(/tmp-.{6}$/, "tmp-XXXXXX"), fs.statSync(tmp).isDirectory()]);
t("rm force missing", () => { fs.rmSync(join(root, "nope"), { force: true }); return []; });
err("rm missing", () => fs.rmSync(join(root, "nope")));
err("rm dir", () => fs.rmSync(join(root, "a")));

(async () => {
	try { await (async () => {
	log("promises read", await readFile(join(root, "a/y.txt"), "utf8"));
	await writeFileAsync(join(root, "p.txt"), "async");
	log("promises stat", (await statAsync(join(root, "p.txt"))).size);
	await mkdirAsync(join(root, "p/q"), { recursive: true });
	log("promises readdir", (await readdirAsync(root)).filter((n) => !n.startsWith("tmp-")).sort().join(","));
	try { await readFile(join(root, "missing")); } catch (e) { log("promises missing", e.code, e.syscall, e.message); }
	})(); } catch (e) { log("promises threw", e.code, e.message); }
	await rmAsync(root, { recursive: true, force: true });
	log("removed", fs.existsSync(root));
})();
