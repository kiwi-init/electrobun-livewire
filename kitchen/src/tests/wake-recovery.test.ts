// After a wake from sleep, macOS webviews whose page can't reach the network
// get a fresh web process; the rest are left alone.

import { Utils } from "electrobun/main";
import { defineTest, expect, sleep } from "../test-framework/types";
import { createTestHarnessRPC } from "./rpc.test";

export const wakeRecoveryTests = [
	defineTest({
		name: "wake recovery leaves a webview that can reach the network alone",
		category: "BrowserView",
		description: "The post-wake probe reaches the core, so the page isn't recycled or reloaded",
		timeout: 20000,
		requires: { platform: "darwin" },
		async run({ createWindow, log }) {
			const rpc = createTestHarnessRPC(3000);
			const win = await createWindow({ url: "views://test-harness/index.html", rpc, renderer: "native", title: "Wake: healthy" });
			await sleep(1500);
			win.webview.executeJavascript("window.__beforeWake = true");
			const recycled = await Utils.recoverWebviewsAfterWake({ settleMs: 0, timeoutMs: 2000, ids: [win.webview.id] });
			log(`recycled: ${JSON.stringify(recycled)}`);
			expect(recycled.includes(win.webview.id)).toBe(false);
			expect(await rpc.request.evaluateJavascriptWithResponse({ script: "return window.__beforeWake === true" })).toBe(true);
		},
	}),

	defineTest({
		name: "wake recovery gives a frozen webview a fresh process",
		category: "BrowserView",
		description: "A page whose JS runs but whose network path is dead (the #550 freeze; simulated by probing a closed port) is recycled and works again",
		timeout: 30000,
		requires: { platform: "darwin" },
		async run({ createWindow, log }) {
			const rpc = createTestHarnessRPC(2000);
			const win = await createWindow({ url: "views://test-harness/index.html", rpc, renderer: "native", title: "Wake: frozen" });
			await sleep(1500);
			// Port 1 refuses the connection, so the probe's script runs but its
			// request never reaches the core: the frozen signature.
			const recycled = await Utils.recoverWebviewsAfterWake({ settleMs: 0, timeoutMs: 2000, probePort: 1, ids: [win.webview.id] });
			log(`recycled: ${JSON.stringify(recycled)}`);
			expect(recycled.includes(win.webview.id)).toBe(true);
			// The reload runs in a new process, so the page answers again.
			const deadline = Date.now() + 10000;
			let result: number | undefined;
			while (result === undefined && Date.now() < deadline) {
				await sleep(500);
				result = await rpc.request.multiply({ a: 6, b: 7 }).catch(() => undefined);
			}
			expect(result).toBe(42);
		},
	}),

	defineTest({
		name: "wake recovery leaves a busy webview alone",
		category: "BrowserView",
		description: "A page whose main thread is busy can't run the probe at all; that's not the frozen signature, so it isn't recycled and keeps its state",
		timeout: 30000,
		requires: { platform: "darwin" },
		async run({ createWindow, log }) {
			const win = await createWindow({ url: "views://test-harness/index.html", renderer: "native", title: "Wake: busy" });
			await sleep(1500);
			win.webview.executeJavascript("setTimeout(() => { const end = Date.now() + 6000; while (Date.now() < end) {} }, 0)");
			await sleep(300);
			const recycled = await Utils.recoverWebviewsAfterWake({ settleMs: 0, timeoutMs: 2000, probePort: 1, ids: [win.webview.id] });
			log(`recycled: ${JSON.stringify(recycled)}`);
			expect(recycled.includes(win.webview.id)).toBe(false);
		},
	}),

	defineTest({
		name: "wake recovery isn't fooled by a page's CSP",
		category: "BrowserView",
		description: "The probe runs in its own content world, so a page whose CSP forbids connections isn't mistaken for a frozen one",
		timeout: 20000,
		requires: { platform: "darwin" },
		async run({ createWindow, log }) {
			const server = Bun.serve({
				hostname: "127.0.0.1",
				port: 0,
				fetch: () =>
					new Response("<!doctype html><h1>strict</h1>", {
						headers: { "content-type": "text/html", "content-security-policy": "default-src 'none'; connect-src 'none'" },
					}),
			});
			try {
				const win = await createWindow({ url: `http://127.0.0.1:${server.port}/`, renderer: "native", title: "Wake: strict CSP" });
				await sleep(1500);
				const recycled = await Utils.recoverWebviewsAfterWake({ settleMs: 0, timeoutMs: 2000, ids: [win.webview.id] });
				log(`recycled: ${JSON.stringify(recycled)}`);
				expect(recycled.includes(win.webview.id)).toBe(false);
			} finally {
				server.stop(true);
			}
		},
	}),
];
