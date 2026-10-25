// The AppEngine contract: what an engine build promises the UIs it runs. It
// has a semver version and a fixed set of capabilities, the only things a UI
// can make the main process do through `engine.invoke(cap, args)`.
//
// A manifest's `requiresContract` is a semver range. The engine refuses a UI
// whose range its version doesn't satisfy before downloading it, so a UI that
// needs a newer engine never runs. Additive capabilities bump the minor
// version; UIs feature-detect them with `engine.has(cap)`.

import { ENGINE_CONTRACT_GLOBAL, type EngineContract, type EngineInvokeResult } from "../../../shared/appengine-protocol";
import { assertStrictSemVer } from "../../../shared/strict-semver.js";
import type { UiManifest } from "./format";

export type CapabilityHandler = (args: unknown) => unknown | Promise<unknown>;

// node-semver range grammar: `||`-separated sets of space-separated
// comparators (`>=1.2.0`, `^1.2`, `~1.2.3`, `1.x`, `*`) or a hyphen range.
const PART = String.raw`(?:0|[1-9]\d*|[xX*])`;
const IDENT = String.raw`[0-9A-Za-z-]+`;
const PARTIAL = String.raw`${PART}(?:\.${PART}(?:\.${PART}(?:-${IDENT}(?:\.${IDENT})*)?(?:\+${IDENT}(?:\.${IDENT})*)?)?)?`;
const COMPARATOR = new RegExp(String.raw`^(?:<=|>=|<|>|=|\^|~>?)?${PARTIAL}$`);
const HYPHEN = new RegExp(String.raw`^${PARTIAL} - ${PARTIAL}$`);

// Why `range` isn't a semver range, or null when it is. Bun.semver.satisfies
// treats anything it can't parse ("banana", "^", "") as "*", so ranges are
// checked here first: a typo must not let every UI through.
export function contractRangeError(range: string): string | null {
	const sets = range.trim().split("||").map((set) => set.trim().replace(/(<=|>=|<|>|=|\^|~>?)\s+/g, "$1").replace(/\s+/g, " "));
	for (const set of sets) {
		if (set === "") return `"${range}" has an empty range`;
		if (HYPHEN.test(set)) continue;
		const bad = set.split(" ").find((comparator) => !COMPARATOR.test(comparator));
		if (bad !== undefined) return `"${range}" isn't a semver range ("${bad}")`;
	}
	return null;
}

// Why the engine won't run this UI, or null when its contract is satisfied.
export function contractRefusal(version: string, manifest: UiManifest): string | null {
	const rangeError = contractRangeError(manifest.requiresContract);
	if (rangeError) return `UI ${manifest.uiVersion}: requiresContract ${rangeError}`;
	let satisfied = false;
	try {
		satisfied = Bun.semver.satisfies(version, manifest.requiresContract);
	} catch {}
	if (satisfied) return null;
	return `UI ${manifest.uiVersion} requires contract "${manifest.requiresContract}", but this engine implements ${version}`;
}

// The capability allowlist. Lookups only see the names it was built with, never
// inherited ones like "constructor" or "toString".
export class Capabilities {
	private readonly handlers: Map<string, CapabilityHandler>;
	readonly contract: EngineContract;

	constructor(version: string, builtIns: Record<string, CapabilityHandler>, app: Record<string, CapabilityHandler> = {}) {
		assertStrictSemVer(version, "contract version");
		this.handlers = new Map(Object.entries(builtIns));
		for (const [name, handler] of Object.entries(app)) {
			if (this.handlers.has(name)) throw new Error(`capability "${name}" is built into AppEngine`);
			if (typeof handler !== "function") throw new Error(`capability "${name}" must be a function`);
			this.handlers.set(name, handler);
		}
		this.contract = Object.freeze({
			version,
			capabilities: Object.freeze([...this.handlers.keys()].sort()),
		});
	}

	has(cap: unknown): boolean {
		return typeof cap === "string" && this.handlers.has(cap);
	}

	async invoke(cap: unknown, args: unknown): Promise<EngineInvokeResult> {
		const handler = typeof cap === "string" ? this.handlers.get(cap) : undefined;
		if (!handler) {
			return {
				ok: false,
				code: "E_NO_CAPABILITY",
				message: `contract ${this.contract.version} has no capability ${JSON.stringify(cap)}`,
			};
		}
		try {
			return { ok: true, value: await handler(args) };
		} catch (error) {
			// Handlers that throw a coded error (StorageError's E_BAD_ARGS,
			// E_QUOTA) keep the code, so UIs can tell failures apart.
			const code = (error as { code?: unknown } | null)?.code;
			return {
				ok: false,
				code: typeof code === "string" && /^E_[A-Z_]+$/.test(code) ? code : "E_FAILED",
				message: error instanceof Error ? error.message : String(error),
			};
		}
	}

	// Script the engine runs ahead of the `window.engine` facade, so the page
	// knows the contract without a round trip. The facade reads and deletes it
	// before any page script runs.
	pagePrelude(): string {
		return `globalThis.${ENGINE_CONTRACT_GLOBAL} = ${JSON.stringify(this.contract)};\n`;
	}
}
