// `window.engine`: what a UI booted by AppEngine talks to. AppEngine injects
// this as the view's preload, so UI bundles don't bundle anything.
//
// A convenience facade, not a security boundary: page code can reach the RPC
// underneath. Every check lives in AppEngine's handlers.

import { Electroview } from "../index";
import { ENGINE_CONTRACT_GLOBAL, type AppEngineRPC, type EngineContract } from "../../shared/appengine-protocol";

// AppEngine runs a line that sets the contract right before this script. Take
// it off the global so page code finds it only on window.engine.
const globals = globalThis as unknown as Record<string, EngineContract | undefined>;
const contract: EngineContract = Object.freeze({
	version: globals[ENGINE_CONTRACT_GLOBAL]?.version ?? "0.0.0",
	capabilities: Object.freeze([...(globals[ENGINE_CONTRACT_GLOBAL]?.capabilities ?? [])]),
});
delete globals[ENGINE_CONTRACT_GLOBAL];

type SwapHandler = (info: { toVersion: string }) => unknown | Promise<unknown>;

let swapHandler: SwapHandler | null = null;
let readySent = false;
const listeners = new Map<string, Set<(payload: unknown) => void>>();

const rpc = Electroview.defineRPC<AppEngineRPC>({
	maxRequestTime: 30_000,
	handlers: {
		requests: {
			prepareSwap: async ({ toVersion }) => ({
				state: swapHandler ? await swapHandler({ toVersion }) : null,
			}),
		},
		messages: {
			event: ({ name, payload }) => {
				for (const listener of listeners.get(name) ?? []) listener(payload);
			},
		},
	},
});
new Electroview({ rpc });

async function invoke(cap: string, args: unknown = {}): Promise<unknown> {
	const result = await rpc.request.invoke({ cap, args });
	if (!result.ok) {
		const error = new Error(`${cap}: ${result.message}`) as Error & { code: string };
		error.code = result.code;
		throw error;
	}
	return result.value;
}

// Errors before ready() fail the boot right away instead of at the deadline;
// errors after it fail a UI that's still on probation.
let runtimeErrorsSent = 0;
function report(message: string) {
	if (!readySent) rpc.send.bootError({ href: location.href, message });
	else if (runtimeErrorsSent++ < 20) rpc.send.runtimeError({ href: location.href, message });
}
// WebKit's `stack` leaves out the message, so send both.
const describe = (error: unknown) =>
	error instanceof Error ? `${error.name}: ${error.message}\n${error.stack ?? ""}` : String(error);
window.addEventListener("error", (event) => report(event.error ? describe(event.error) : event.message));
window.addEventListener("unhandledrejection", (event) => report(describe(event.reason)));

const engine = Object.freeze({
	invoke,
	// The contract this engine implements: its semver version and the
	// capabilities invoke() accepts. Feature-detect with has(cap).
	contract,
	has: (cap: string) => contract.capabilities.includes(cap),
	// Tell the engine this UI booted. Call it once the UI can be used; until
	// then a failure rolls back to the previous UI.
	ready: (info?: unknown) => {
		readySent = true;
		rpc.send.ready({ href: location.href, info });
	},
	// Some errors never reach window.onerror (a throw inside a
	// startViewTransition callback, errors an error boundary caught), so UIs
	// report those themselves.
	reportError: (error: unknown) => report(describe(error)),
	// State to hand to the next UI version on a hot swap.
	onBeforeSwap: (handler: SwapHandler) => {
		swapHandler = handler;
	},
	on: (name: string, listener: (payload: unknown) => void) => {
		if (!listeners.has(name)) listeners.set(name, new Set());
		listeners.get(name)!.add(listener);
		return () => listeners.get(name)?.delete(listener);
	},
	info: () => invoke("engine.info"),
	session: Object.freeze({
		takeHandoff: () => invoke("session.takeHandoff"),
	}),
});

Object.defineProperty(window, "engine", {
	value: engine,
	writable: false,
	configurable: false,
	enumerable: true,
});
