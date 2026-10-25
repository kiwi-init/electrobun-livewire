// Strips debug info and symbol tables from a Linux release's ELF files before
// they're packed: without it, the core tarball's own builds (bsdiff, bspatch,
// zig-zstd, extractor, zig-asar, libasar, libNativeWrapper*, process_helper)
// and libwebgpu_dawn.so carry about 38 MB of DWARF and symbols that apps then
// ship. CEF's own binaries (dist/cef) stay as CEF distributes them.

import { execFileSync } from "node:child_process";
import { closeSync, lstatSync, openSync, readSync, readdirSync } from "node:fs";
import { join, relative } from "node:path";

const ELF_MAGIC = Buffer.from([0x7f, 0x45, 0x4c, 0x46]);

function isElf(file) {
	const fd = openSync(file, "r");
	try {
		const head = Buffer.alloc(4);
		return readSync(fd, head, 0, 4, 0) === 4 && head.equals(ELF_MAGIC);
	} finally {
		closeSync(fd);
	}
}

function elfFiles(root, dir = root) {
	return readdirSync(dir).flatMap((name) => {
		const path = join(dir, name);
		const stat = lstatSync(path);
		if (stat.isDirectory()) return dir === root && name === "cef" ? [] : elfFiles(root, path);
		return stat.isFile() && isElf(path) ? [path] : [];
	});
}

// --strip-unneeded drops debug sections and the static symbol table but keeps
// the dynamic symbols that shared libraries export and executables import.
export function stripLinuxRelease(distPath, { strip = "strip", run = execFileSync, log = console.log } = {}) {
	let before = 0;
	let after = 0;
	const files = elfFiles(distPath);
	for (const file of files) {
		const size = lstatSync(file).size;
		run(strip, ["--strip-unneeded", file], { stdio: "inherit" });
		const stripped = lstatSync(file).size;
		before += size;
		after += stripped;
		log(`  ${relative(distPath, file)}: ${(size / 1048576).toFixed(1)} → ${(stripped / 1048576).toFixed(1)} MB`);
	}
	log(`Stripped ${files.length} ELF files: ${(before / 1048576).toFixed(1)} → ${(after / 1048576).toFixed(1)} MB`);
	return { files: files.map((file) => relative(distPath, file)), before, after };
}
