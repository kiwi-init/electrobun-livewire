// AppEngine UI channel: finds the newest signed UI bundle for this app, checks
// it, and installs it where views can load it (appdata://ui/<sha256>/).
//
// The channel only fetches, verifies and installs. Deciding when to boot an
// install, and rolling back, is the boot lifecycle's job.

import { randomBytes } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, readdirSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join, relative, sep } from "node:path";
import { readUipack, type TrustedKeys, type UiManifest, verifyManifest } from "./format";

export type UiChannelOptions = {
	// Where the signed manifest is published.
	manifestUrl: string;
	// Trusted publishing keys: keyId -> raw 32-byte Ed25519 public key, base64url.
	trustedKeys: TrustedKeys;
	// Server-Sent Events endpoint that announces new publishes. Without it the
	// channel only polls.
	eventsUrl?: string | null;
	// Poll even while push is connected, as a safety net. Default 5 minutes.
	pollIntervalMs?: number;
	// Install root. Defaults to `ui` under Utils.paths.appdataProtocolRoot, so
	// installs load as appdata://ui/<sha256>/..., or to
	// userData/appengine/packs when the channel keeps packs.
	root?: string;
	// The folder appdata:// serves, used to build install URLs. Defaults to
	// Utils.paths.appdataProtocolRoot.
	appdataRoot?: string;
	// The channel's own state (last accepted seq). Defaults to
	// userData/appengine/channel.json, outside what appdata:// serves.
	stateFile?: string;
	// Largest bundle the channel downloads. Default 256 MB.
	maxBundleBytes?: number;
	// What an install keeps on disk: the bundle's files, unpacked where views
	// load them (default), or the verified .uipack itself, for an engine that
	// checks it against its signed manifest again before every boot.
	keep?: "files" | "pack";
	// Extra checks run after the signature and anti-replay checks, before
	// download (contract range, data schema, known-bad bundles). Return a
	// reason to refuse the manifest, or null to accept it.
	accept?: (manifest: UiManifest) => string | null;
	// For tests.
	fetch?: typeof fetch;
	now?: () => number;
};

export type UiInstall = {
	manifest: UiManifest;
	// Directory the bundle's files were installed into, or null when the
	// channel keeps packs.
	dir: string | null;
	// The kept .uipack, or null when the channel keeps files.
	pack: string | null;
	// appdata:// URL of the bundle's index.html, or null when `root` isn't
	// under the appdata:// root or the channel keeps packs.
	url: string | null;
};

export type UiChannelEvent =
	| { type: "available"; install: UiInstall }
	| { type: "refused"; reason: string; manifest?: UiManifest }
	| { type: "status"; online: boolean };

export type UiCheckResult =
	| { result: "available"; install: UiInstall }
	| { result: "up-to-date" }
	| { result: "offline"; error: string }
	| { result: "refused"; reason: string };

type ChannelState = { seenSeq: number };

const DEFAULT_POLL_INTERVAL_MS = 5 * 60_000;
const MANIFEST_MAX_BYTES = 64 * 1024;
const DEFAULT_MAX_BUNDLE_BYTES = 256 * 1024 * 1024;
const MANIFEST_TIMEOUT_MS = 15_000;
// Downloads may be slow but not stall: abort when no bytes arrive for this
// long, and give up on a bundle that still isn't done after the deadline.
const STALL_TIMEOUT_MS = 30_000;
const BUNDLE_DEADLINE_MS = 15 * 60_000;
// The server sends a comment every 15 s; a silent stream is a dead one.
const PUSH_SILENCE_TIMEOUT_MS = 45_000;
const PUSH_BACKOFF_MS = [1_000, 2_000, 5_000, 10_000, 30_000];

export class UiChannel {
	private readonly options: UiChannelOptions;
	private readonly fetchImpl: typeof fetch;
	private readonly now: () => number;
	private readonly listeners = new Set<(event: UiChannelEvent) => void>();
	private paths: { root: string; stateFile: string; appdataRoot: string | null } | null = null;
	private state: ChannelState | null = null;
	private running = false;
	private online: boolean | null = null;
	private pollTimer: ReturnType<typeof setInterval> | null = null;
	private pushAbort: AbortController | null = null;
	private checking: Promise<UiCheckResult> | null = null;
	private recheckQueued = false;

