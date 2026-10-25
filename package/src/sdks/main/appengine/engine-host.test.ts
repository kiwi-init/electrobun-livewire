import { afterEach, describe, expect, test } from "bun:test";
import { existsSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { AppEngine, type AppEngineEvent, type HostUi } from "./engine";
import { buildUipack, generateSigningKey, signManifest, uipackFileName, type UiManifest } from "./format";

const key = generateSigningKey();
const trustedKeys = { [key.keyId]: key.publicKey };
const BUILT_IN_SEQ = 1_000;
const text = (bytes: Uint8Array) => new TextDecoder().decode(bytes);
const encode = (value: string) => new TextEncoder().encode(value);

// A static UI host serving manifest.json and flat bundles.
function startHost() {
	let manifest: UiManifest | null = null;
	const bundles = new Map<string, Uint8Array>();
	const server = Bun.serve({
		hostname: "127.0.0.1",
		port: 0,
		fetch(request) {
			const name = new URL(request.url).pathname.slice(1);
			if (name === "manifest.json") return manifest ? Response.json(manifest) : new Response("none", { status: 404 });
			const bundle = bundles.get(name);
			return bundle ? new Response(new Blob([bundle as Uint8Array<ArrayBuffer>])) : new Response("not found", { status: 404 });
		},
	});
	return {
		manifestUrl: `http://127.0.0.1:${server.port}/manifest.json`,
		publish(seq: number, files: Record<string, string>, options: { dataSchema?: number } = {}) {
			const uiVersion = `2.0.${seq}`;
			const { bytes, sha256 } = buildUipack(uiVersion, new Map(Object.entries(files).map(([path, value]) => [path, encode(value)])));
			const name = uipackFileName(uiVersion, sha256);
			bundles.set(name, bytes);
			manifest = signManifest(
				{
					format: "livewire-manifest/1",
					keyId: key.keyId,
					seq,
					uiVersion,
					bundleUrl: name,
					sha256,
					requiresContract: "^1.0.0",
					dataSchema: options.dataSchema ?? 1,
					publishedAt: Date.now(),
				},
				key.privateKeyPem,
			);
			return manifest;
		},
		stop: () => server.stop(true),
	};
}

// Rigs to stop and delete after each test; a restarted rig takes over its folder.
const rigs: { dir: string; engine: AppEngine | null }[] = [];

function makeRig(options: { dir?: string; canSwap?: () => boolean; readyTimeoutMs?: number } = {}) {
	const dir = options.dir ?? mkdtempSync(join(tmpdir(), "lw-engine-host-"));
	const viewsRoot = join(dir, "views");
	const shown: HostUi[] = [];
	const events: AppEngineEvent[] = [];
	let engine: AppEngine | null = null;
	const rig = {
		dir,
		viewsRoot,
		shown,
		events,
		start(manifestUrl: string) {
			engine = new AppEngine({
				contractVersion: "1.0.0",
				stateFile: join(dir, "engine.json"),
				readyTimeoutMs: options.readyTimeoutMs ?? 2_000,
				probationMs: 50,
				channel: {
					manifestUrl,
					trustedKeys,
					root: join(dir, "packs"),
					stateFile: join(dir, "channel.json"),
					pollIntervalMs: 60_000,
				},
				host: {
					viewsRoot,
					prefix: "internal",
					builtIn: {
						uiVersion: "built-in",
						seq: BUILT_IN_SEQ,
						files: () =>
							new Map([
								["index.html", encode("<p>built-in</p>")],
								["assets/app-builtin.js", encode("builtin()")],
							]),
					},
					show: (ui) => void shown.push(ui),
					canSwap: options.canSwap,
				},
			});
			engine.on((event) => void events.push(event));
			return engine.start().then(() => engine!);
		},
		get engine(): AppEngine {
			return engine!;
		},
		served(path: string) {
			const file = join(viewsRoot, "internal", path);
			return existsSync(file) ? readFileSync(file, "utf8") : null;
		},
		lastShown() {
			const ui = shown.at(-1)!;
			return { uiVersion: ui.uiVersion, index: text(ui.index), fresh: ui.fresh };
		},
		until(predicate: (event: AppEngineEvent) => boolean) {
			return waitFor(() => events.find(predicate));
		},
	};
	rigs.push(rig);
	return rig;
}

async function waitFor<T>(probe: () => T | undefined | null | false, timeoutMs = 3_000): Promise<T> {
	const deadline = Date.now() + timeoutMs;
	for (;;) {
		const value = probe();
		if (value) return value;
		if (Date.now() > deadline) throw new Error("timed out");
		await Bun.sleep(5);
	}
}

const hosts: ReturnType<typeof startHost>[] = [];
function host() {
	const started = startHost();
	hosts.push(started);
	return started;
}

afterEach(() => {
	for (const rig of rigs.splice(0)) {
		rig.engine?.stop();
		rmSync(rig.dir, { recursive: true, force: true });
	}
	for (const started of hosts.splice(0)) started.stop();
});

describe("AppEngine host mode", () => {
	test("shows the built-in UI when nothing is installed, with its files under the viewsRoot and index.html kept out", async () => {
		const rig = makeRig();
		await rig.start(host().manifestUrl);
		expect(rig.lastShown()).toEqual({ uiVersion: "built-in", index: "<p>built-in</p>", fresh: false });
		expect(rig.served("assets/app-builtin.js")).toBe("builtin()");
		expect(rig.served("index.html")).toBeNull();
		expect(rig.events.find((event) => event.type === "built-in")).toBeTruthy();
		expect(await rig.engine.invoke("engine.info", null)).toMatchObject({ ok: true, value: { uiVersion: "built-in", seq: BUILT_IN_SEQ } });
	});

	test("boots a published UI, promotes it after probation, and prunes the built-in UI's files", async () => {
		const server = host();
		server.publish(2_000, { "index.html": "<p>v2</p>", "assets/app-2.js": "two()" });
		const rig = makeRig();
		await rig.start(server.manifestUrl);
		await waitFor(() => rig.shown.length === 2);
		expect(rig.lastShown()).toEqual({ uiVersion: "2.0.2000", index: "<p>v2</p>", fresh: false });
		expect(rig.served("assets/app-2.js")).toBe("two()");
		expect(rig.served("assets/app-builtin.js")).toBe("builtin()");

		rig.engine.ready();
		await rig.until((event) => event.type === "promoted");
		expect(rig.engine.showing?.uiVersion).toBe("2.0.2000");
		expect(rig.served("assets/app-builtin.js")).toBeNull();
		expect(readdirSync(join(rig.dir, "packs"))).toEqual([expect.stringMatching(/^[0-9a-f]{64}\.uipack$/)]);
	});

	test("an error before ready() bans the UI and goes back to the built-in one", async () => {
		const server = host();
		server.publish(2_000, { "index.html": "<p>v2</p>", "assets/app-2.js": "throw()" });
		const rig = makeRig();
		await rig.start(server.manifestUrl);
		await waitFor(() => rig.shown.length === 2);
		rig.engine.error("TypeError: boom\n    at app-2.js:1");
		const failed = await rig.until((event) => event.type === "failed");
		expect(failed).toMatchObject({ banned: true, reason: "uncaught error before ready(): TypeError: boom" });
		await waitFor(() => rig.shown.length === 3);
		expect(rig.lastShown().uiVersion).toBe("built-in");
		expect(rig.engine.showing).toBeNull();
	});

	test("a crash during probation rolls back to the last good UI", async () => {
		const server = host();
		server.publish(2_000, { "index.html": "<p>v2</p>", "assets/app-2.js": "two()" });
		const rig = makeRig();
		await rig.start(server.manifestUrl);
		await waitFor(() => rig.shown.length === 2);
		rig.engine.ready();
		await rig.until((event) => event.type === "promoted");

		server.publish(3_000, { "index.html": "<p>v3</p>", "assets/app-3.js": "three()" });
		await rig.engine.channel.check();
		await waitFor(() => rig.shown.length === 3);
		rig.engine.ready();
		rig.engine.error("ReferenceError: later");
		await waitFor(() => rig.shown.length === 4);
		expect(rig.lastShown().uiVersion).toBe("2.0.2000");
		rig.engine.ready();
		expect(rig.engine.showing?.uiVersion).toBe("2.0.2000");
		expect(rig.served("assets/app-3.js")).toBeNull();
	});

	test("canSwap() holds a new UI until applyUpdate(), and a restart boots it", async () => {
		const server = host();
		server.publish(2_000, { "index.html": "<p>v2</p>", "assets/app-2.js": "two()" });
		const rig = makeRig({ canSwap: () => false });
		await rig.start(server.manifestUrl);
		await rig.until((event) => event.type === "waiting");
		expect(rig.shown.length).toBe(1);
		rig.engine.applyUpdate();
		await waitFor(() => rig.shown.length === 2);
		expect(rig.lastShown().uiVersion).toBe("2.0.2000");

		const held = makeRig({ canSwap: () => false });
		server.publish(3_000, { "index.html": "<p>v3</p>", "assets/app-3.js": "three()" });
		await held.start(server.manifestUrl);
		await held.until((event) => event.type === "waiting");
		held.engine.stop();
		const restarted = makeRig({ dir: held.dir });
		rigs.splice(rigs.indexOf(held), 1);
		await restarted.start(server.manifestUrl);
		expect(restarted.shown[0]!.uiVersion).toBe("2.0.3000");
	});

	test("a pack changed on disk isn't booted: the built-in UI shows instead", async () => {
		const server = host();
		server.publish(2_000, { "index.html": "<p>v2</p>", "assets/app-2.js": "two()" });
		const first = makeRig();
		await first.start(server.manifestUrl);
		await waitFor(() => first.shown.length === 2);
		first.engine.ready();
		await first.until((event) => event.type === "promoted");
		first.engine.stop();

		const packs = join(first.dir, "packs");
		const pack = join(packs, readdirSync(packs)[0]!);
		writeFileSync(pack, readFileSync(pack, "utf8").replace("dHdvKCk=", "ZXZpbCgp"));
		const again = makeRig({ dir: first.dir });
		rigs.splice(rigs.indexOf(first), 1);
		await again.start(server.manifestUrl);
		expect(again.shown[0]!.uiVersion).toBe("built-in");
		expect(again.events.find((event) => event.type === "failed")).toMatchObject({
			banned: false,
			reason: "the installed pack doesn't check out: bundle hash doesn't match the manifest",
		});
	});

	test("rewrites a served file that changed on disk before booting", async () => {
		const server = host();
		server.publish(2_000, { "index.html": "<p>v2</p>", "assets/app-2.js": "two()" });
		const first = makeRig();
		await first.start(server.manifestUrl);
		await waitFor(() => first.shown.length === 2);
		first.engine.ready();
		await first.until((event) => event.type === "promoted");
		first.engine.stop();

		writeFileSync(join(first.viewsRoot, "internal", "assets", "app-2.js"), "evil()");
		const again = makeRig({ dir: first.dir });
		rigs.splice(rigs.indexOf(first), 1);
		await again.start(server.manifestUrl);
		expect(again.shown[0]!.uiVersion).toBe("2.0.2000");
		expect(again.served("assets/app-2.js")).toBe("two()");
	});

	test("refuses UIs not newer than the built-in one", async () => {
		const server = host();
		server.publish(BUILT_IN_SEQ, { "index.html": "<p>old</p>" });
		const rig = makeRig();
		await rig.start(server.manifestUrl);
		const result = await rig.engine.channel.check();
		expect(result).toEqual({ result: "refused", reason: "UI 2.0.1000 is older than the one this app ships (built-in)" });
		expect(rig.shown.length).toBe(1);
	});

	test("a missed ready() deadline is a strike: the UI gets one more try at the next start", async () => {
		const server = host();
		server.publish(2_000, { "index.html": "<p>v2</p>", "assets/app-2.js": "slow()" });
		const rig = makeRig({ readyTimeoutMs: 30 });
		await rig.start(server.manifestUrl);
		const failed = await rig.until((event) => event.type === "failed");
		expect(failed).toMatchObject({ banned: false });
		await waitFor(() => rig.shown.length === 3);
		expect(rig.lastShown()).toMatchObject({ uiVersion: "built-in", fresh: true });
		rig.engine.stop();

		const again = makeRig({ dir: rig.dir, readyTimeoutMs: 2_000 });
		rigs.splice(rigs.indexOf(rig), 1);
		await again.start(server.manifestUrl);
		expect(again.shown[0]!.uiVersion).toBe("2.0.2000");
	});

	test("needs a host, or a window and a safe mode URL", () => {
		expect(() => new AppEngine({ contractVersion: "1.0.0", channel: { manifestUrl: "https://example.invalid/m.json", trustedKeys } })).toThrow(
			"AppEngine needs either `host`, or `window` and `safeModeUrl`",
		);
	});
});
