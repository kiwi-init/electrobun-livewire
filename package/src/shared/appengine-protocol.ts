// The RPC between AppEngine (main process) and the `window.engine` facade it
// injects into the UI it boots. Both halves compile this file, so they can't
// disagree inside one devkit.

import type { RPCSchema } from "./rpc";

export type EngineInvokeResult =
	| { ok: true; value: unknown }
	| { ok: false; code: string; message: string };

export type AppEngineRPC = {
	bun: RPCSchema<{
		requests: {
			invoke: { params: { cap: string; args: unknown }; response: EngineInvokeResult };
		};
		messages: {
			// The health signal: a UI only counts as booted once it sends this.
			// It must not wait for a paint: rAF is suspended in hidden windows.
			ready: { href: string; info?: unknown };
			// Uncaught error or rejection before ready(): the boot failed.
			bootError: { href: string; message: string };
			// Uncaught error or rejection after ready(): fails a UI on probation.
			runtimeError: { href: string; message: string };
		};
	}>;
	webview: RPCSchema<{
		requests: {
			// Asked right before a hot swap. Whatever the UI returns is what the
			// next UI gets from session.takeHandoff.
			prepareSwap: { params: { toVersion: string }; response: { state: unknown } };
		};
		messages: {
			event: { name: string; payload: unknown };
		};
	}>;
};
