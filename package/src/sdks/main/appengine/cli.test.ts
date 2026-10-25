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

function uiSource(name: string, uiVersion: string, dataSchema = 1, requiresContract = "^1.0.0") {
	const dir = join(root, name);
	mkdirSync(join(dir, "assets"), { recursive: true });
	mkdirSync(join(dir, ".git"), { recursive: true });
	writeFileSync(join(dir, "ui.json"), JSON.stringify({ uiVersion, requiresContract, dataSchema }));
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

	test("keygen writes a 0600 private key named after its keyId", () => {
		expect(statSync(keys.privatePath).mode & 0o777).toBe(0o600);
		expect(readFileSync(keys.publicPath, "utf8").trim()).toBe(keys.publicKey);
		expect(keys.keyId).toMatch(/^[0-9a-f]{16}$/);
		expect(keys.privatePath).toEndWith(`${keys.keyId}.private.pem`);
	});

	test("publish packs the UI, skips ui.json and dotfiles, signs with seq = publish time", async () => {
		const result = await publish({ srcDir: uiSource("v1", "1.0.0"), privateKeyPem, siteDir: site, now: 1_000 });
		expect(result.manifest).toMatchObject({ seq: 1_000, uiVersion: "1.0.0", keyId: keys.keyId, dataSchema: 1, publishedAt: 1_000 });
		expect(result.manifest.bundleUrl).toBe(`bundles/${readdirSync(join(site, "bundles"))[0]}`);
		const files = readUipack(new Uint8Array(readFileSync(result.bundlePath)), result.manifest);
		expect(files.ok && [...files.value.keys()].sort()).toEqual(["assets/app.js", "index.html"]);
		expect(verifySite({ siteDir: site, publicKey: keys.publicKey })).toMatchObject({ bundle: { files: 2 } });
	});

	test("the next publish raises seq and keeps the old bundle for rollback", async () => {
		const result = await publish({ srcDir: uiSource("v2", "1.1.0"), privateKeyPem, siteDir: site, expiresInMs: parseDuration("30d"), now: 2_000 });
		expect(result.manifest.seq).toBe(2_000);
		expect(result.manifest.expiresAt).toBe(2_000 + 30 * 86_400_000);
		expect(readdirSync(join(site, "bundles"))).toHaveLength(2);
		expect(verifySite({ siteDir: site, publicKey: keys.publicKey, now: 3_000 }).manifest.uiVersion).toBe("1.1.0");
	});

	test("refuses to lower the data schema", async () => {
		await expect(publish({ srcDir: uiSource("v0", "1.2.0", 0), privateKeyPem, siteDir: site })).rejects.toThrow(/dataSchema 0 is below the published 1/);
	});

	test("refuses a requiresContract that isn't a semver range", async () => {
		await expect(publish({ srcDir: uiSource("typo", "1.2.0", 1, "^1.O.0"), privateKeyPem, siteDir: site })).rejects.toThrow(/isn't a semver range/);
	});

	test("a fresh checkout still publishes past the live seq", async () => {
		// Clock behind the live seq: previous + 1 wins.
		const fresh = join(root, "fresh-site");
		const fromPath = await publish({ srcDir: uiSource("f1", "1.3.0"), privateKeyPem, siteDir: fresh, previous: join(site, "manifest.json"), now: 1_500 });
		expect(fromPath.manifest.seq).toBe(2_001);
		// Even with no record at all, the publish time is far above any counter-based seq.
		const bare = await publish({ srcDir: uiSource("f2", "1.3.1"), privateKeyPem, siteDir: join(root, "bare-site") });
		expect(bare.manifest.seq).toBeGreaterThan(2_001);
	});

	test("--previous can be the live manifest's URL; it also drives the dataSchema guard", async () => {
		const live = JSON.parse(readFileSync(join(site, "manifest.json"), "utf8"));
		const server = Bun.serve({
			port: 0,
			fetch: (request) => (new URL(request.url).pathname === "/manifest.json" ? Response.json({ ...live, seq: 9_999_999_999_999, dataSchema: 5 }) : new Response("no", { status: 404 })),
		});
		try {
			const url = `http://127.0.0.1:${server.port}/manifest.json`;
			await expect(publish({ srcDir: uiSource("u1", "1.4.0", 4), privateKeyPem, siteDir: join(root, "url-site"), previous: url })).rejects.toThrow(/below the published 5/);
			const result = await publish({ srcDir: uiSource("u2", "1.4.1", 5), privateKeyPem, siteDir: join(root, "url-site2"), previous: url });
			expect(result.manifest.seq).toBe(10_000_000_000_000);
			const missing = await publish({ srcDir: uiSource("u3", "1.4.2"), privateKeyPem, siteDir: join(root, "url-site3"), previous: `http://127.0.0.1:${server.port}/none.json`, now: 7 });
			expect(missing.manifest.seq).toBe(7);
		} finally {
			server.stop(true);
		}
		await expect(publish({ srcDir: uiSource("u4", "1.4.3"), privateKeyPem, siteDir: join(root, "url-site4"), previous: join(root, "nope.json") })).rejects.toThrow(/doesn't exist/);
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

	test("an absolute base URL is used as is, and isn't fetched by verify", async () => {
		const remoteSite = join(root, "remote-site");
		const result = await publish({ srcDir: uiSource("r1", "2.0.0"), privateKeyPem, siteDir: remoteSite, baseUrl: "https://cdn.example.com/app/" });
		expect(result.manifest.bundleUrl).toMatch(/^https:\/\/cdn\.example\.com\/app\/bundles\/2\.0\.0-[0-9a-f]{12}\.uipack$/);
		expect(verifySite({ siteDir: remoteSite, publicKey: keys.publicKey }).bundle).toBe("remote (not checked)");
	});

	test.skipIf(process.platform === "win32")("refuses symlinks and missing ui.json", async () => {
		const src = uiSource("linked", "3.0.0");
		symlinkSync("/etc/hosts", join(src, "hosts"));
		await expect(publish({ srcDir: src, privateKeyPem, siteDir: join(root, "s3") })).rejects.toThrow(/symlink/);
		const bare = join(root, "bare");
		mkdirSync(bare);
		writeFileSync(join(bare, "index.html"), "x");
		await expect(publish({ srcDir: bare, privateKeyPem, siteDir: join(root, "s4") })).rejects.toThrow(/ui\.json is missing/);
		expect(existsSync(join(root, "s4", "manifest.json"))).toBe(false);
	});

	test("--flat puts the bundle next to manifest.json, for hosts without folders", async () => {
		const flatSite = join(root, "flat-site");
		const result = await publish({ srcDir: uiSource("flat", "4.0.0"), privateKeyPem, siteDir: flatSite, flat: true });
		expect(result.manifest.bundleUrl).toMatch(/^4\.0\.0-[0-9a-f]{12}\.uipack$/);
		expect(readdirSync(flatSite).sort()).toEqual([result.manifest.bundleUrl, "manifest.json"].sort());
		expect(verifySite({ siteDir: flatSite, publicKey: keys.publicKey }).bundle).toEqual({ files: 2 });
		const based = await publish({ srcDir: uiSource("flat-based", "4.0.1"), privateKeyPem, siteDir: flatSite, flat: true, baseUrl: "https://example.com/dl/" });
		expect(based.manifest.bundleUrl).toBe(`https://example.com/dl/${based.manifest.bundleUrl.split("/").at(-1)}`);
		expect(based.manifest.bundleUrl).not.toContain("/bundles/");
	});

	test("durations", () => {
		expect(parseDuration("90m")).toBe(5_400_000);
		expect(parseDuration("12h")).toBe(43_200_000);
		expect(() => parseDuration("1w")).toThrow(/bad duration/);
	});
});
