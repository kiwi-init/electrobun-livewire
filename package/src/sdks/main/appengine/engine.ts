// AppEngine boot lifecycle: shows the newest UI that's known to work, swaps to
// new installs from the UI channel while the app runs, and rolls back when one
// fails. A UI only becomes a rollback target after it boots (ready() within the
// deadline) and then runs through probation without an uncaught error.

import { randomBytes } from "node:crypto";
import { mkdirSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { appEnginePageScript } from "../../../preload/.generated/compiled";
import type { AppEngineRPC, EngineInvokeResult } from "../../../shared/appengine-protocol";
import { BrowserView } from "../core/BrowserView";
import type { BrowserWindow } from "../core/BrowserWindow";
import { UiChannel, type UiChannelOptions, type UiInstall } from "./channel";
import type { UiManifest } from "./format";

// Engine-owned storage AppEngine can snapshot around a never-good UI's first
// boot, so a migration that crashes halfway can be undone.
export type EngineStorage = {
	snapshot(name: string): void;
	restore(name: string): boolean;
};

export type AppEngineOptions = {
	// The UI channel the engine installs from. The engine adds its own accept
	// checks (bundles that failed before, data schema downgrades).
	channel: UiChannelOptions;
	// The window the UI is shown in. Its own webview stays blank; the UI runs
	// in a BrowserView on top that the engine can throw away.
	window: BrowserWindow;
	// Shown when no UI that works is installed, e.g. views://safemode/index.html.
	safeModeUrl: string;
	// Time a booting UI gets to call engine.ready(). Default 5 s.
	readyTimeoutMs?: number;
	// Error-free time after ready() before a new UI becomes a rollback target.
	// Default 30 s.
	probationMs?: number;
	// Rollback targets kept installed. Default 3.
	keepGood?: number;
	// The engine's own state. Defaults to userData/appengine/engine.json.
	stateFile?: string;
	storage?: EngineStorage;
	// Handlers for engine.invoke(cap, args) beyond the built-in ones.
	capabilities?: Record<string, (args: unknown) => unknown | Promise<unknown>>;
};

export type AppEngineEvent =
	| { type: "booting"; manifest: UiManifest; reason: BootReason }
	| { type: "ready"; manifest: UiManifest }
	| { type: "promoted"; manifest: UiManifest }
	| { type: "failed"; manifest: UiManifest; reason: string }
	| { type: "safe-mode"; reason: string };

type BootReason = "startup" | "update" | "rollback";

type EngineState = {
	current: UiManifest | null;
	// Rollback targets, newest first: UIs that booted and passed probation.
	good: UiManifest[];
	// sha256 -> why it failed. Never booted again.
	bad: Record<string, string>;
	// Highest data schema a UI that booted has used: older UIs would misread it.
	dataSchemaHighWater: number;
};

type Pending = {
	manifest: UiManifest;
	url: string;
	reason: BootReason;
	snapshot: string | null;
	timer: ReturnType<typeof setTimeout>;
};

type Probation = {
	manifest: UiManifest;
	url: string;
	snapshot: string | null;
	timer: ReturnType<typeof setTimeout>;
};

const stripQuery = (url: string) => url.split(/[?#]/)[0]!;

export class AppEngine {
	readonly channel: UiChannel;
	private readonly options: AppEngineOptions;
	private readonly listeners = new Set<(event: AppEngineEvent) => void>();
	private readonly readyTimeoutMs: number;
	private readonly probationMs: number;
	private readonly keepGood: number;
	private state: EngineState = { current: null, good: [], bad: {}, dataSchemaHighWater: 0 };
	private stateFile = "";
	private view: BrowserView | null = null;
	private rpc: ReturnType<typeof makeRpc> | null = null;
	private generation = 0;
	private pending: Pending | null = null;
	private probation: Probation | null = null;
	private queued: UiInstall | null = null;
	private handoff: unknown = null;
	private showingManifest: UiManifest | null = null;
	private started = false;

	constructor(options: AppEngineOptions) {
		this.options = options;
		this.readyTimeoutMs = options.readyTimeoutMs ?? 5_000;
		this.probationMs = options.probationMs ?? 30_000;
		this.keepGood = options.keepGood ?? 3;
		const appAccept = options.channel.accept;
		this.channel = new UiChannel({
			...options.channel,
			accept: (manifest) => this.accept(manifest) ?? appAccept?.(manifest) ?? null,
		});
	}

	on(listener: (event: AppEngineEvent) => void) {
		this.listeners.add(listener);
		return () => this.listeners.delete(listener);
	}

	// The UI on screen, or null in safe mode.
	get showing(): UiManifest | null {
		return this.showingManifest;
	}

	// The BrowserView the UI currently runs in (replaced after a hang).
	getView(): BrowserView | null {
		return this.view;
	}

	async start() {
		if (this.started) return;
		this.started = true;
		this.stateFile = this.options.stateFile ?? (await defaultStateFile());
		this.state = { ...this.state, ...readJson<Partial<EngineState>>(this.stateFile) };

		const startup = await this.pickRollbackTarget([this.state.current, ...this.state.good]);
		if (startup) {
			this.mountView(startup.url);
			this.boot(startup.install.manifest, startup.url, "startup");
		} else {
			this.mountView(this.options.safeModeUrl);
			this.emit({ type: "safe-mode", reason: "no UI that works is installed" });
		}

		this.channel.on((event) => {
			if (event.type === "available") this.swapTo(event.install);
		});
		this.channel.start();
	}

	stop() {
		this.channel.stop();
		if (this.pending) clearTimeout(this.pending.timer);
		if (this.probation) clearTimeout(this.probation.timer);
		this.pending = null;
		this.probation = null;
	}

	// Refuses bundles the engine knows it can't run, before they're downloaded.
	private accept(manifest: UiManifest): string | null {
		const failure = this.state.bad[manifest.sha256];
		if (failure) return `failed before: ${failure}`;
		if (manifest.dataSchema < this.state.dataSchemaHighWater) {
			return `data schema ${manifest.dataSchema} is older than the ${this.state.dataSchemaHighWater} this app's data uses`;
		}
		return null;
	}

	// ---------------------------------------------------------------- views

	private mountView(url: string) {
		const window = this.options.window;
		const frame = window.getFrame();
		const rpc = makeRpc(this);
		this.view = new BrowserView({
			windowId: window.id,
			url,
			// Ephemeral: nothing the page stores outlives the view, so UI data
			// lives in engine storage, which the engine can snapshot and guard.
			partition: `appengine-${++this.generation}`,
			preload: appEnginePageScript,
			rpc,
			allowedProtocols: { views: true, appData: true },
			navigationRules: JSON.stringify(["^*", "appdata://ui/*", `${stripQuery(this.options.safeModeUrl)}*`]),
			frame: { x: 0, y: 0, width: frame.width, height: frame.height },
			autoResize: true,
		});
		this.rpc = rpc;
		const view = this.view;
		view.on("render-process-gone", () => {
			if (view !== this.view) return;
			// Wake recovery killed a frozen process and reloads the same UI, which
			// boots again: not the UI's failure.
			if (view.rendererExitExpected) {
				view.rendererExitExpected = null;
				return;
			}
			this.onRendererGone();
		});
	}

	// A UI stuck in a loop can't be navigated away from: kill its renderer and
	// show the next URL in a fresh view.
	private replaceView(url: string) {
		const old = this.view;
		old?.terminateRenderer();
		this.mountView(url);
		old?.remove();
	}

	// ---------------------------------------------------------------- boot

	private boot(manifest: UiManifest, url: string, reason: BootReason, options: { load?: boolean; fresh?: boolean } = {}) {
		// First boot of a UI that never worked: snapshot storage so a failed
		// boot (a migration that crashes halfway) can be undone.
		const isNew = !this.state.good.some((good) => good.sha256 === manifest.sha256);
		const snapshot = isNew && this.options.storage ? `before-${manifest.sha256}` : null;
		if (snapshot) this.options.storage!.snapshot(snapshot);

		this.pending = {
			manifest,
			url,
			reason,
			snapshot,
			timer: setTimeout(() => this.bootFailed(`no ready() within ${this.readyTimeoutMs}ms`, true), this.readyTimeoutMs),
		};
		this.emit({ type: "booting", manifest, reason });
		if (options.fresh) this.replaceView(url);
		else if (options.load) this.view?.loadURL(url);
	}

	onReady(href: string) {
		const pending = this.pending;
		if (!pending || stripQuery(href) !== pending.url) return;
		clearTimeout(pending.timer);
		this.pending = null;
		this.showingManifest = pending.manifest;
		this.state.current = pending.manifest;
		// The UI has migrated its data by now: older UIs are refused from here on.
		this.state.dataSchemaHighWater = Math.max(this.state.dataSchemaHighWater, pending.manifest.dataSchema);
		this.handoff = null;
		const known = this.state.good.some((good) => good.sha256 === pending.manifest.sha256);
		if (known) {
			this.state.good = [pending.manifest, ...this.state.good.filter((good) => good.sha256 !== pending.manifest.sha256)];
		} else {
			this.startProbation(pending);
		}
		this.saveState();
		this.emit({ type: "ready", manifest: pending.manifest });
		void this.prune();
		this.afterSettle();
	}

	onBootError(href: string, message: string) {
		if (this.pending && stripQuery(href) === this.pending.url) {
			this.bootFailed(`uncaught error before ready(): ${firstLine(message)}`, false);
		}
	}

	async onRuntimeError(href: string, message: string) {
		const probation = this.probation;
		if (!probation || this.pending || stripQuery(href) !== probation.url) return;
		this.cancelProbation();
		// The page is still alive, so the user's work can come along.
		const fallback = await this.pickRollbackTarget(this.state.good);
		if (fallback) await this.captureHandoff(fallback.install.manifest.uiVersion);
		// The user may have worked in the new UI: only undo their writes if the
		// fallback couldn't read the data otherwise.
		const restore = probation.manifest.dataSchema > (fallback?.install.manifest.dataSchema ?? 0) ? probation.snapshot : null;
		this.markBadAndRollBack(probation.manifest, `crashed during probation: ${firstLine(message)}`, restore, false);
	}

	private onRendererGone() {
		if (this.pending) this.bootFailed("the renderer process ended", false);
		else if (this.probation) void this.onRuntimeError(this.probation.url, "the renderer process ended");
	}

	private bootFailed(reason: string, hung: boolean) {
		const pending = this.pending;
		if (!pending) return;
		clearTimeout(pending.timer);
		this.pending = null;
		this.markBadAndRollBack(pending.manifest, reason, pending.snapshot, hung);
	}

	private startProbation(pending: Pending) {
		const { manifest, url, snapshot } = pending;
		this.probation = {
			manifest,
			url,
			snapshot,
			timer: setTimeout(() => {
				this.probation = null;
				this.state.good = [manifest, ...this.state.good.filter((good) => good.sha256 !== manifest.sha256)].slice(0, this.keepGood);
				this.saveState();
				this.emit({ type: "promoted", manifest });
				void this.prune();
			}, this.probationMs),
		};
	}

	private cancelProbation() {
		if (this.probation) clearTimeout(this.probation.timer);
		this.probation = null;
	}

	private async markBadAndRollBack(manifest: UiManifest, reason: string, snapshot: string | null, hung: boolean) {
		this.state.bad[manifest.sha256] = reason;
		this.state.good = this.state.good.filter((good) => good.sha256 !== manifest.sha256);
		if (this.state.current?.sha256 === manifest.sha256) this.state.current = null;
		this.state.dataSchemaHighWater = Math.max(0, ...this.state.good.map((good) => good.dataSchema));
		this.saveState();
		if (snapshot) this.options.storage?.restore(snapshot);
		this.emit({ type: "failed", manifest, reason });

		const fallback = await this.pickRollbackTarget(this.state.good);
		if (fallback) {
			// A hung renderer can't be navigated: use a fresh view.
			this.boot(fallback.install.manifest, fallback.url, "rollback", hung ? { fresh: true } : { load: true });
			return;
		}
		this.showingManifest = null;
		if (hung) this.replaceView(this.options.safeModeUrl);
		else this.view?.loadURL(this.options.safeModeUrl);
		this.emit({ type: "safe-mode", reason: `${manifest.uiVersion} failed and no UI that works is installed` });
		this.afterSettle();
	}

	// ---------------------------------------------------------------- swaps

	private async swapTo(install: UiInstall) {
		if (!install.url) return;
		// One boot at a time: a publish that lands mid-boot waits for it.
		if (this.pending) {
			this.queued = install;
			return;
		}
		// A newer UI supersedes one still on probation, which is never promoted.
		this.cancelProbation();
		await this.captureHandoff(install.manifest.uiVersion);
		this.boot(install.manifest, install.url, "update", { load: true });
	}

	private afterSettle() {
		const queued = this.queued;
		this.queued = null;
		if (queued) void this.swapTo(queued);
	}

	private async captureHandoff(toVersion: string) {
		try {
			const result = await Promise.race([
				this.rpc?.request.prepareSwap({ toVersion }),
				new Promise<undefined>((resolve) => setTimeout(() => resolve(undefined), 1_000)),
			]);
			this.handoff = result?.state ?? null;
		} catch {
			this.handoff = null;
		}
	}

	// ---------------------------------------------------------------- invoke

	async invoke(cap: string, args: unknown): Promise<EngineInvokeResult> {
		try {
			if (cap === "engine.info") {
				return { ok: true, value: { uiVersion: this.showingManifest?.uiVersion ?? null, seq: this.showingManifest?.seq ?? null } };
			}
			if (cap === "session.takeHandoff") return { ok: true, value: this.handoff };
			const handler = this.options.capabilities?.[cap];
			if (!handler) return { ok: false, code: "E_NO_CAPABILITY", message: `no capability "${cap}"` };
			return { ok: true, value: await handler(args) };
		} catch (error) {
			return { ok: false, code: "E_FAILED", message: error instanceof Error ? error.message : String(error) };
		}
	}

	// ---------------------------------------------------------------- state

	private async pickRollbackTarget(candidates: (UiManifest | null)[]) {
		for (const manifest of candidates) {
			if (!manifest || this.state.bad[manifest.sha256]) continue;
			const install = await this.channel.installed(manifest);
			if (install?.url) return { install, url: install.url };
		}
		return null;
	}

	private async prune() {
		const keep = [...this.state.good, this.state.current, this.pending?.manifest, this.probation?.manifest]
			.filter((manifest): manifest is UiManifest => !!manifest)
			.map((manifest) => manifest.sha256);
		await this.channel.prune(keep);
	}

	private saveState() {
		mkdirSync(dirname(this.stateFile), { recursive: true });
		const tmp = `${this.stateFile}.${randomBytes(4).toString("hex")}.tmp`;
		writeFileSync(tmp, JSON.stringify(this.state));
		renameSync(tmp, this.stateFile);
	}

	private emit(event: AppEngineEvent) {
		for (const listener of this.listeners) {
			try {
				listener(event);
			} catch (error) {
				console.error("AppEngine listener failed:", error);
			}
		}
	}
}

function makeRpc(engine: AppEngine) {
	return BrowserView.defineRPC<AppEngineRPC>({
		maxRequestTime: 10_000,
		handlers: {
			requests: {
				invoke: ({ cap, args }) => engine.invoke(cap, args),
			},
			messages: {
				ready: ({ href }) => engine.onReady(href),
				bootError: ({ href, message }) => engine.onBootError(href, message),
				runtimeError: ({ href, message }) => void engine.onRuntimeError(href, message),
			},
		},
	});
}

const firstLine = (message: string) => message.split("\n")[0]!;

function readJson<T>(path: string): T | Record<string, never> {
	try {
		return JSON.parse(readFileSync(path, "utf8")) as T;
	} catch {
		return {};
	}
}

async function defaultStateFile() {
	// Imported lazily: Utils needs the app's runtime.
	const { paths } = await import("../core/Utils");
	return join(paths.userData, "appengine", "engine.json");
}
