import { afterEach, describe, expect, test } from "bun:test";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { UiChannel, type UiChannelEvent } from "./channel";
import { buildUipack, generateSigningKey, readUipack, signManifest, uipackFileName, type UiManifest } from "./format";

const key = generateSigningKey();
const trustedKeys = { [key.keyId]: key.publicKey };

// A static UI host: manifest.json, bundles/*, and an SSE stream at /events.
function startHost() {
	let manifest: UiManifest | null = null;
	const bundles = new Map<string, Uint8Array>();
	const streams = new Set<ReadableStreamDefaultController<Uint8Array>>();
	const endless = new Set<string>();
	const server = Bun.serve({
		hostname: "127.0.0.1",
		port: 0,
		idleTimeout: 0,
		fetch(request) {
			const { pathname } = new URL(request.url);
			if (pathname === "/manifest.json") {
				return manifest ? Response.json(manifest) : new Response("no manifest", { status: 404 });
			}
			if (pathname === "/events") {
				const stream = new ReadableStream<Uint8Array>({
					start: (controller) => void streams.add(controller),
					cancel: () => undefined,
				});
				return new Response(stream, { headers: { "content-type": "text/event-stream" } });
			}
			const name = pathname.slice(1);
			if (endless.has(name)) {
				const chunk = new Uint8Array(64 * 1024);
				return new Response(new ReadableStream({ pull: (controller) => controller.enqueue(chunk) }));
			}
			const bundle = bundles.get(name);
			return bundle ? new Response(new Blob([bundle as Uint8Array<ArrayBuffer>])) : new Response("not found", { status: 404 });
		},
	});
	const base = `http://127.0.0.1:${server.port}/`;
	return {
		manifestUrl: `${base}manifest.json`,
		eventsUrl: `${base}events`,
		// Publishes a UI and returns its manifest. `tamper` corrupts the bundle bytes;
		// `endless` serves a bundle that never stops streaming; `bundleUrl`
		// overrides where it points.
		publish(
			seq: number,
			options: {
				signer?: typeof key;
				tamper?: boolean;
				endless?: boolean;
				bundleUrl?: string;
				expiresAt?: number;
				uiVersion?: string;
			} = {},
		) {
			const uiVersion = options.uiVersion ?? `1.0.${seq}`;
			const files = new Map([["index.html", new TextEncoder().encode(`<h1>${uiVersion}</h1>`)]]);
			const { bytes, sha256 } = buildUipack(uiVersion, files);
			const name = `bundles/${uipackFileName(uiVersion, sha256)}`;
			bundles.set(name, options.tamper ? new TextEncoder().encode("tampered") : bytes);
			if (options.endless) endless.add(name);
			const signer = options.signer ?? key;
			manifest = signManifest(
				{
					format: "livewire-manifest/1",
					keyId: signer.keyId,
					seq,
					uiVersion,
					bundleUrl: options.bundleUrl ?? name,
					sha256,
					requiresContract: "^1.0.0",
					dataSchema: 1,
					publishedAt: Date.now(),
					...(options.expiresAt !== undefined ? { expiresAt: options.expiresAt } : {}),
				},
				signer.privateKeyPem,
			);
			return manifest;
		},
		fixBundle(m: UiManifest) {
			const files = new Map([["index.html", new TextEncoder().encode(`<h1>${m.uiVersion}</h1>`)]]);
			bundles.set(m.bundleUrl, buildUipack(m.uiVersion, files).bytes);
		},
		push() {
			for (const controller of streams) controller.enqueue(new TextEncoder().encode("event: manifest\ndata: {}\n\n"));
		},
		stop() {
			for (const controller of streams) {
				try {
					controller.close();
				} catch {}
			}
			server.stop(true);
		},
	};
}

const cleanups: (() => void)[] = [];
afterEach(() => {
	while (cleanups.length) cleanups.pop()!();
});

