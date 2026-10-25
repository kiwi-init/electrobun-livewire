// AppEngine boot lifecycle: boot, probation, rollback, hang recovery and safe
// mode, against signed UIs published to a local host.

import { mkdtempSync } from "fs";
import { tmpdir } from "os";
import { join } from "path";
import { Utils } from "electrobun/main";
import {
	AppEngine,
	buildUipack,
	generateSigningKey,
	signManifest,
	uipackFileName,
	type AppEngineEvent,
	type UiManifest,
} from "electrobun/main/appengine";
import { defineTest, expect } from "../test-framework/types";

const key = generateSigningKey();

const UIS = {
	good: (label: string) => `<!doctype html><h1>${label}</h1><script>window.engine.ready()</script>`,
	crash: () => `<!doctype html><script>throw new Error("boom before ready")</script>`,
	hang: () => `<!doctype html><script>for (;;) {}</script>`,
	lateCrash: () =>
		`<!doctype html><script>window.engine.ready(); setTimeout(() => { throw new Error("boom on probation") }, 300)</script>`,
};

// A static UI host plus an AppEngine with its own state and install root.
function setup(createWindow: (options: any) => Promise<any>, testName: string) {
	const bundles = new Map<string, Uint8Array>();
	let manifest: UiManifest | null = null;
	let seq = 0;
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
	const publish = (html: string, dataSchema = 1) => {
		seq++;
		const uiVersion = `${testName.replace(/[^a-z0-9]/gi, "")}-${seq}`;
		const { bytes, sha256 } = buildUipack(uiVersion, new Map([["index.html", new TextEncoder().encode(html)]]));
		const bundleUrl = `bundles/${uipackFileName(uiVersion, sha256)}`;
		bundles.set(bundleUrl, bytes);
		manifest = signManifest(
			{
				format: "livewire-manifest/1",
				keyId: key.keyId,
				seq,
				uiVersion,
				bundleUrl,
				sha256,
				requiresContract: "^1.0.0",
				dataSchema,
				publishedAt: Date.now(),
			},
			key.privateKeyPem,
		);
		return manifest;
	};
	const events: AppEngineEvent[] = [];
	let engine: AppEngine | null = null;
	const state = mkdtempSync(join(tmpdir(), "kitchen-appengine-"));
	return {
		publish,
		events,
		async start() {
			const win = await createWindow({ title: `AppEngine: ${testName}` });
			engine = new AppEngine({
				window: win.window,
				safeModeUrl: "views://test-harness/index.html",
				readyTimeoutMs: 3000,
				probationMs: 1000,
				stateFile: join(state, "engine.json"),
				channel: {
					manifestUrl: `http://127.0.0.1:${server.port}/manifest.json`,
					trustedKeys: { [key.keyId]: key.publicKey },
					// Each test installs into its own folder, so engines running in
					// parallel can't prune each other's installs.
					root: join(Utils.paths.appdataProtocolRoot, "ui", `kitchen-${seq}-${Date.now()}`),
					stateFile: join(state, "channel.json"),
					pollIntervalMs: 60_000,
				},
			});
			engine.on((event) => events.push(event));
			await engine.start();
			return engine;
		},
		check: () => engine!.channel.check(),
		stop() {
			engine?.stop();
			server.stop(true);
		},
	};
}

async function until(events: AppEngineEvent[], predicate: (event: AppEngineEvent) => boolean, timeoutMs = 10_000) {
	const deadline = Date.now() + timeoutMs;
	for (;;) {
		const match = events.find(predicate);
		if (match) return match;
		if (Date.now() > deadline) throw new Error(`timed out; events: ${JSON.stringify(events.map(describe))}`);
		await new Promise((resolve) => setTimeout(resolve, 50));
	}
}

const describe = (event: AppEngineEvent) =>
	"manifest" in event ? `${event.type} ${event.manifest.uiVersion}` : `${event.type}: ${event.reason}`;
const is = (type: AppEngineEvent["type"], manifest?: UiManifest) => (event: AppEngineEvent) =>
	event.type === type && (!manifest || ("manifest" in event && event.manifest.sha256 === manifest.sha256));

