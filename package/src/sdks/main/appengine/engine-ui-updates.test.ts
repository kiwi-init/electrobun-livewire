import { afterEach, describe, expect, mock, test } from "bun:test";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import type { BrowserWindow } from "../core/BrowserWindow";
import { AppEngine, uiUpdatesEnabled, type AppEngineEvent, type AppEngineOptions, type HostUi } from "./engine";
import { buildUipack, generateSigningKey, signManifest, uipackFileName, type UiManifest } from "./format";

// The default display, without a native view: records what it's asked to show.
const mounted: string[] = [];
mock.module("./view-display", () => ({
	ViewDisplay: class {
		mount(url: string) {
			mounted.push(url);
		}
		load() {}
		replace() {}
		getView() {
			return null;
		}
	},
}));

const key = generateSigningKey();
const trustedKeys = { [key.keyId]: key.publicKey };
const encode = (value: string) => new TextEncoder().encode(value);
const text = (bytes: Uint8Array) => new TextDecoder().decode(bytes);

// A UI host with one published UI that counts every request it gets.
function startHost() {
	const uiVersion = "2.0.2000";
	const { bytes, sha256 } = buildUipack(uiVersion, new Map([["index.html", encode("<p>v2</p>")]]));
	const name = uipackFileName(uiVersion, sha256);
	const manifest: UiManifest = signManifest(
		{
			format: "livewire-manifest/1",
			keyId: key.keyId,
			seq: 2_000,
			uiVersion,
			bundleUrl: name,
			sha256,
			requiresContract: "^1.0.0",
			dataSchema: 1,
			publishedAt: Date.now(),
		},
		key.privateKeyPem,
	);
	const requests: string[] = [];
	const server = Bun.serve({
		hostname: "127.0.0.1",
		port: 0,
		fetch(request) {
			const path = new URL(request.url).pathname.slice(1);
			requests.push(path);
			if (path === "manifest.json") return Response.json(manifest);
			if (path === name) return new Response(new Blob([bytes as Uint8Array<ArrayBuffer>]));
			return new Response("not found", { status: 404 });
		},
	});
	return { manifestUrl: `http://127.0.0.1:${server.port}/manifest.json`, requests, stop: () => server.stop(true) };
}

const cleanups: (() => void)[] = [];
afterEach(() => {
	for (const cleanup of cleanups.splice(0)) cleanup();
	mounted.length = 0;
});

function rig(dir: string, manifestUrl: string, extra: Partial<AppEngineOptions>) {
	const events: AppEngineEvent[] = [];
	const shown: HostUi[] = [];
	const engine = new AppEngine({
		contractVersion: "1.0.0",
		stateFile: join(dir, "engine.json"),
		readyTimeoutMs: 2_000,
		probationMs: 20,
		channel: {
			manifestUrl,
			trustedKeys,
			root: join(dir, "packs"),
			stateFile: join(dir, "channel.json"),
			pollIntervalMs: 60_000,
		},
		host: {
			viewsRoot: join(dir, "views"),
			prefix: "internal",
			builtIn: { uiVersion: "built-in", seq: 1_000, files: () => new Map([["index.html", encode("<p>built-in</p>")]]) },
			show: (ui) => void shown.push(ui),
		},
		...extra,
	});
	engine.on((event) => void events.push(event));
	cleanups.push(() => engine.stop());
	return { engine, events, shown };
}

function setup() {
	const dir = mkdtempSync(join(tmpdir(), "lw-engine-ui-updates-"));
	const host = startHost();
	cleanups.push(() => {
		host.stop();
		rmSync(dir, { recursive: true, force: true });
	});
	return { dir, host };
}

async function waitFor(probe: () => boolean, timeoutMs = 3_000) {
	const deadline = Date.now() + timeoutMs;
	while (!probe()) {
		if (Date.now() > deadline) throw new Error("timed out");
		await Bun.sleep(5);
	}
}

describe("uiUpdatesEnabled", () => {
	test("is on by default", () => {
		expect(uiUpdatesEnabled(undefined, undefined)).toBe(true);
		expect(uiUpdatesEnabled({}, undefined)).toBe(true);
		expect(uiUpdatesEnabled({ uiUpdates: true }, true)).toBe(true);
	});

	test("runtime.uiUpdates: false in the build turns it off, and the app's option can't turn it back on", () => {
		expect(uiUpdatesEnabled({ uiUpdates: false }, undefined)).toBe(false);
		expect(uiUpdatesEnabled({ uiUpdates: false }, true)).toBe(false);
	});

	test("the app's option can turn it off", () => {
		expect(uiUpdatesEnabled(undefined, false)).toBe(false);
		expect(uiUpdatesEnabled({ uiUpdates: true }, false)).toBe(false);
	});
});

describe("AppEngine with UI updates off", () => {
	test("host mode shows the built-in UI and never contacts the channel", async () => {
		const { dir, host } = setup();
		const { engine, events, shown } = rig(dir, host.manifestUrl, { uiUpdates: false });
		await engine.start();
		await Bun.sleep(50);

		expect(shown.map((ui) => [ui.uiVersion, text(ui.index)])).toEqual([["built-in", "<p>built-in</p>"]]);
		expect(events).toEqual([{ type: "built-in", reason: "UI updates are off in this build" }]);
		expect(host.requests).toEqual([]);
	});

	test("a check the app runs on engine.channel is refused before anything is downloaded", async () => {
		const { dir, host } = setup();
		const { engine, shown } = rig(dir, host.manifestUrl, { uiUpdates: false });
		await engine.start();
		const result = await engine.channel.check();

		expect(result).toMatchObject({ result: "refused" });
		expect(host.requests).toEqual(["manifest.json"]);
		expect(shown).toHaveLength(1);
	});

	test("a UI installed while updates were on isn't booted once they're off", async () => {
		const { dir, host } = setup();
		const on = rig(dir, host.manifestUrl, {});
		await on.engine.start();
		await waitFor(() => on.shown.length === 2);
		on.engine.ready();
		await waitFor(() => on.events.some((event) => event.type === "promoted"));
		on.engine.stop();
		host.requests.length = 0;

		const off = rig(dir, host.manifestUrl, { uiUpdates: false });
		await off.engine.start();
		expect(off.shown.map((ui) => ui.uiVersion)).toEqual(["built-in"]);
		expect(off.engine.showing).toBeNull();
		expect(host.requests).toEqual([]);
	});

	test("the default display shows safeModeUrl and never contacts the channel", async () => {
		const { dir, host } = setup();
		const { engine, events } = rig(dir, host.manifestUrl, {
			uiUpdates: false,
			host: undefined,
			window: {} as BrowserWindow,
			safeModeUrl: "views://safemode/index.html",
		});
		await engine.start();
		await Bun.sleep(50);

		expect(mounted).toEqual(["views://safemode/index.html"]);
		expect(events).toEqual([{ type: "safe-mode", reason: "UI updates are off in this build" }]);
		expect(host.requests).toEqual([]);
	});
});
