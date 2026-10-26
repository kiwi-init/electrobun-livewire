import { afterEach, describe, expect, test } from "bun:test";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { buildUipack, generateSigningKey, signManifest } from "./format";
import { placeFiles, pruneFiles, readFolder, readVerifiedPack } from "./served";

const dirs: string[] = [];
const tmp = () => {
	const dir = mkdtempSync(join(tmpdir(), "lw-served-"));
	dirs.push(dir);
	return dir;
};
afterEach(() => {
	for (const dir of dirs.splice(0)) rmSync(dir, { recursive: true, force: true });
});
const files = (entries: Record<string, string>) => new Map(Object.entries(entries).map(([path, value]) => [path, new TextEncoder().encode(value)]));

describe("served files", () => {
	test("placeFiles writes what's missing or different and leaves the rest", () => {
		const dir = tmp();
		placeFiles(dir, files({ "assets/a.js": "a", "icon.svg": "<svg/>" }));
		writeFileSync(join(dir, "assets/a.js"), "tampered");
		mkdirSync(join(dir, "icon.svg.d"));
		placeFiles(dir, files({ "assets/a.js": "a", "assets/b.js": "b" }));
		expect(readFileSync(join(dir, "assets/a.js"), "utf8")).toBe("a");
		expect(readFileSync(join(dir, "assets/b.js"), "utf8")).toBe("b");
		expect(readFileSync(join(dir, "icon.svg"), "utf8")).toBe("<svg/>");
	});

	test("placeFiles replaces a folder or symlink sitting where a file goes", () => {
		const dir = tmp();
		const outside = join(tmp(), "outside.js");
		writeFileSync(outside, "outside");
		mkdirSync(join(dir, "assets"));
		symlinkSync(outside, join(dir, "assets/a.js"));
		mkdirSync(join(dir, "b.js"));
		placeFiles(dir, files({ "assets/a.js": "a", "b.js": "b" }));
		expect(readFileSync(join(dir, "assets/a.js"), "utf8")).toBe("a");
		expect(readFileSync(outside, "utf8")).toBe("outside");
		expect(readFileSync(join(dir, "b.js"), "utf8")).toBe("b");
	});

	test("placeFiles refuses paths that could leave the folder", () => {
		expect(() => placeFiles(tmp(), files({ "../x.js": "x" }))).toThrow('file "../x.js" has an empty, . or .. segment');
	});

	test("pruneFiles keeps only the listed files and drops folders it empties", () => {
		const dir = tmp();
		placeFiles(dir, files({ "assets/a.js": "a", "assets/old/b.js": "b", "c.css": "c" }));
		pruneFiles(dir, new Set(["assets/a.js"]));
		expect(readFolder(dir)).toEqual(files({ "assets/a.js": "a" }));
		expect(existsSync(join(dir, "assets/old"))).toBe(false);
		expect(existsSync(dir)).toBe(true);
	});

	test("readFolder refuses symlinks", () => {
		const dir = tmp();
		symlinkSync("/etc/hosts", join(dir, "hosts"));
		expect(() => readFolder(dir)).toThrow("is a symlink");
	});

	test("readVerifiedPack checks the signature and the pack's bytes, not expiry", () => {
		const key = generateSigningKey();
		const other = generateSigningKey();
		const dir = tmp();
		const { bytes, sha256 } = buildUipack("1.0.0", files({ "index.html": "hi" }));
		const pack = join(dir, `${sha256}.uipack`);
		writeFileSync(pack, bytes);
		const manifest = signManifest(
			{
				format: "livewire-manifest/1",
				keyId: key.keyId,
				seq: 1,
				uiVersion: "1.0.0",
				bundleUrl: "x.uipack",
				sha256,
				requiresContract: "^1.0.0",
				dataSchema: 0,
				publishedAt: 1_000,
				expiresAt: 2_000,
			},
			key.privateKeyPem,
		);
		const trusted = { [key.keyId]: key.publicKey };
		const read = readVerifiedPack(pack, manifest, trusted);
		expect(read.ok && new TextDecoder().decode(read.value.get("index.html"))).toBe("hi");
		expect(readVerifiedPack(pack, { ...manifest, seq: 2 }, trusted)).toEqual({ ok: false, reason: "bad signature" });
		expect(readVerifiedPack(pack, manifest, { [other.keyId]: other.publicKey })).toEqual({ ok: false, reason: `key "${key.keyId}" isn't trusted` });
		writeFileSync(pack, new TextEncoder().encode("tampered"));
		expect(readVerifiedPack(pack, manifest, trusted)).toEqual({ ok: false, reason: "bundle hash doesn't match the manifest" });
	});
});