	constructor(options: UiChannelOptions) {
		this.options = options;
		this.fetchImpl = options.fetch ?? fetch;
		this.now = options.now ?? Date.now;
	}

	on(listener: (event: UiChannelEvent) => void) {
		this.listeners.add(listener);
		return () => this.listeners.delete(listener);
	}

	// Checks now, then on every push and poll until stop().
	start() {
		if (this.running) return;
		this.running = true;
		void this.check();
		this.pollTimer = setInterval(
			() => void this.check(),
			this.options.pollIntervalMs ?? DEFAULT_POLL_INTERVAL_MS,
		);
		if (this.options.eventsUrl) void this.subscribe(this.options.eventsUrl);
	}

	stop() {
		this.running = false;
		if (this.pollTimer) clearInterval(this.pollTimer);
		this.pollTimer = null;
		this.pushAbort?.abort();
		this.pushAbort = null;
	}

	// The install for a bundle hash, if it's on disk.
	async installed(manifest: UiManifest): Promise<UiInstall | null> {
		const paths = await this.resolvePaths();
		return existsSync(this.installPath(manifest, paths.root)) ? this.installFor(manifest, paths) : null;
	}

	// Deletes installs (unpacked or packs) whose sha256 isn't in `keep`.
	async prune(keep: Iterable<string>) {
		const { root } = await this.resolvePaths();
		const kept = new Set(keep);
		let entries: string[];
		try {
			entries = readdirSync(root);
		} catch {
			return;
		}
		for (const entry of entries) {
			const sha = /^([0-9a-f]{64})(?:\.uipack)?$/.exec(entry)?.[1];
			if (sha && !kept.has(sha)) {
				rmSync(join(root, entry), { recursive: true, force: true });
			}
		}
	}

	// One check at a time; a check requested while one runs runs once after it.
	check(): Promise<UiCheckResult> {
		if (this.checking) {
			this.recheckQueued = true;
			return this.checking;
		}
		this.checking = this.runCheck().finally(() => {
			this.checking = null;
			if (this.recheckQueued) {
				this.recheckQueued = false;
				void this.check();
			}
		});
		return this.checking;
	}

	private async runCheck(): Promise<UiCheckResult> {
		const paths = await this.resolvePaths();
		const state = this.loadState(paths.stateFile);

		let fetched: unknown;
		try {
			const response = await this.fetchImpl(this.options.manifestUrl, {
				cache: "no-store",
				signal: AbortSignal.timeout(MANIFEST_TIMEOUT_MS),
			});
			if (!response.ok) throw new Error(`manifest: HTTP ${response.status}`);
			const bytes = await readCapped(response, MANIFEST_MAX_BYTES, "manifest");
			fetched = JSON.parse(new TextDecoder().decode(bytes));
		} catch (error) {
			this.setOnline(false);
			return { result: "offline", error: String(error) };
		}
		this.setOnline(true);

		// Shape, signer, signature and expiry.
		const verified = verifyManifest(fetched, this.options.trustedKeys, this.now());
		if (!verified.ok) return this.refuse(verified.reason);
		const manifest = verified.value;
		if (manifest.seq < state.seenSeq) return this.refuse("replay", manifest);
		if (manifest.seq === state.seenSeq) return { result: "up-to-date" };
		const extra = this.options.accept?.(manifest);
		if (extra) return this.refuse(extra, manifest);

		let install: UiInstall;
		try {
			install = await this.download(manifest, paths);
		} catch (error) {
			// Not recorded as seen: the next check retries the same publish.
			return this.refuse(`download: ${String(error)}`, manifest);
		}
		this.saveState(paths.stateFile, { seenSeq: manifest.seq });
		this.emit({ type: "available", install });
		return { result: "available", install };
	}

