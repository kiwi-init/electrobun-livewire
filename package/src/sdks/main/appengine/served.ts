// Host mode serving: the files of the UI on screen, in the folder the app's own
// view serves views:// from (its viewsRoot). The engine owns that folder and
// rewrites it from a verified source before every boot, so what the page loads
// matches the signed bundle (or the app's built-in UI) at boot time.
//
// Files of different UIs share the folder, so names should be content-hashed
// (as bundlers emit them). A file two UIs both have is rewritten with the
// booting UI's bytes, and files no UI on screen uses are pruned after it boots.

import { lstatSync, mkdirSync, readFileSync, readdirSync, rmSync, rmdirSync } from "node:fs";
import { dirname, join, relative, sep } from "node:path";
import { readUipack, uipackPathError, verifyManifest, type Result, type TrustedKeys, type UiManifest } from "./format";
import { writeAtomic } from "./storage";

export type UiFiles = Map<string, Uint8Array>;

// The files of a kept pack, after checking its manifest is still signed by a
// trusted key and the pack still hashes to what the manifest says. Expiry is
// not checked: a UI that installed before its manifest expired may still boot.
export function readVerifiedPack(pack: string, manifest: UiManifest, trustedKeys: TrustedKeys): Result<UiFiles> {
	const signed = verifyManifest(manifest, trustedKeys, manifest.publishedAt);
	if (!signed.ok) return signed;
	let bytes: Uint8Array;
	try {
		bytes = new Uint8Array(readFileSync(pack));
	} catch (error) {
		return { ok: false, reason: `can't read ${pack}: ${String(error)}` };
	}
	return readUipack(bytes, manifest);
}

// Every regular file under `dir`, by relative path. Symlinks are refused.
export function readFolder(dir: string): UiFiles {
	const files: UiFiles = new Map();
	const walk = (folder: string) => {
		for (const entry of readdirSync(folder).sort()) {
			const path = join(folder, entry);
			const stat = lstatSync(path);
			if (stat.isSymbolicLink()) throw new Error(`${path} is a symlink`);
			if (stat.isDirectory()) walk(path);
			else if (stat.isFile()) files.set(relative(dir, path).split(sep).join("/"), new Uint8Array(readFileSync(path)));
		}
	};
	walk(dir);
	return files;
}

// Makes `dir` hold `files` byte for byte, rewriting any file that differs.
export function placeFiles(dir: string, files: UiFiles) {
	for (const [path, content] of files) {
		const error = uipackPathError(path);
		if (error) throw new Error(`file "${path}" ${error}`);
		const target = join(dir, path);
		if (sameBytes(target, content)) continue;
		const stat = statOrNull(target);
		if (stat && !stat.isFile()) rmSync(target, { recursive: true, force: true });
		mkdirSync(dirname(target), { recursive: true });
		writeAtomic(target, content);
	}
}

// Deletes every file under `dir` whose relative path isn't in `keep`, and the
// folders that leaves empty.
export function pruneFiles(dir: string, keep: ReadonlySet<string>) {
	const walk = (folder: string): boolean => {
		let entries: string[];
		try {
			entries = readdirSync(folder);
		} catch {
			return false;
		}
		let empty = true;
		for (const entry of entries) {
			const path = join(folder, entry);
			const stat = statOrNull(path);
			if (stat?.isDirectory()) {
				if (walk(path)) rmdirSync(path);
				else empty = false;
			} else if (keep.has(relative(dir, path).split(sep).join("/"))) {
				empty = false;
			} else {
				rmSync(path, { force: true });
			}
		}
		return empty;
	};
	walk(dir);
}

function statOrNull(path: string) {
	try {
		return lstatSync(path);
	} catch {
		return null;
	}
}

function sameBytes(path: string, content: Uint8Array) {
	const stat = statOrNull(path);
	if (!stat?.isFile() || stat.size !== content.byteLength) return false;
	try {
		return readFileSync(path).equals(content);
	} catch {
		return false;
	}
}
