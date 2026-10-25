import { describe, expect, test } from "bun:test";
import {
	buildUipack,
	canonicalManifestBytes,
	generateSigningKey,
	keyIdFor,
	manifestShapeError,
	publicKeyFromPrivate,
	readUipack,
	sha256Hex,
	signManifest,
	uipackFileName,
	uipackPathError,
	verifyManifest,
	type UnsignedUiManifest,
} from "./format";

const key = generateSigningKey();
const trusted = { [key.keyId]: key.publicKey };

function unsigned(overrides: Partial<UnsignedUiManifest> = {}): UnsignedUiManifest {
	return {
		format: "livewire-manifest/1",
		keyId: key.keyId,
		seq: 3,
		uiVersion: "1.4.0",
		bundleUrl: "bundles/1.4.0-abc.uipack",
		sha256: "a".repeat(64),
		requiresContract: "^1.2.0",
		dataSchema: 2,
		publishedAt: 1_700_000_000_000,
		...overrides,
	};
}

describe("manifest signing", () => {
	test("canonical bytes sort keys and leave out the signature", () => {
		const bytes = new TextDecoder().decode(
			canonicalManifestBytes({ ...unsigned(), signature: "ignored" } as never),
		);
		const keys = Object.keys(JSON.parse(bytes));
		expect(keys).toEqual([...keys].sort());
		expect(keys).not.toContain("signature");
		expect(bytes).not.toContain(" ");
		// Field order in the object doesn't change the bytes.
		const reversed = Object.fromEntries(Object.entries(unsigned()).reverse()) as UnsignedUiManifest;
		expect(canonicalManifestBytes(reversed)).toEqual(canonicalManifestBytes(unsigned()));
	});

	test("a signed manifest verifies, and survives a JSON round trip", () => {
		const signed = signManifest(unsigned(), key.privateKeyPem);
		const result = verifyManifest(JSON.parse(JSON.stringify(signed)), trusted);
		expect(result).toEqual({ ok: true, value: signed });
	});

	test("any changed field breaks the signature", () => {
		const signed = signManifest(unsigned(), key.privateKeyPem);
		for (const change of [{ seq: 4 }, { sha256: "b".repeat(64) }, { bundleUrl: "https://evil.example/x.uipack" }, { dataSchema: 1 }]) {
			expect(verifyManifest({ ...signed, ...change }, trusted)).toEqual({ ok: false, reason: "bad signature" });
		}
		expect(verifyManifest({ ...signed, expiresAt: Date.now() + 1_000 }, trusted)).toEqual({ ok: false, reason: "bad signature" });
	});

	test("refuses untrusted keys, other keys under a trusted id, and missing signatures", () => {
		const other = generateSigningKey();
		const byOther = signManifest(unsigned({ keyId: other.keyId }), other.privateKeyPem);
		expect(verifyManifest(byOther, trusted)).toMatchObject({ ok: false, reason: expect.stringContaining("isn't trusted") });
		const impostor = signManifest(unsigned(), other.privateKeyPem);
		expect(verifyManifest(impostor, trusted)).toEqual({ ok: false, reason: "bad signature" });
		expect(verifyManifest(unsigned(), trusted)).toEqual({ ok: false, reason: "manifest is unsigned" });
		expect(verifyManifest(signManifest(unsigned(), key.privateKeyPem), { toString: key.publicKey } as never)).toMatchObject({ ok: false });
	});

	test("expiry is enforced", () => {
		const signed = signManifest(unsigned({ expiresAt: 2_000 }), key.privateKeyPem);
		expect(verifyManifest(signed, trusted, 1_999).ok).toBe(true);
		expect(verifyManifest(signed, trusted, 2_000)).toEqual({ ok: false, reason: "manifest has expired" });
	});

	test("rejects malformed manifests before checking signatures", () => {
		expect(manifestShapeError(null)).toBe("not an object");
		expect(manifestShapeError({ ...unsigned(), extra: 1 })).toBe('unknown field "extra"');
		expect(manifestShapeError({ ...unsigned(), format: "v0" })).toContain("format");
		expect(manifestShapeError(unsigned({ seq: 0 }))).toContain("seq");
		expect(manifestShapeError(unsigned({ seq: 1.5 }))).toContain("seq");
		expect(manifestShapeError(unsigned({ sha256: "A".repeat(64) }))).toContain("sha256");
		expect(manifestShapeError(unsigned({ uiVersion: "../x" }))).toBe("bad uiVersion");
		expect(manifestShapeError(unsigned({ dataSchema: -1 }))).toContain("dataSchema");
		expect(manifestShapeError(unsigned())).toBeNull();
		expect(() => signManifest(unsigned({ seq: 0 }), key.privateKeyPem)).toThrow(/seq/);
	});

	test("keys: keyId derives from the public key, which derives from the private key", () => {
		expect(publicKeyFromPrivate(key.privateKeyPem)).toBe(key.publicKey);
		expect(Buffer.from(key.publicKey, "base64url")).toHaveLength(32);
		expect(keyIdFor(key.publicKey)).toBe(key.keyId);
		expect(key.keyId).toMatch(/^[0-9a-f]{16}$/);
		expect(key.privateKeyPem).toStartWith("-----BEGIN PRIVATE KEY-----");
	});
});