export const appEngineTests = [
	defineTest({
		name: "AppEngine boots a published UI and promotes it after probation",
		category: "AppEngine",
		description: "A signed UI installs, boots, calls engine.ready() and becomes a rollback target",
		timeout: 30000,
		async run({ createWindow, log }) {
			const t = setup(createWindow, "boot");
			try {
				const v1 = t.publish(UIS.good("v1"));
				const engine = await t.start();
				await until(t.events, is("ready", v1));
				await until(t.events, is("promoted", v1));
				expect(engine.showing?.sha256).toBe(v1.sha256);
				log(t.events.map(describe).join(" → "));
			} finally {
				t.stop();
			}
		},
	}),

	defineTest({
		name: "AppEngine rolls back a UI that crashes before ready()",
		category: "AppEngine",
		description: "An uncaught error before engine.ready() fails the boot and the previous UI comes back",
		timeout: 30000,
		async run({ createWindow, log }) {
			const t = setup(createWindow, "crash");
			try {
				const v1 = t.publish(UIS.good("v1"));
				const engine = await t.start();
				await until(t.events, is("promoted", v1));
				const v2 = t.publish(UIS.crash());
				await t.check();
				const failed = await until(t.events, is("failed", v2));
				expect(failed.type === "failed" && failed.reason).toContain("uncaught error before ready()");
				await until(t.events, (e) => e.type === "ready" && e.manifest.sha256 === v1.sha256 && t.events.indexOf(e) > t.events.indexOf(failed));
				expect(engine.showing?.sha256).toBe(v1.sha256);
				// The failed bundle is refused if it's offered again.
				log(t.events.map(describe).join(" → "));
			} finally {
				t.stop();
			}
		},
	}),

	defineTest({
		name: "AppEngine recovers from a UI that hangs before ready()",
		category: "AppEngine",
		description: "A UI stuck in a loop misses the ready() deadline; its renderer is killed and the previous UI boots in a fresh view",
		timeout: 30000,
		async run({ createWindow, log }) {
			const t = setup(createWindow, "hang");
			try {
				const v1 = t.publish(UIS.good("v1"));
				const engine = await t.start();
				await until(t.events, is("promoted", v1));
				const v2 = t.publish(UIS.hang());
				await t.check();
				const failed = await until(t.events, is("failed", v2));
				expect(failed.type === "failed" && failed.reason).toContain("no ready() within");
				await until(t.events, (e) => e.type === "ready" && e.manifest.sha256 === v1.sha256 && t.events.indexOf(e) > t.events.indexOf(failed));
				expect(engine.showing?.sha256).toBe(v1.sha256);
				log(t.events.map(describe).join(" → "));
			} finally {
				t.stop();
			}
		},
	}),

	defineTest({
		name: "AppEngine rolls back a UI that crashes during probation",
		category: "AppEngine",
		description: "A UI that boots but throws during probation is never promoted and the previous UI comes back",
		timeout: 30000,
		async run({ createWindow, log }) {
			const t = setup(createWindow, "probation");
			try {
				const v1 = t.publish(UIS.good("v1"));
				const engine = await t.start();
				await until(t.events, is("promoted", v1));
				const v2 = t.publish(UIS.lateCrash());
				await t.check();
				await until(t.events, is("ready", v2));
				const failed = await until(t.events, is("failed", v2));
				expect(failed.type === "failed" && failed.reason).toContain("crashed during probation");
				expect(t.events.some(is("promoted", v2))).toBe(false);
				await until(t.events, (e) => e.type === "ready" && e.manifest.sha256 === v1.sha256 && t.events.indexOf(e) > t.events.indexOf(failed));
				expect(engine.showing?.sha256).toBe(v1.sha256);
				log(t.events.map(describe).join(" → "));
			} finally {
				t.stop();
			}
		},
	}),

	defineTest({
		name: "AppEngine falls back to safe mode when no UI works",
		category: "AppEngine",
		description: "With no rollback target, a UI that fails leaves the engine on its safe-mode page",
		timeout: 30000,
		async run({ createWindow, log }) {
			const t = setup(createWindow, "safemode");
			try {
				const v1 = t.publish(UIS.crash());
				const engine = await t.start();
				await until(t.events, is("failed", v1));
				await until(t.events, (e) => e.type === "safe-mode" && e.reason.includes("failed"));
				expect(engine.showing).toBe(null);
				log(t.events.map(describe).join(" → "));
			} finally {
				t.stop();
			}
		},
	}),
];
