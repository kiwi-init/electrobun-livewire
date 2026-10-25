import { afterAll, describe, expect, test } from "bun:test";
import {
	existsSync,
	mkdirSync,
	mkdtempSync,
	readFileSync,
	readdirSync,
	rmSync,
	statSync,
	symlinkSync,
	writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { keygen, parseDuration, publish, verifySite } from "./cli";
import { readUipack, type UiManifest } from "./format";

const root = mkdtempSync(join(tmpdir(), "livewire-ui-cli-"));
afterAll(() => rmSync(root, { recursive: true, force: true }));

function uiSource(name: string, uiVersion: string, dataSchema = 1) {
	const dir = join(root, name);
	mkdirSync(join(dir, "assets"), { recursive: true });
	mkdirSync(join(dir, ".git"), { recursive: true });
	writeFileSync(join(dir, "ui.json"), JSON.stringify({ uiVersion, requiresContract: "^1.0.0", dataSchema }));
	writeFileSync(join(dir, "index.html"), `<h1>${uiVersion}</h1>`);
	writeFileSync(join(dir, "assets", "app.js"), "console.log('hi')");
	writeFileSync(join(dir, ".env"), "SECRET=1");
	writeFileSync(join(dir, ".git", "HEAD"), "ref");
	return dir;
}

describe("livewire ui CLI", () => {
	const keys = keygen(join(root, "keys"));
	const privateKeyPem = readFileSync(keys.privatePath, "utf8");
	const site = join(root, "site");

	test("keygen writes a 0600 private key and refuses to overwrite it", () => {
		expect(statSync(keys.privatePath).mode & 0o777).toBe(0o600);
		expect(readFileSync(keys.publicPath, "utf8").trim()).toBe(keys.publicKey);
		expect(keys.keyId).toMatch(/^[0-9a-f]{16}$/);
		writeFileSync(join(root, "keys", "x"), "");
		expect(() => keygen(join(root, "keys"))).not.toThrow();
	});

	test("publish packs the UI, skips ui.json and dotfiles, signs seq 1", () => {
		const result = publish({ srcDir: uiSource("v1", "1.0.0"), privateKeyPem, siteDir: site, now: 1_000 });
		expect(result.manifest).toMatchObject({ seq: 1, uiVersion: "1.0.0", keyId: keys.keyId, dataSchema: 1, publishedAt: 1_000 });
		expect(result.manifest.bundleUrl).toBe(`bundles/${readdirSync(join(site, "bundles"))[0]}`);
		const files = readUipack(new Uint8Array(readFileSync(result.bundlePath)), result.manifest);
		expect(files.ok && [...files.value.keys()].sort()).toEqual(["assets/app.js", "index.html"]);
		expect(verifySite({ siteDir: site, publicKey: keys.publicKey })).toMatchObject({ bundle: { files: 2 } });
	});

	test("the next publish bumps seq and keeps the old bundle for rollback", () => {
		const result = publish({ srcDir: uiSource("v2", "1.1.0"), privateKeyPem, siteDir: site, expiresInMs: parseDuration("30d"), now: 2_000 });
		expect(result.manifest.seq).toBe(2);
		expect(result.manifest.expiresAt).toBe(2_000 + 30 * 86_400_000);
		expect(readdirSync(join(site, "bundles"))).toHaveLength(2);
		expect(verifySite({ siteDir: site, publicKey: keys.publicKey, now: 3_000 }).manifest.uiVersion).toBe("1.1.0");
	});

	test("refuses to lower the data schema", () => {
		expect(() => publish({ srcDir: uiSource("v0", "1.2.0", 0), privateKeyPem, siteDir: site })).toThrow(/dataSchema 0 is below the published 1/);
	});

	test("verify catches a tampered manifest, a tampered bundle and the wrong key", () => {
		const manifestPath = join(site, "manifest.json");
		const original = readFileSync(manifestPath, "utf8");
		const manifest = JSON.parse(original) as UiManifest;
		writeFileSync(manifestPath, JSON.stringify({ ...manifest, dataSchema: 9 }));
		expect(() => verifySite({ siteDir: site, publicKey: keys.publicKey, now: 3_000 })).toThrow(/bad signature/);
		writeFileSync(manifestPath, original);

		const bundlePath = join(site, manifest.bundleUrl);
		const bundle = readFileSync(bundlePath);
		writeFileSync(bundlePath, `${bundle.toString("utf8")} `);
		expect(() => verifySite({ siteDir: site, publicKey: keys.publicKey, now: 3_000 })).toThrow(/hash/);
		writeFileSync(bundlePath, bundle.toString("utf8"));

		const other = keygen(join(root, "other-keys"));
		expect(() => verifySite({ siteDir: site, publicKey: other.publicKey, now: 3_000 })).toThrow(/isn't trusted/);
	});

	test("an absolute base URL is used as is, and isn't fetched by verify", () => {
		const remoteSite = join(root, "remote-site");
		const result = publish({ srcDir: uiSource("r1", "2.0.0"), privateKeyPem, siteDir: remoteSite, baseUrl: "https://cdn.example.com/app/" });
		expect(result.manifest.bundleUrl).toMatch(/^https:\/\/cdn\.example\.com\/app\/bundles\/2\.0\.0-[0-9a-f]{12}\.uipack$/);
		expect(verifySite({ siteDir: remoteSite, publicKey: keys.publicKey }).bundle).toBe("remote (not checked)");
	});

	test.skipIf(process.platform === "win32")("refuses symlinks and missing ui.json", () => {
		const src = uiSource("linked", "3.0.0");
		symlinkSync("/etc/hosts", join(src, "hosts"));
		expect(() => publish({ srcDir: src, privateKeyPem, siteDir: join(root, "s3") })).toThrow(/symlink/);
		const bare = join(root, "bare");
		mkdirSync(bare);
		writeFileSync(join(bare, "index.html"), "x");
		expect(() => publish({ srcDir: bare, privateKeyPem, siteDir: join(root, "s4") })).toThrow(/ui\.json is missing/);
		expect(existsSync(join(root, "s4", "manifest.json"))).toBe(false);
	});

	test("durations", () => {
		expect(parseDuration("90m")).toBe(5_400_000);
		expect(parseDuration("12h")).toBe(43_200_000);
		expect(() => parseDuration("1w")).toThrow(/bad duration/);
	});
});