describe("uipack", () => {
	const files = new Map<string, Uint8Array>([
		["index.html", new TextEncoder().encode("<h1>hi</h1>")],
		["assets/app.js", new TextEncoder().encode("console.log(1)")],
		["assets/logo.png", new Uint8Array([0, 255, 128, 7])],
	]);

	test("builds deterministically and reads back byte for byte", () => {
		const a = buildUipack("1.4.0", files);
		const b = buildUipack("1.4.0", new Map([...files].reverse()));
		expect(a.sha256).toBe(b.sha256);
		expect(a.sha256).toBe(sha256Hex(a.bytes));
		expect(a.fileCount).toBe(3);
		const read = readUipack(a.bytes, { sha256: a.sha256, uiVersion: "1.4.0" });
		expect(read.ok).toBe(true);
		if (read.ok) expect(read.value).toEqual(files);
	});

	test("refuses a wrong hash, a wrong version, or a bad path inside", () => {
		const { bytes, sha256 } = buildUipack("1.4.0", files);
		expect(readUipack(bytes, { sha256: "0".repeat(64), uiVersion: "1.4.0" })).toMatchObject({ ok: false, reason: expect.stringContaining("hash") });
		expect(readUipack(bytes, { sha256, uiVersion: "1.5.0" })).toMatchObject({ ok: false, reason: expect.stringContaining("uiVersion") });
		const evil = new TextEncoder().encode(JSON.stringify({ format: "uipack/1", uiVersion: "1.4.0", files: { "../escape.js": "" } }));
		expect(readUipack(evil, { sha256: sha256Hex(evil), uiVersion: "1.4.0" })).toMatchObject({ ok: false, reason: expect.stringContaining("../escape.js") });
	});

	test("path rules", () => {
		for (const ok of ["index.html", "a/b/c.js", "x-y_z.v2.css"]) expect(uipackPathError(ok)).toBeNull();
		for (const bad of ["", "../x", "a/./b", "a//b", "/abs", "sp ace.js", "ü.js", "a\\b", "x".repeat(200), "dir./a.js", "a.", "CON", "nul.txt", "x/Com1.js", "lpt9"]) {
			expect(uipackPathError(bad)).not.toBeNull();
		}
		expect(() => buildUipack("1.4.0", new Map([["../x", new Uint8Array()]]))).toThrow(/\.\.\/x/);
		expect(() => buildUipack("bad version", files)).toThrow(/uiVersion/);
		for (const ok of ["console.js", "nullable.js", "a.b/c"]) expect(uipackPathError(ok)).toBeNull();
	});

	test("refuses paths that differ only in case", () => {
		const twins = new Map([["App.js", new Uint8Array([1])], ["app.js", new Uint8Array([2])]]);
		expect(() => buildUipack("1.4.0", twins)).toThrow(/differ only in case/);
		const evil = new TextEncoder().encode(JSON.stringify({ format: "uipack/1", uiVersion: "1.4.0", files: { "A.js": "", "a.js": "" } }));
		expect(readUipack(evil, { sha256: sha256Hex(evil), uiVersion: "1.4.0" })).toMatchObject({ ok: false, reason: expect.stringContaining("differ only in case") });
	});

	test("file names", () => {
		expect(uipackFileName("1.4.0", "abcdef0123456789".padEnd(64, "0"))).toBe("1.4.0-abcdef012345.uipack");
	});
});
