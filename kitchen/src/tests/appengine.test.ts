// AppEngine boot lifecycle (boot, probation, rollback, hang recovery, safe
// mode) and contract, against signed UIs published to a local host.

import { mkdtempSync, readFileSync, writeFileSync } from "fs";
import { tmpdir } from "os";
import { join } from "path";
import { Utils } from "electrobun/main";
import {
	AppEngine,
	EngineStore,
	buildUipack,
	storageCapabilities,
	generateSigningKey,
	signManifest,
	uipackFileName,
	type AppEngineEvent,
	type EngineStorage,
	type UiChannelEvent,
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
function setup(
	createWindow: (options: any) => Promise<any>,
	testName: string,
	options: {
		probationMs?: number;
		storage?: EngineStorage;
		capabilities?: Record<string, (args: unknown) => unknown>;
	} = {},
) {
	const bundles = new Map<string, Uint8Array>();
	const fetched = new Set<string>();
	let manifest: UiManifest | null = null;
	let seq = 0;
	const server = Bun.serve({
		hostname: "127.0.0.1",
		port: 0,
		fetch(request) {
			const name = new URL(request.url).pathname.slice(1);
			if (name === "manifest.json") return manifest ? Response.json(manifest) : new Response("none", { status: 404 });
			const bundle = bundles.get(name);
			if (bundle) fetched.add(name);
			return bundle ? new Response(new Blob([bundle as Uint8Array<ArrayBuffer>])) : new Response("not found", { status: 404 });
		},
	});
	const publish = (html: string, dataSchema = 1, requiresContract = "^1.0.0") => {
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
				requiresContract,
				dataSchema,
				publishedAt: Date.now(),
			},
			key.privateKeyPem,
		);
		return manifest;
	};
	const events: AppEngineEvent[] = [];
	const channelEvents: UiChannelEvent[] = [];
	let engine: AppEngine | null = null;
	const state = mkdtempSync(join(tmpdir(), "kitchen-appengine-"));
	// Each test installs into its own folder, so engines running in parallel
	// can't prune each other's installs.
	const root = join(Utils.paths.appdataProtocolRoot, "ui", `kitchen-${testName}-${Date.now()}`);
	return {
		publish,
		events,
		channelEvents,
		stateDir: state,
		stateFile: join(state, "engine.json"),
		// Stops the engine and starts a new one on the same state: an app restart.
		async restart() {
			engine?.stop();
			return this.start();
		},
		fetched: (published: UiManifest) => fetched.has(published.bundleUrl),
		async start() {
			const win = await createWindow({ title: `AppEngine: ${testName}` });
			engine = new AppEngine({
				window: win.window,
				safeModeUrl: "views://test-harness/index.html",
				readyTimeoutMs: 3000,
				probationMs: options.probationMs ?? 1000,
				stateFile: join(state, "engine.json"),
				storage: options.storage,
				contractVersion: "1.0.0",
				capabilities: options.capabilities,
				channel: {
					manifestUrl: `http://127.0.0.1:${server.port}/manifest.json`,
					trustedKeys: { [key.keyId]: key.publicKey },
					root,
					stateFile: join(state, "channel.json"),
					pollIntervalMs: 60_000,
				},
			});
			engine.on((event) => events.push(event));
			engine.channel.on((event) => channelEvents.push(event));
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
		name: "AppEngine refuses a UI that needs a newer contract before downloading it",
		category: "AppEngine",
		description: "A manifest whose requiresContract the engine's contractVersion doesn't satisfy is refused and its bundle never fetched",
		timeout: 30000,
		async run({ createWindow, log }) {
			const t = setup(createWindow, "contract");
			try {
				const v1 = t.publish(UIS.good("v1"));
				const engine = await t.start();
				await until(t.events, is("promoted", v1));
				const v2 = t.publish(UIS.good("v2"), 1, "^2.0.0");
				const result = await t.check();
				expect(result.result).toBe("refused");
				expect(result.result === "refused" && result.reason).toContain('requires contract "^2.0.0", but this engine implements 1.0.0');
				expect(t.fetched(v2)).toBe(false);
				expect(engine.showing?.sha256).toBe(v1.sha256);
				log(`${describe(t.events.at(-1)!)}; refused: ${result.result === "refused" ? result.reason : ""}`);
			} finally {
				t.stop();
			}
		},
	}),

	defineTest({
		name: "AppEngine gives the UI its contract on window.engine",
		category: "AppEngine",
		description: "window.engine.contract and has(cap) list the engine's capabilities, and invoke() refuses names outside them",
		timeout: 30000,
		async run({ createWindow, log }) {
			let report: (value: unknown) => void = () => {};
			const reported = new Promise<any>((resolve) => (report = resolve));
			const t = setup(createWindow, "facade", { capabilities: { "test.report": (args) => void report(args) } });
			try {
				t.publish(`<!doctype html><script>
					const e = window.engine;
					e.invoke("constructor").then(() => "ok", (error) => error.code).then((inherited) =>
						e.invoke("test.report", {
							version: e.contract.version,
							capabilities: e.contract.capabilities,
							has: e.has("test.report"),
							hasInherited: e.has("constructor"),
							global: typeof window.__livewireEngineContract,
							frozen: Object.isFrozen(e.contract),
							inherited,
						}),
					).then(() => e.ready());
				</script>`);
				await t.start();
				const page = await reported;
				expect(page.version).toBe("1.0.0");
				expect(page.capabilities).toEqual(["engine.info", "session.takeHandoff", "test.report"]);
				expect(page.has).toBe(true);
				expect(page.hasInherited).toBe(false);
				expect(page.global).toBe("undefined");
				expect(page.frozen).toBe(true);
				expect(page.inherited).toBe("E_NO_CAPABILITY");
				log(JSON.stringify(page));
			} finally {
				t.stop();
			}
		},
	}),

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
		name: "AppEngine doesn't fail a UI whose renderer wake recovery recycled",
		category: "AppEngine",
		description: "A wake recycle during probation reloads the same UI; it isn't marked failed and still gets promoted",
		timeout: 40000,
		requires: { platform: "darwin" },
		async run({ createWindow, log }) {
			const t = setup(createWindow, "wake", { probationMs: 6000 });
			try {
				const v1 = t.publish(UIS.good("v1"));
				const engine = await t.start();
				await until(t.events, is("ready", v1));
				// Probing a closed port makes every view look frozen.
				const viewId = engine.getView()!.id;
				const recycled = await Utils.recoverWebviewsAfterWake({ settleMs: 0, timeoutMs: 1500, probePort: 1, ids: [viewId] });
				expect(recycled).toEqual([viewId]);
				await until(t.events, is("promoted", v1), 15000);
				expect(t.events.some((e) => e.type === "failed")).toBe(false);
				expect(engine.showing?.sha256).toBe(v1.sha256);
				log(t.events.map(describe).join(" → "));
			} finally {
				t.stop();
			}
		},
	}),

	defineTest({
		name: "AppEngine retries a UI that timed out once and bans it the second time",
		category: "AppEngine",
		description: "A missed ready() deadline can be the machine's fault: the UI gets one more try at the next startup, and is banned only if it fails again",
		timeout: 40000,
		async run({ createWindow, log }) {
			const t = setup(createWindow, "strikes");
			try {
				const v1 = t.publish(UIS.good("v1"));
				await t.start();
				await until(t.events, is("promoted", v1));
				const v2 = t.publish(UIS.hang());
				await t.check();
				const first = await until(t.events, is("failed", v2));
				expect(first.type === "failed" && first.banned).toBe(false);
				await until(t.events, (e) => e.type === "ready" && e.manifest.sha256 === v1.sha256 && t.events.indexOf(e) > t.events.indexOf(first));
				const saved = JSON.parse(readFileSync(t.stateFile, "utf8"));
				expect(saved.strikes[v2.sha256]).toBe(1);
				expect(saved.retry?.sha256).toBe(v2.sha256);
				expect(saved.bad[v2.sha256]).toBe(undefined);

				log("Restarting: the retry boots first");
				const before = t.events.length;
				const engine = await t.restart();
				await until(t.events, (e) => is("booting", v2)(e) && t.events.indexOf(e) >= before);
				const second = await until(t.events, (e) => is("failed", v2)(e) && t.events.indexOf(e) >= before);
				expect(second.type === "failed" && second.banned).toBe(true);
				await until(t.events, (e) => e.type === "ready" && e.manifest.sha256 === v1.sha256 && t.events.indexOf(e) > t.events.indexOf(second));
				expect(engine.showing?.sha256).toBe(v1.sha256);
				log(t.events.map(describe).join(" → "));
			} finally {
				t.stop();
			}
		},
	}),

	defineTest({
		name: "AppEngine starts with defaults when engine.json is damaged",
		category: "AppEngine",
		description: "A state file with the wrong shape falls back to defaults instead of breaking startup",
		timeout: 30000,
		async run({ createWindow, log }) {
			const t = setup(createWindow, "damaged");
			try {
				writeFileSync(t.stateFile, JSON.stringify({ current: 5, good: "x", bad: [1], strikes: { a: -1 }, dataSchemaHighWater: "y" }));
				const v1 = t.publish(UIS.good("v1"));
				const engine = await t.start();
				await until(t.events, is("ready", v1));
				expect(engine.showing?.sha256).toBe(v1.sha256);
				log(t.events.map(describe).join(" → "));
			} finally {
				t.stop();
			}
		},
	}),

	defineTest({
		name: "AppEngine hot-swaps to a new UI and hands it the old UI's state",
		category: "AppEngine",
		description: "The running UI's onBeforeSwap state reaches the next UI through session.takeHandoff (the spike's v1 to v2)",
		timeout: 30000,
		async run({ createWindow, log }) {
			const reports: unknown[] = [];
			const t = setup(createWindow, "handoff", { capabilities: { "test.report": (args) => reports.push(args) } });
			try {
				const v1 = t.publish(`<!doctype html><script>
					window.engine.onBeforeSwap(({ toVersion }) => ({ draft: "half-written note", toVersion }));
					window.engine.ready();
				</script>`);
				await t.start();
				await until(t.events, is("ready", v1));
				const v2 = t.publish(`<!doctype html><script>
					window.engine.session.takeHandoff()
						.then((state) => window.engine.invoke("test.report", state))
						.then(() => window.engine.ready());
				</script>`);
				await t.check();
				await until(t.events, is("ready", v2));
				expect(reports).toEqual([{ draft: "half-written note", toVersion: v2.uiVersion }]);
				log(t.events.map(describe).join(" → "));
			} finally {
				t.stop();
			}
		},
	}),

	defineTest({
		name: "AppEngine restores storage when a UI's migration crashes",
		category: "AppEngine",
		description: "A new UI migrates the stored data and then throws before ready(): the snapshot comes back and the previous UI reads its data intact (the spike's v5b)",
		timeout: 30000,
		async run({ createWindow, log }) {
			const reports: unknown[] = [];
			const store = await EngineStore.open({ dir: join(mkdtempSync(join(tmpdir(), "kitchen-store-")), "storage") });
			const t = setup(createWindow, "migration", {
				storage: store,
				capabilities: { ...storageCapabilities(store), "test.report": (args) => reports.push(args) },
			});
			try {
				// v1 reads (and on first run writes) notes, schema 1.
				const v1 = t.publish(`<!doctype html><script>
					(async () => {
						let notes = await window.engine.invoke("storage.get", { key: "notes" });
						if (!notes) { notes = ["a", "b", "c"]; await window.engine.invoke("storage.set", { key: "notes", value: notes }); }
						await window.engine.invoke("test.report", { notes, notebook: await window.engine.invoke("storage.get", { key: "notebook" }) });
						window.engine.ready();
					})();
				</script>`);
				await t.start();
				await until(t.events, is("promoted", v1));
				// v2 migrates to schema 2, then crashes before ready().
				const v2 = t.publish(`<!doctype html><script>
					(async () => {
						const notes = await window.engine.invoke("storage.get", { key: "notes" });
						await window.engine.invoke("storage.batch", { ops: [
							{ op: "set", key: "notebook", value: { schema: 2, notes } },
							{ op: "delete", key: "notes" },
						] });
						throw new Error("migration bug");
					})();
				</script>`, 2);
				await t.check();
				const failed = await until(t.events, is("failed", v2));
				await until(t.events, (e) => e.type === "ready" && e.manifest.sha256 === v1.sha256 && t.events.indexOf(e) > t.events.indexOf(failed));
				expect(store.get("notes")).toEqual(["a", "b", "c"]);
				expect(store.get("notebook")).toBe(null);
				// v1 booted again after the rollback and read the restored notes.
				expect(reports.at(-1)).toEqual({ notes: ["a", "b", "c"], notebook: null });
				log(t.events.map(describe).join(" → "));
			} finally {
				t.stop();
			}
		},
	}),

	defineTest({
		name: "AppEngine refuses a UI older than the app's data after a migration",
		category: "AppEngine",
		description: "Once a UI on data schema 2 has booted, a UI on schema 1 is refused before download (the spike's v5 then v2)",
		timeout: 30000,
		async run({ createWindow, log }) {
			const t = setup(createWindow, "schema");
			try {
				const v1 = t.publish(UIS.good("schema 2"), 2);
				const engine = await t.start();
				await until(t.events, is("promoted", v1));
				t.publish(UIS.good("schema 1"), 1);
				const result = await t.check();
				expect(result.result).toBe("refused");
				expect(result.result === "refused" && result.reason).toContain("data schema 1 is older than the 2");
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