	private async download(
		manifest: UiManifest,
		paths: { root: string; appdataRoot: string | null },
	): Promise<UiInstall> {
		const target = this.installPath(manifest, paths.root);
		if (existsSync(target)) return this.installFor(manifest, paths);

		const bundleUrl = new URL(manifest.bundleUrl, this.options.manifestUrl);
		// Signed, but a relative URL can still resolve to file:, which fetch reads.
		if (bundleUrl.protocol !== "https:" && bundleUrl.protocol !== "http:") {
			throw new Error(`bundle URL must be http(s), not ${bundleUrl.protocol}`);
		}
		const abort = new AbortController();
		const deadline = setTimeout(() => abort.abort(new Error("bundle download took over 15 minutes")), BUNDLE_DEADLINE_MS);
		let bytes: Uint8Array;
		try {
			const response = await this.fetchImpl(bundleUrl.toString(), { signal: abort.signal });
			if (!response.ok) throw new Error(`bundle: HTTP ${response.status}`);
			bytes = await readCapped(response, this.options.maxBundleBytes ?? DEFAULT_MAX_BUNDLE_BYTES, "bundle", abort);
		} finally {
			clearTimeout(deadline);
		}
		// Checks the hash, version and every path before anything is written.
		const files = readUipack(bytes, manifest);
		if (!files.ok) throw new Error(files.reason);

		// Write next to the final path and rename it into place, so a
		// half-written install is never visible under its hash.
		mkdirSync(paths.root, { recursive: true });
		const staging = join(paths.root, `.staging-${manifest.sha256}-${randomBytes(4).toString("hex")}`);
		try {
			if (this.keepsPacks) {
				writeFileSync(staging, bytes);
			} else {
				for (const [path, content] of files.value) {
					const file = join(staging, path);
					mkdirSync(dirname(file), { recursive: true });
					writeFileSync(file, content);
				}
			}
			renameSync(staging, target);
		} catch (error) {
			rmSync(staging, { recursive: true, force: true });
			if (!existsSync(target)) throw error;
		}
		return this.installFor(manifest, paths);
	}

	private get keepsPacks() {
		return this.options.keep === "pack";
	}

	private installPath(manifest: UiManifest, root: string) {
		return join(root, this.keepsPacks ? `${manifest.sha256}.uipack` : manifest.sha256);
	}

	private installFor(manifest: UiManifest, paths: { root: string; appdataRoot: string | null }): UiInstall {
		const path = this.installPath(manifest, paths.root);
		if (this.keepsPacks) return { manifest, dir: null, pack: path, url: null };
		const rel = paths.appdataRoot ? relative(paths.appdataRoot, path) : null;
		const url =
			rel && !rel.startsWith("..") && !rel.startsWith(sep)
				? `appdata://${rel.split(sep).join("/")}/index.html`
				: null;
		return { manifest, dir: path, pack: null, url };
	}

	// Server-Sent Events: any event means "check now". Reconnects with backoff
	// and treats 45 s of silence as a dropped connection.
	private async subscribe(eventsUrl: string) {
		let attempt = 0;
		while (this.running) {
			const abort = new AbortController();
			this.pushAbort = abort;
			let silence: ReturnType<typeof setTimeout> | null = null;
			const resetSilence = () => {
				if (silence) clearTimeout(silence);
				silence = setTimeout(() => abort.abort(), PUSH_SILENCE_TIMEOUT_MS);
			};
			try {
				const response = await this.fetchImpl(eventsUrl, {
					headers: { accept: "text/event-stream" },
					signal: abort.signal,
				});
				if (!response.ok || !response.body) throw new Error(`events: HTTP ${response.status}`);
				attempt = 0;
				resetSilence();
				// A (re)connect may have missed a publish.
				void this.check();
				const reader = response.body.getReader();
				const decoder = new TextDecoder();
				let buffered = "";
				for (;;) {
					const { done, value } = await reader.read();
					if (done) break;
					resetSilence();
					buffered += decoder.decode(value, { stream: true });
					// Events are tiny; a block this long without an end is junk.
					if (buffered.length > MANIFEST_MAX_BYTES) throw new Error("event too large");
					let boundary: number;
					while ((boundary = buffered.indexOf("\n\n")) !== -1) {
						const block = buffered.slice(0, boundary);
						buffered = buffered.slice(boundary + 2);
						// Lines starting with ":" are keep-alive comments.
						if (block.split("\n").some((line) => line && !line.startsWith(":"))) {
							void this.check();
						}
					}
				}
			} catch {
				// Reconnect below.
			} finally {
				if (silence) clearTimeout(silence);
			}
			if (!this.running) break;
			this.setOnline(false);
			const delay = PUSH_BACKOFF_MS[Math.min(attempt, PUSH_BACKOFF_MS.length - 1)]!;
			attempt++;
			await new Promise((resolve) => setTimeout(resolve, delay));
		}
	}

