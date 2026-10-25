// AppEngine boot lifecycle: shows the newest UI that's known to work, swaps to
// new installs from the UI channel while the app runs, and rolls back when one
// fails. A UI only becomes a rollback target after it boots (ready() within the
// deadline) and then runs through probation without an uncaught error.
//
// Two ways to display a UI:
// - default: a BrowserView the engine owns on top of `window`, with a
//   throwaway partition and the `window.engine` preload (view-display.ts).
// - host mode (`host`): the app's own view, with its own partition, preload,
//   bridge and guards. The engine keeps the UI's files in the view's viewsRoot,
//   rewritten from the signed pack before every boot (served.ts), hands the
//   app the UI's index.html, and the app reports ready() and errors from its
//   own bridge. The app's built-in UI is the fallback instead of a safe mode.

import { mkdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import type { EngineInvokeResult } from "../../../shared/appengine-protocol";
import type { BrowserView } from "../core/BrowserView";
import type { BrowserWindow } from "../core/BrowserWindow";
import { UiChannel, type UiChannelOptions } from "./channel";
import { Capabilities, contractRefusal, type CapabilityHandler } from "./contract";
import { manifestShapeError, type UiManifest } from "./format";
import { placeFiles, pruneFiles, readVerifiedPack, type UiFiles } from "./served";
import { writeAtomic } from "./storage";
import type { ViewDisplay } from "./view-display";

// Engine-owned storage AppEngine can snapshot around a never-good UI's first
// boot, so a migration that crashes halfway can be undone.
export type EngineStorage = {
	snapshot(name: string): void;
	restore(name: string): boolean;
};

// The UI an app ships inside its bundle, for host mode.
export type BuiltInUi = {
	uiVersion: string;
	// Where it sits among published UIs: installed UIs whose seq isn't above it
	// are never booted, and the channel refuses them. Publishers use the publish
	// time in ms as seq, so the build time in ms fits.
	seq: number;
	// Its files by relative path, index.html included. Read from the app bundle,
	// which is trusted like the app's code.
	files(): UiFiles;
};

// What host mode shows. The engine has placed the UI's files under the
// viewsRoot; `index` is its index.html, which it keeps out of there so the app
// can load it its own way (with a CSP added, for example).
export type HostUi = {
	// null for the built-in UI.
	manifest: UiManifest | null;
	uiVersion: string;
	index: Uint8Array;
	// The renderer hung: replace it rather than navigate it.
	fresh: boolean;
};

export type AppEngineHost = {
	// The folder the app's view serves views:// from (its viewsRoot), and the
	// path under it the UI's files go to: "internal" for views://internal/.
	// The engine owns viewsRoot/prefix and deletes anything else in it.
	viewsRoot: string;
	prefix: string;
	builtIn: BuiltInUi;
	show(ui: HostUi): void;
	// The page's state to carry across a swap (session.takeHandoff returns it).
	prepareSwap?(toVersion: string): Promise<unknown>;
	// Whether a newly installed UI may replace the one on screen right now.
	// When it says no, the UI waits for applyUpdate() or the next start.
	canSwap?(): boolean;
};

export type AppEngineOptions = {
	// The UI channel the engine installs from. The engine adds its own accept
	// checks (bundles that failed before, data schema downgrades).
	channel: UiChannelOptions;
	// Default display: the window the UI is shown in. Its own webview stays
	// blank; the UI runs in a BrowserView on top that the engine can throw away.
	window?: BrowserWindow;
	// Default display: shown when no UI that works is installed, e.g.
	// views://safemode/index.html.
	safeModeUrl?: string;
	// Host mode: show UIs in the app's own view instead. There's no storage
	// snapshot unless `storage` is given: a UI's data is usually the app's own
	// (localStorage), so a UI that migrates it and then fails can't be undone.
	// The dataSchema guard still refuses older UIs once a newer one booted.
	host?: AppEngineHost;
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
	// The contract version this engine build implements (strict semver, e.g.
	// "1.2.0"). UIs whose manifest requiresContract it doesn't satisfy are
	// refused before download. Bump the minor version when you add
	// capabilities, the major one when you remove or change them.
	contractVersion: string;
	// Handlers for engine.invoke(cap, args) beyond the built-in ones
	// (engine.info, session.takeHandoff). Only these names can be invoked.
	capabilities?: Record<string, CapabilityHandler>;
};

export type AppEngineEvent =
	| { type: "booting"; manifest: UiManifest; reason: BootReason }
	| { type: "ready"; manifest: UiManifest }
	| { type: "promoted"; manifest: UiManifest }
	// `banned`: the UI won't be booted again. A missed deadline or a renderer
	// that went away can be the machine's fault, so those ban on the second
	// strike; an uncaught error the UI threw bans right away.
	| { type: "failed"; manifest: UiManifest; reason: string; banned: boolean }
	| { type: "safe-mode"; reason: string }
	// Host mode: the app's built-in UI is on screen.
	| { type: "built-in"; reason: string }
	// Host mode: a new UI is installed and waits for applyUpdate() or the next start.
	| { type: "waiting"; manifest: UiManifest };

type BootReason = "startup" | "update" | "rollback";

// What kind of failure: the UI's own fault, or possibly the environment's
// (a slow cold start, the OS killing a process, a frozen renderer after sleep).
type FailureKind = "crash" | "environmental";

const MAX_STRIKES = 2;
const INDEX = "index.html";

type EngineState = {
	current: UiManifest | null;
	// Rollback targets, newest first: UIs that booted and passed probation.
	good: UiManifest[];
	// sha256 -> why it failed. Never booted again.
	bad: Record<string, string>;
	// sha256 -> environmental failures so far; banned at MAX_STRIKES.
	strikes: Record<string, number>;
	// A new UI that failed environmentally once, tried again at next startup.
	retry: UiManifest | null;
	// Host mode: the newest install, not booted yet. Booted at next startup.
	next: UiManifest | null;
	// Highest data schema a UI that booted has used: older UIs would misread it.
	dataSchemaHighWater: number;
};

const emptyState = (): EngineState => ({
	current: null,
	good: [],
	bad: {},
	strikes: {},
	retry: null,
	next: null,
	dataSchemaHighWater: 0,
});

// A UI ready to boot. `url` identifies it to ready() and the error reports:
// its appdata:// URL by default, `ui:<sha256>` in host mode, where `files` are
// its verified files.
type Target = { manifest: UiManifest; url: string; files: UiFiles | null };

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
	private readonly host: AppEngineHost | null;
	private readonly listeners = new Set<(event: AppEngineEvent) => void>();
	private readonly readyTimeoutMs: number;
	private readonly probationMs: number;
	private readonly keepGood: number;
	private state: EngineState = emptyState();
	// UIs that failed in this run: skipped as rollback targets until restart.
	private readonly failedThisRun = new Set<string>();
	private stateFile = "";
	private display: ViewDisplay | null = null;
	private pending: Pending | null = null;
	private probation: Probation | null = null;
	private queued: UiManifest | null = null;
	private waiting: UiManifest | null = null;
	private handoff: unknown = null;
	private showingManifest: UiManifest | null = null;
	private showingBuiltIn = false;
	// Host mode: the files each UI placed under viewsRoot/prefix uses, by
	// sha256 ("built-in" for the app's own), since the last prune.
	private placed = new Map<string, Set<string>>();
	private started = false;
	private readonly capabilities: Capabilities;

	constructor(options: AppEngineOptions) {
		this.options = options;
		this.host = options.host ?? null;
		if (!this.host && (!options.window || !options.safeModeUrl)) {
			throw new Error("AppEngine needs either `host`, or `window` and `safeModeUrl`");
		}
		if (this.host && options.channel.keep === "files") {
			throw new Error("AppEngine in host mode keeps packs, so it can check them before every boot");
		}
		this.readyTimeoutMs = options.readyTimeoutMs ?? 5_000;
		this.probationMs = options.probationMs ?? 30_000;
		this.keepGood = options.keepGood ?? 3;
		this.capabilities = new Capabilities(
			options.contractVersion,
			{
				"engine.info": () => ({
					uiVersion: this.showingManifest?.uiVersion ?? (this.showingBuiltIn ? this.host!.builtIn.uiVersion : null),
					seq: this.showingManifest?.seq ?? (this.showingBuiltIn ? this.host!.builtIn.seq : null),
					contract: this.capabilities.contract,
				}),
				"session.takeHandoff": () => this.handoff,
			},
			options.capabilities,
		);
		const appAccept = options.channel.accept;
		this.channel = new UiChannel({
			...options.channel,
			...(this.host ? { keep: "pack" as const } : {}),
			accept: (manifest) => this.accept(manifest) ?? appAccept?.(manifest) ?? null,
		});
	}

	on(listener: (event: AppEngineEvent) => void) {
		this.listeners.add(listener);
		return () => this.listeners.delete(listener);
	}

	// The installed UI on screen, or null in safe mode or on the built-in UI.
	get showing(): UiManifest | null {
		return this.showingManifest;
	}

	// The BrowserView the UI currently runs in (replaced after a hang). Always
	// null in host mode, where the app owns the view.
	getView(): BrowserView | null {
		return this.display?.getView() ?? null;
	}

	async start() {
		if (this.started) return;
		this.started = true;
		this.stateFile = this.options.stateFile ?? (await defaultStateFile());
		this.state = parseState(readJson(this.stateFile));
		if (!this.host) {
			// Imported lazily: host mode never loads BrowserView or the page script.
			const { ViewDisplay } = await import("./view-display");
			this.display = new ViewDisplay(this, this.options.window!, this.options.safeModeUrl!, this.capabilities.pagePrelude());
		}

		const candidates = [this.state.retry, this.state.current, ...this.state.good];
		const startup = await this.pickRollbackTarget(this.host ? [this.state.next, ...candidates] : candidates);
		if (startup) {
			this.display?.mount(startup.url);
			this.boot(startup, "startup");
		} else if (this.host) {
			this.showBuiltIn("no installed UI that works is newer than the built-in one", false);
		} else {
			this.display!.mount(this.options.safeModeUrl!);
			this.emit({ type: "safe-mode", reason: "no UI that works is installed" });
		}

		this.channel.on((event) => {
			if (event.type !== "available") return;
			if (this.host) {
				this.state.next = event.install.manifest;
				this.saveState();
			}
			void this.swapTo(event.install.manifest);
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

	// ---------------------------------------------------------------- host mode

	// The page host.show() loaded booted. The app calls this from its own bridge
	// after every load it does for the engine.
	ready() {
		if (this.pending) this.onReady(this.pending.url);
	}

	// An uncaught error in the page: a failed boot before ready(), a crash
	// during probation, nothing after that.
	error(message: string) {
		if (this.pending) this.onBootError(this.pending.url, message);
		else if (this.probation) void this.onRuntimeError(this.probation.url, message);
	}

	// The page's renderer process ended on its own.
	rendererGone() {
		this.onRendererGone();
	}

	// Boots the installed UI that canSwap() held back, if any.
	applyUpdate() {
		const waiting = this.waiting;
		this.waiting = null;
		if (waiting) void this.swapTo(waiting, true);
	}

	// Refuses bundles the engine knows it can't run, before they're downloaded.
	private accept(manifest: UiManifest): string | null {
		const refusal = contractRefusal(this.capabilities.contract.version, manifest);
		if (refusal) return refusal;
		const failure = this.state.bad[manifest.sha256];
		if (failure) return `failed before: ${failure}`;
		if (manifest.dataSchema < this.state.dataSchemaHighWater) {
			return `data schema ${manifest.dataSchema} is older than the ${this.state.dataSchemaHighWater} this app's data uses`;
		}
		if (this.host && manifest.seq <= this.host.builtIn.seq) {
			return `UI ${manifest.uiVersion} is older than the one this app ships (${this.host.builtIn.uiVersion})`;
		}
		return null;
	}

	// ---------------------------------------------------------------- boot

	private boot(target: Target, reason: BootReason, options: { load?: boolean; fresh?: boolean } = {}) {
		const { manifest, url } = target;
		// First boot of a UI that never worked: snapshot storage so a failed
		// boot (a migration that crashes halfway) can be undone.
		const isNew = !this.state.good.some((good) => good.sha256 === manifest.sha256);
		const snapshot = isNew && this.options.storage ? `before-${manifest.sha256}` : null;
		if (snapshot) this.options.storage!.snapshot(snapshot);
		if (this.state.next?.sha256 === manifest.sha256) {
			this.state.next = null;
			this.saveState();
		}

		this.pending = {
			manifest,
			url,
			reason,
			snapshot,
			timer: setTimeout(
				() => this.bootFailed(`no ready() within ${this.readyTimeoutMs}ms`, true, "environmental"),
				this.readyTimeoutMs,
			),
		};
		this.emit({ type: "booting", manifest, reason });
		if (this.host) {
			this.showingBuiltIn = false;
			try {
				this.hostShow(manifest, manifest.uiVersion, target.files!, !!options.fresh);
			} catch (error) {
				this.bootFailed(`couldn't show it: ${String(error)}`, false, "environmental");
			}
		} else if (options.fresh) this.display?.replace(url);
		else if (options.load) this.display?.load(url);
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
		delete this.state.strikes[pending.manifest.sha256];
		if (this.state.retry?.sha256 === pending.manifest.sha256) this.state.retry = null;
		const known = this.state.good.some((good) => good.sha256 === pending.manifest.sha256);
		if (known) {
			this.state.good = [pending.manifest, ...this.state.good.filter((good) => good.sha256 !== pending.manifest.sha256)];
		} else {
			this.startProbation(pending);
		}
		this.saveState();
		this.emit({ type: "ready", manifest: pending.manifest });
		this.pruneServed();
		void this.prune();
		this.afterSettle();
	}

	onBootError(href: string, message: string) {
		if (this.pending && stripQuery(href) === this.pending.url) {
			this.bootFailed(`uncaught error before ready(): ${firstLine(message)}`, false, "crash");
		}
	}

	async onRuntimeError(href: string, message: string, kind: FailureKind = "crash") {
		const probation = this.probation;
		if (!probation || this.pending || stripQuery(href) !== probation.url) return;
		this.cancelProbation();
		// The page is still alive, so the user's work can come along.
		const fallback = await this.pickRollbackTarget(this.state.good);
		await this.captureHandoff(fallback?.manifest.uiVersion ?? this.host?.builtIn.uiVersion ?? null);
		// The user may have worked in the new UI: only undo their writes if the
		// fallback couldn't read the data otherwise.
		const restore = probation.manifest.dataSchema > (fallback?.manifest.dataSchema ?? 0) ? probation.snapshot : null;
		this.markFailedAndRollBack(probation.manifest, `crashed during probation: ${firstLine(message)}`, restore, false, kind);
	}

	onRendererGone() {
		if (this.pending) this.bootFailed("the renderer process ended", false, "environmental");
		else if (this.probation) void this.onRuntimeError(this.probation.url, "the renderer process ended", "environmental");
	}

	private bootFailed(reason: string, hung: boolean, kind: FailureKind) {
		const pending = this.pending;
		if (!pending) return;
		clearTimeout(pending.timer);
		this.pending = null;
		this.markFailedAndRollBack(pending.manifest, reason, pending.snapshot, hung, kind);
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

	private async markFailedAndRollBack(
		manifest: UiManifest,
		reason: string,
		snapshot: string | null,
		hung: boolean,
		kind: FailureKind,
	) {
		const sha = manifest.sha256;
		this.failedThisRun.add(sha);
		const strikes = (this.state.strikes[sha] ?? 0) + 1;
		const banned = kind === "crash" || strikes >= MAX_STRIKES;
		if (banned) {
			this.state.bad[sha] = reason;
			delete this.state.strikes[sha];
			this.state.good = this.state.good.filter((good) => good.sha256 !== sha);
			if (this.state.current?.sha256 === sha) this.state.current = null;
			if (this.state.retry?.sha256 === sha) this.state.retry = null;
			this.state.dataSchemaHighWater = Math.max(0, ...this.state.good.map((good) => good.dataSchema));
		} else {
			// A rollback target keeps its place; a new UI gets one more try at
			// the next startup.
			this.state.strikes[sha] = strikes;
			const known = this.state.good.some((good) => good.sha256 === sha) || this.state.current?.sha256 === sha;
			if (!known) this.state.retry = manifest;
		}
		this.saveState();
		if (snapshot) this.options.storage?.restore(snapshot);
		this.emit({ type: "failed", manifest, reason, banned });

		const fallback = await this.pickRollbackTarget(this.state.good);
		if (fallback) {
			// A hung renderer can't be navigated: use a fresh view.
			this.boot(fallback, "rollback", hung ? { fresh: true } : { load: true });
			return;
		}
		this.showingManifest = null;
		if (this.host) {
			this.showBuiltIn(`${manifest.uiVersion} failed and no other installed UI works`, hung);
		} else {
			if (hung) this.display?.replace(this.options.safeModeUrl!);
			else this.display?.load(this.options.safeModeUrl!);
			this.emit({ type: "safe-mode", reason: `${manifest.uiVersion} failed and no UI that works is installed` });
		}
		this.afterSettle();
	}

	// ---------------------------------------------------------------- swaps

	private async swapTo(manifest: UiManifest, force = false) {
		// One boot at a time: a publish that lands mid-boot waits for it.
		if (this.pending) {
			this.queued = manifest;
			return;
		}
		if (this.host && !force && this.host.canSwap?.() === false) {
			this.waiting = manifest;
			this.emit({ type: "waiting", manifest });
			return;
		}
		const target = await this.pickRollbackTarget([manifest]);
		if (!target) return;
		// A newer UI supersedes one still on probation, which is never promoted.
		this.cancelProbation();
		await this.captureHandoff(manifest.uiVersion);
		this.boot(target, "update", { load: true });
	}

	private afterSettle() {
		const queued = this.queued;
		this.queued = null;
		if (queued) void this.swapTo(queued);
	}

	private async captureHandoff(toVersion: string | null) {
		const prepare = this.host ? this.host.prepareSwap?.bind(this.host) : this.display?.prepareSwap.bind(this.display);
		if (!prepare || toVersion === null) {
			this.handoff = null;
			return;
		}
		try {
			const result = await Promise.race([
				prepare(toVersion),
				new Promise<undefined>((resolve) => setTimeout(() => resolve(undefined), 1_000)),
			]);
			this.handoff = result ?? null;
		} catch {
			this.handoff = null;
		}
	}

	// ---------------------------------------------------------------- host display

	// Puts the UI's files under the viewsRoot, then has the app show it. What
	// the UI it replaces uses stays until pruneServed().
	private hostShow(manifest: UiManifest | null, uiVersion: string, files: UiFiles, fresh: boolean) {
		const host = this.host!;
		const index = files.get(INDEX)!;
		const assets = new Map(files);
		assets.delete(INDEX);
		placeFiles(join(host.viewsRoot, host.prefix), assets);
		this.placed.set(manifest?.sha256 ?? "built-in", new Set(assets.keys()));
		host.show({ manifest, uiVersion, index, fresh });
	}

	private showBuiltIn(reason: string, fresh: boolean) {
		const host = this.host!;
		this.showingManifest = null;
		try {
			const files = host.builtIn.files();
			if (!files.has(INDEX)) throw new Error(`it has no ${INDEX}`);
			this.hostShow(null, host.builtIn.uiVersion, files, fresh);
		} catch (error) {
			this.emit({ type: "safe-mode", reason: `the built-in UI can't be shown: ${String(error)}` });
			return;
		}
		this.showingBuiltIn = true;
		this.pruneServed();
		this.emit({ type: "built-in", reason });
	}

	// Drops the files only UIs no longer on screen used.
	private pruneServed() {
		const host = this.host;
		const key = this.showingBuiltIn ? "built-in" : this.showingManifest?.sha256;
		const keep = key ? this.placed.get(key) : undefined;
		if (!host || !key || !keep) return;
		this.placed = new Map([[key, keep]]);
		try {
			pruneFiles(join(host.viewsRoot, host.prefix), keep);
		} catch (error) {
			console.error("AppEngine couldn't prune its served files:", error);
		}
	}

	// ---------------------------------------------------------------- invoke

	// The contract this engine implements, as UIs see it.
	get contract() {
		return this.capabilities.contract;
	}

	invoke(cap: string, args: unknown): Promise<EngineInvokeResult> {
		return this.capabilities.invoke(cap, args);
	}

	// ---------------------------------------------------------------- state

	private async pickRollbackTarget(candidates: (UiManifest | null)[]): Promise<Target | null> {
		for (const manifest of candidates) {
			if (!manifest || this.state.bad[manifest.sha256] || this.failedThisRun.has(manifest.sha256)) continue;
			const install = await this.channel.installed(manifest);
			if (!this.host) {
				if (install?.url) return { manifest, url: install.url, files: null };
				continue;
			}
			if (!install?.pack || manifest.seq <= this.host.builtIn.seq) continue;
			const files = readVerifiedPack(install.pack, manifest, this.options.channel.trustedKeys);
			if (files.ok && files.value.has(INDEX)) return { manifest, url: `ui:${manifest.sha256}`, files: files.value };
			// Changed on disk since it was installed, or damaged: not the UI's
			// fault, but nothing to boot either until the channel brings a new one.
			this.failedThisRun.add(manifest.sha256);
			const problem = files.ok ? `it has no ${INDEX}` : files.reason;
			this.emit({ type: "failed", manifest, reason: `the installed pack doesn't check out: ${problem}`, banned: false });
		}
		return null;
	}

	private async prune() {
		const keep = [
			...this.state.good,
			this.state.current,
			this.state.retry,
			this.state.next,
			this.pending?.manifest,
			this.probation?.manifest,
		]
			.filter((manifest): manifest is UiManifest => !!manifest)
			.map((manifest) => manifest.sha256);
		await this.channel.prune(keep);
	}

	private saveState() {
		mkdirSync(dirname(this.stateFile), { recursive: true });
		writeAtomic(this.stateFile, JSON.stringify(this.state));
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

const firstLine = (message: string) => message.split("\n")[0]!;

function readJson(path: string): unknown {
	try {
		return JSON.parse(readFileSync(path, "utf8"));
	} catch {
		return null;
	}
}

// engine.json as read from disk, field by field: anything damaged falls back
// to its default rather than breaking startup.
function parseState(raw: unknown): EngineState {
	const state = emptyState();
	if (!raw || typeof raw !== "object" || Array.isArray(raw)) return state;
	const value = raw as Record<string, unknown>;
	const manifest = (entry: unknown) => (manifestShapeError(entry) === null ? (entry as UiManifest) : null);
	const record = <T>(entry: unknown, valid: (item: unknown) => item is T): Record<string, T> => {
		const out: Record<string, T> = {};
		if (!entry || typeof entry !== "object" || Array.isArray(entry)) return out;
		for (const [key, item] of Object.entries(entry)) if (valid(item)) out[key] = item;
		return out;
	};
	state.current = manifest(value["current"]);
	state.retry = manifest(value["retry"]);
	state.next = manifest(value["next"]);
	if (Array.isArray(value["good"])) {
		state.good = value["good"].map(manifest).filter((entry): entry is UiManifest => entry !== null);
	}
	state.bad = record(value["bad"], (item): item is string => typeof item === "string");
	state.strikes = record(value["strikes"], (item): item is number => Number.isSafeInteger(item) && (item as number) > 0);
	const highWater = value["dataSchemaHighWater"];
	if (Number.isSafeInteger(highWater) && (highWater as number) >= 0) state.dataSchemaHighWater = highWater as number;
	return state;
}

async function defaultStateFile() {
	// Imported lazily: Utils needs the app's runtime.
	const { paths } = await import("../core/Utils");
	return join(paths.userData, "appengine", "engine.json");
}