function setup(
	options: {
		accept?: (m: UiManifest) => string | null;
		now?: () => number;
		maxBundleBytes?: number;
		fetch?: typeof fetch;
		keep?: "files" | "pack";
	} = {},
) {
	const host = startHost();
	const appdataRoot = mkdtempSync(join(tmpdir(), "uichannel-appdata-"));
	const stateDir = mkdtempSync(join(tmpdir(), "uichannel-state-"));
	const events: UiChannelEvent[] = [];
	const make = () => {
		const channel = new UiChannel({
			manifestUrl: host.manifestUrl,
			eventsUrl: host.eventsUrl,
			trustedKeys,
			appdataRoot,
			root: join(appdataRoot, "ui"),
			stateFile: join(stateDir, "channel.json"),
			...options,
		});
		channel.on((event) => events.push(event));
		cleanups.push(() => channel.stop());
		return channel;
	};
	cleanups.push(() => {
		host.stop();
		rmSync(appdataRoot, { recursive: true, force: true });
		rmSync(stateDir, { recursive: true, force: true });
	});
	return { host, appdataRoot, stateDir, events, make };
}

describe("UiChannel", () => {
	test("installs a signed bundle, then reports up to date", async () => {
		const { host, appdataRoot, stateDir, events, make } = setup();
		const manifest = host.publish(1);
		const channel = make();

		const first = await channel.check();
		expect(first.result).toBe("available");
		if (first.result !== "available") return;
		expect(first.install.dir).toBe(join(appdataRoot, "ui", manifest.sha256));
		expect(first.install.url).toBe(`appdata://ui/${manifest.sha256}/index.html`);
		expect(readFileSync(join(first.install.dir!, "index.html"), "utf8")).toBe("<h1>1.0.1</h1>");
		expect(JSON.parse(readFileSync(join(stateDir, "channel.json"), "utf8"))).toEqual({ seenSeq: 1 });
		expect(events.map((e) => e.type)).toEqual(["status", "available"]);

		expect((await channel.check()).result).toBe("up-to-date");
		// A new instance (an app restart) keeps the seq it accepted.
		expect((await make().check()).result).toBe("up-to-date");
	});

	test("keep: pack keeps the verified .uipack instead of unpacking it, and prunes it like an install", async () => {
		const { host, appdataRoot, make } = setup({ keep: "pack" });
		const manifest = host.publish(1);
		const channel = make();
		const result = await channel.check();
		if (result.result !== "available") throw new Error(`expected available, got ${result.result}`);
		const pack = join(appdataRoot, "ui", `${manifest.sha256}.uipack`);
		expect(result.install).toEqual({ manifest, dir: null, pack, url: null });
		expect(readdirSync(join(appdataRoot, "ui"))).toEqual([`${manifest.sha256}.uipack`]);
		expect(readUipack(new Uint8Array(readFileSync(pack)), manifest).ok).toBe(true);
		expect(await channel.installed(manifest)).toEqual(result.install);
		await channel.prune([]);
		expect(existsSync(pack)).toBe(false);
		expect(await channel.installed(manifest)).toBeNull();
	});

	test("refuses an older seq (replay)", async () => {
		const { host, make } = setup();
		host.publish(2);
		const channel = make();
		expect((await channel.check()).result).toBe("available");
		host.publish(1);
		expect(await channel.check()).toEqual({ result: "refused", reason: "replay" });
	});

	test("refuses a manifest from an untrusted key without installing or advancing", async () => {
		const { host, appdataRoot, stateDir, make } = setup();
		const manifest = host.publish(1, { signer: generateSigningKey() });
		const result = await make().check();
		expect(result.result).toBe("refused");
		if (result.result === "refused") expect(result.reason).toContain("isn't trusted");
		expect(existsSync(join(appdataRoot, "ui", manifest.sha256))).toBe(false);
		expect(existsSync(join(stateDir, "channel.json"))).toBe(false);
	});

	test("refuses a bundle whose bytes don't match, and retries the same publish later", async () => {
		const { host, appdataRoot, make } = setup();
		const manifest = host.publish(1, { tamper: true });
		const channel = make();
		const refused = await channel.check();
		expect(refused.result).toBe("refused");
		if (refused.result === "refused") expect(refused.reason).toContain("hash");
		expect(existsSync(join(appdataRoot, "ui", manifest.sha256))).toBe(false);

		host.fixBundle(manifest);
		expect((await channel.check()).result).toBe("available");
	});

	test("refuses an expired manifest", async () => {
		const { host, make } = setup({ now: () => 5_000 });
		host.publish(1, { expiresAt: 4_000 });
		expect(await make().check()).toEqual({ result: "refused", reason: "manifest has expired" });
	});

	test("lets the caller refuse a manifest before download", async () => {
		const { host, make } = setup({ accept: (m) => (m.requiresContract === "^1.0.0" ? "needs a newer engine" : null) });
		host.publish(1);
		expect(await make().check()).toEqual({ result: "refused", reason: "needs a newer engine" });
	});

	test("reports offline when the manifest can't be fetched", async () => {
		const { events, make } = setup();
		const result = await make().check();
		expect(result.result).toBe("offline");
		expect(events).toEqual([{ type: "status", online: false }]);
	});

	test("refuses a bundle that keeps streaming past the size cap, writing nothing", async () => {
		const { host, appdataRoot, stateDir, make } = setup({ maxBundleBytes: 1024 * 1024 });
		host.publish(1, { endless: true });
		const result = await make().check();
		expect(result.result).toBe("refused");
		if (result.result === "refused") expect(result.reason).toContain("over the 1048576-byte limit");
		// Refused before anything was written: not even the install root exists.
		expect(existsSync(join(appdataRoot, "ui"))).toBe(false);
		expect(existsSync(join(stateDir, "channel.json"))).toBe(false);
	});

	test("refuses a bundle whose declared length is over the cap before reading it", async () => {
		// Bun's server rewrites content-length, so the bundle response is stubbed.
		const stub: typeof fetch = Object.assign(
			async (input: RequestInfo | URL, init?: RequestInit) =>
				String(input).includes("/bundles/")
					? new Response("x", { headers: { "content-length": "5000" } })
					: fetch(input, init),
			{ preconnect: fetch.preconnect },
		);
		const { host, make } = setup({ maxBundleBytes: 1000, fetch: stub });
		host.publish(1);
		const result = await make().check();
		expect(result.result === "refused" && result.reason).toContain("5000 bytes, over the 1000-byte limit");
	});

	test("refuses a bundle URL that isn't http(s)", async () => {
		const { host, make } = setup();
		host.publish(1, { bundleUrl: "file:///etc/hosts" });
		const result = await make().check();
		expect(result.result === "refused" && result.reason).toContain("must be http(s)");
	});

	test("sweeps staging dirs left by an interrupted install", async () => {
		const { host, appdataRoot, make } = setup();
		mkdirSync(join(appdataRoot, "ui", ".staging-dead-beef"), { recursive: true });
		host.publish(1);
		expect((await make().check()).result).toBe("available");
		expect(existsSync(join(appdataRoot, "ui", ".staging-dead-beef"))).toBe(false);
	});

	test("a push checks right away", async () => {
		const { host, events, make } = setup();
		host.publish(1);
		const channel = make();
		channel.start();
		await waitFor(() => events.some((e) => e.type === "available"));

		host.publish(2);
		host.push();
		await waitFor(() => events.filter((e) => e.type === "available").length === 2);
		const last = events.filter((e) => e.type === "available").at(-1);
		expect(last?.type === "available" && last.install.manifest.seq).toBe(2);
	});
});

async function waitFor(condition: () => boolean, timeoutMs = 5_000) {
	const deadline = Date.now() + timeoutMs;
	while (!condition()) {
		if (Date.now() > deadline) throw new Error("timed out");
		await new Promise((resolve) => setTimeout(resolve, 20));
	}
}