	private refuse(reason: string, manifest?: UiManifest): UiCheckResult {
		this.emit({ type: "refused", reason, manifest });
		return { result: "refused", reason };
	}

	private setOnline(online: boolean) {
		if (this.online === online) return;
		this.online = online;
		this.emit({ type: "status", online });
	}

	private emit(event: UiChannelEvent) {
		for (const listener of this.listeners) {
			try {
				listener(event);
			} catch (error) {
				console.error("UiChannel listener failed:", error);
			}
		}
	}

	// Staging dirs left by a crash mid-unpack, which appdata:// would serve.
	private sweepStaging(root: string) {
		try {
			for (const entry of readdirSync(root)) {
				if (entry.startsWith(".staging-")) rmSync(join(root, entry), { recursive: true, force: true });
			}
		} catch {
			// No install root yet.
		}
	}

	private async resolvePaths() {
		if (this.paths) return this.paths;
		let appdataRoot = this.options.appdataRoot;
		let root = this.options.root;
		let stateFile = this.options.stateFile;
		if (!root || !stateFile || (!appdataRoot && !this.keepsPacks)) {
			// Imported lazily: Utils needs the app's runtime, which unit tests don't have.
			const { paths } = await import("../core/Utils");
			if (!this.keepsPacks) appdataRoot ??= paths.appdataProtocolRoot;
			root ??= this.keepsPacks ? join(paths.userData, "appengine", "packs") : join(appdataRoot!, "ui");
			stateFile ??= join(paths.userData, "appengine", "channel.json");
		}
		this.paths = { root, stateFile, appdataRoot: appdataRoot ?? null };
		this.sweepStaging(root);
		return this.paths;
	}

	private loadState(stateFile: string): ChannelState {
		if (this.state) return this.state;
		try {
			const parsed = JSON.parse(readFileSync(stateFile, "utf8")) as Partial<ChannelState>;
			this.state = { seenSeq: typeof parsed.seenSeq === "number" ? parsed.seenSeq : 0 };
		} catch {
			this.state = { seenSeq: 0 };
		}
		return this.state;
	}

	private saveState(stateFile: string, state: ChannelState) {
		this.state = state;
		mkdirSync(dirname(stateFile), { recursive: true });
		const tmp = `${stateFile}.${randomBytes(4).toString("hex")}.tmp`;
		writeFileSync(tmp, JSON.stringify(state));
		renameSync(tmp, stateFile);
	}
}

// Reads a response body, refusing it as soon as it's larger than `maxBytes`
// (by its content-length or by what actually arrives), or when no bytes
// arrive for STALL_TIMEOUT_MS.
async function readCapped(
	response: Response,
	maxBytes: number,
	what: string,
	abort?: AbortController,
): Promise<Uint8Array> {
	const declared = Number(response.headers.get("content-length"));
	if (Number.isFinite(declared) && declared > maxBytes) {
		abort?.abort();
		throw new Error(`${what} is ${declared} bytes, over the ${maxBytes}-byte limit`);
	}
	if (!response.body) return new Uint8Array(0);
	const reader = response.body.getReader();
	const chunks: Uint8Array[] = [];
	let total = 0;
	let stall: ReturnType<typeof setTimeout> | null = null;
	try {
		for (;;) {
			const next = reader.read();
			const stalled = new Promise<never>((_, reject) => {
				stall = setTimeout(() => reject(new Error(`${what} download stalled`)), STALL_TIMEOUT_MS);
			});
			const { done, value } = await Promise.race([next, stalled]);
			if (stall) clearTimeout(stall);
			if (done) break;
			total += value.byteLength;
			if (total > maxBytes) throw new Error(`${what} is over the ${maxBytes}-byte limit`);
			chunks.push(value);
		}
	} catch (error) {
		if (stall) clearTimeout(stall);
		abort?.abort();
		void reader.cancel().catch(() => {});
		throw error;
	}
	const bytes = new Uint8Array(total);
	let offset = 0;
	for (const chunk of chunks) {
		bytes.set(chunk, offset);
		offset += chunk.byteLength;
	}
	return bytes;
}
