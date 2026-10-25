// After some wakes from sleep, a macOS WebContent process loses its
// connection to WebKit's Networking process: the page keeps running JS, but no
// request it makes ever completes and nothing repaints, until the process is
// replaced. So after each wake every system webview is probed, and only views
// showing exactly that (the probe's script ran, but its loopback request never
// arrived) get a fresh process and reload. A view whose script didn't run at
// all is busy or hung, not frozen this way; it keeps its state.

import { randomBytes } from "crypto";
import { ffi } from "../proc/native";
import { BrowserView } from "./BrowserView";

// Time for the network to come back after a wake before probing, and how long
// a probe may take.
const SETTLE_MS = 2_000;
const PROBE_TIMEOUT_MS = 5_000;

// Probes every system webview (or only those in `ids`) and recycles the
// frozen ones. Returns the ids of the webviews it recycled. `probePort` points
// the probes elsewhere, for tests that need a network path that's down.
export async function recoverWebviewsAfterWake(
	options: { settleMs?: number; timeoutMs?: number; probePort?: number; ids?: number[] } = {},
): Promise<number[]> {
	await sleep(options.settleMs ?? SETTLE_MS);
	const probes: { view: BrowserView; nonce: string }[] = [];
	for (const view of BrowserView.getAll()) {
		if (view.isRemoved || (options.ids && !options.ids.includes(view.id))) continue;
		const nonce = randomBytes(16).toString("hex");
		if (ffi.request.webviewProbeNetwork({ id: view.id, nonce, port: options.probePort })) {
			probes.push({ view, nonce });
		}
	}
	if (probes.length === 0) return [];

	await sleep(options.timeoutMs ?? PROBE_TIMEOUT_MS);
	const recycled: number[] = [];
	for (const { view, nonce } of probes) {
		if (view.isRemoved) continue;
		const jsAlive = ffi.request.webviewProbeAnswered({ nonce });
		if (!jsAlive || ffi.request.hostTransportProbeSeen({ nonce })) continue;
		// Lets owners of the view (AppEngine) tell this from a crash.
		view.rendererExitExpected = "wake-recovery";
		view.terminateRenderer();
		ffi.request.webviewReload({ id: view.id });
		// If no render-process-gone ever arrives, don't hide a later real crash.
		setTimeout(() => {
			if (view.rendererExitExpected === "wake-recovery") view.rendererExitExpected = null;
		}, 10_000);
		recycled.push(view.id);
	}
	return recycled;
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
