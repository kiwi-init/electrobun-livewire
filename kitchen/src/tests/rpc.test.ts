// RPC Tests - Tests for bidirectional RPC communication

import { defineTest, expect, sleep } from "../test-framework/types";
import { BrowserView, BuildConfig } from "electrobun/main";
import type {
  SocketSendSummary,
  StressMessageStats,
  StressRequestSummary,
  TestHarnessRPC,
} from "../test-harness/index";
import { waitForHostSocketOpen } from "./rpc-stress-readiness";

function createStressMessageCollector() {
  const ids = new Set<number>();
  const duplicates = new Set<number>();

  return {
    reset() {
      ids.clear();
      duplicates.clear();
    },
    record(id: number) {
      if (ids.has(id)) {
        duplicates.add(id);
      }
      ids.add(id);
    },
    getStats(expectedCount: number): StressMessageStats {
      const missing: number[] = [];
      for (let id = 0; id < expectedCount; id++) {
        if (!ids.has(id)) {
          missing.push(id);
        }
      }

      return {
        count: ids.size,
        expectedCount,
        missing,
        duplicates: Array.from(duplicates).sort((a, b) => a - b),
      };
    },
  };
}

type StressValueCollector<T> = {
  set(value: T): void;
  wait(label: string, timeoutMs?: number): Promise<T>;
};

function createStressValueCollector<T>(): StressValueCollector<T> {
  let value: T | undefined;

  return {
    set(nextValue: T) {
      value = nextValue;
    },
    async wait(label: string, timeoutMs = 10000) {
      const deadline = Date.now() + timeoutMs;
      while (Date.now() < deadline) {
        if (value !== undefined) {
          return value;
        }
        await sleep(50);
      }
      throw new Error(`Timed out waiting for ${label}`);
    },
  };
}

function describeStressFailure(label: string, stats: StressMessageStats) {
  return [
    `${label}: expected ${stats.expectedCount}, received ${stats.count}`,
    `missing=${stats.missing.length ? stats.missing.slice(0, 20).join(",") : "none"}`,
    `duplicates=${stats.duplicates.length ? stats.duplicates.slice(0, 20).join(",") : "none"}`,
  ].join("; ");
}

async function waitForStressStats(
  readStats: () => StressMessageStats | Promise<StressMessageStats>,
  expectedCount: number,
  timeoutMs = 5000,
) {
  const deadline = Date.now() + timeoutMs;
  let stats = await readStats();

  while (
    Date.now() < deadline &&
    (stats.count < expectedCount || stats.missing.length > 0)
  ) {
    await sleep(50);
    stats = await readStats();
  }

  return stats;
}

function assertAllStressMessagesArrived(label: string, stats: StressMessageStats) {
  if (
    stats.count !== stats.expectedCount ||
    stats.missing.length > 0 ||
    stats.duplicates.length > 0
  ) {
    throw new Error(describeStressFailure(label, stats));
  }
}

// Create RPC config for test harness
type StressHandlers = {
  ready?: () => void;
  bunStressMessages?: ReturnType<typeof createStressMessageCollector>;
  webviewMessageSummary?: StressValueCollector<StressMessageStats>;
  webviewRequestSummary?: StressValueCollector<StressRequestSummary>;
  socketSendSummary?: StressValueCollector<SocketSendSummary>;
};

export function createTestHarnessRPC(
  maxRequestTime = 10000,
  stressHandlers: StressHandlers = {},
) {
  return BrowserView.defineRPC<TestHarnessRPC>({
    maxRequestTime,
    handlers: {
      requests: {
        echo: ({ value }) => value,
        add: ({ a, b }) => a + b,
        throwError: ({ message }) => {
          throw new Error(message || "Intentional test error");
        },
        delayed: async ({ ms, value }) => {
          await new Promise((resolve) => setTimeout(resolve, ms));
          return value;
        },
      },
      messages: {
        ready: () => stressHandlers.ready?.(),
        ping: ({ timestamp }) => {
          console.log(`Received ping at ${timestamp}`);
        },
        stressMessageToBun: ({ id }) => {
          stressHandlers.bunStressMessages?.record(id);
        },
        stressWebviewMessageSummary: (stats) => {
          stressHandlers.webviewMessageSummary?.set(stats);
        },
        stressWebviewRequestSummary: (summary) => {
          stressHandlers.webviewRequestSummary?.set(summary);
        },
        stressSocketSendSummary: (summary) => {
          stressHandlers.socketSendSummary?.set(summary);
        },
      },
    },
  });
}

// Take the name as the first argument so scripts/check-*-test-mirrors.ts can
// resolve the literal test names.
function iframeBridgeTest(name: string, renderer: "native" | "cef") {
  return defineTest({
    name,
    category: "RPC",
    description: `Test that a cross-origin iframe inside a views:// page gets neither the RPC secrets nor a working RPC bridge (${renderer} renderer)`,
    timeout: 20000,
    async run({ createWindow, log }) {
      const received: number[] = [];
      const rpc = createTestHarnessRPC(3000, {
        bunStressMessages: { record: (id: number) => received.push(id) } as any,
      });
      await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        renderer,
        title: `Iframe bridge (${renderer})`,
      });
      await sleep(2000);
      expect(await rpc.request.multiply({ a: 6, b: 7 })).toBe(42);

      // The iframe tries every way to post an RPC message straight to Bun,
      // then reports which Electrobun globals it can see.
      const iframeHtml = `<script>
        const packet = (id) => JSON.stringify({ type: "message", id: "stressMessageToBun", payload: { id } });
        try { window.webkit.messageHandlers.hostBridge.postMessage(packet(777)); } catch (e) {}
        try { window.__electrobunHostBridge.postMessage(packet(778)); } catch (e) {}
        parent.postMessage({ iframeProbe: {
          key: typeof window.__electrobunSecretKeyBytes,
          port: typeof window.__electrobunRpcSocketPort,
          hostBridge: typeof window.__electrobunHostBridge,
          internalBridge: typeof window.__electrobunInternalBridge,
        } }, "*");
      <\/script>`;
      log("Loading a cross-origin data: iframe");
      const probe = await rpc.request.evaluateJavascriptWithResponse({
        script: `return new Promise((resolve) => {
          window.addEventListener("message", (event) => {
            if (event.data && event.data.iframeProbe) resolve(event.data.iframeProbe);
          });
          const frame = document.createElement("iframe");
          frame.src = "data:text/html," + encodeURIComponent(${JSON.stringify(iframeHtml)});
          document.body.appendChild(frame);
          setTimeout(() => resolve("no probe"), 5000);
        });`,
      });
      log(`iframe sees: ${JSON.stringify(probe)}`);
      expect(probe).toEqual({
        key: "undefined",
        port: "undefined",
        hostBridge: "undefined",
        internalBridge: "undefined",
      });

      // Control: the page itself can still post through its bridge.
      await rpc.request.evaluateJavascriptWithResponse({
        script: `window.__electrobunHostBridge.postMessage(JSON.stringify({ type: "message", id: "stressMessageToBun", payload: { id: 1 } })); return true;`,
      });
      await sleep(1000);
      log(`Bun received: ${JSON.stringify(received)}`);
      expect(received.includes(1)).toBe(true);
      expect(received.includes(777) || received.includes(778)).toBe(false);
    },
  });
}

// Posts an RPC message straight to every bridge the current page might have.
function postToBridges(id: number) {
  return `(() => {
    const packet = JSON.stringify({ type: "message", id: "stressMessageToBun", payload: { id: ${id} } });
    try { window.webkit.messageHandlers.hostBridge.postMessage(packet); } catch (e) {}
    try { window.__electrobunHostBridge.postMessage(packet); } catch (e) {}
  })()`;
}

function bridgeOriginsTest(name: string, renderer: "native" | "cef") {
  return defineTest({
    name,
    category: "RPC",
    description: `Test that a page outside bridgeOrigins can't reach RPC and an allowed page can (${renderer} renderer)`,
    timeout: 30000,
    async run({ createWindow, log }) {
      const received: number[] = [];
      const rpc = createTestHarnessRPC(3000, {
        bunStressMessages: { record: (id: number) => received.push(id) } as any,
      });
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        renderer,
        bridgeOrigins: ["views://test-harness/*"],
        title: `bridgeOrigins (${renderer})`,
      });
      await sleep(2000);
      expect(await rpc.request.multiply({ a: 6, b: 7 })).toBe(42);

      log("Navigating to a page outside bridgeOrigins");
      win.webview.loadURL("views://test-runner/index.html");
      await sleep(2500);
      win.webview.executeJavascript(postToBridges(888));
      await sleep(1000);

      // That page can't answer RPC, so it reports what it saw by navigating
      // back to the allowed harness with the result in the query string.
      log("Navigating back to an allowed page with the probe result");
      win.webview.executeJavascript(
        `location.href = "views://test-harness/index.html?probe=" + [typeof window.__electrobunSecretKeyBytes, typeof window.__electrobunRpcSocketPort].join(",")`,
      );
      await sleep(2500);
      const search = await rpc.request.evaluateJavascriptWithResponse({ script: "return location.search" });
      expect(search).toBe("?probe=undefined,undefined");
      win.webview.executeJavascript(postToBridges(2));
      await sleep(1000);

      log(`Bun received: ${JSON.stringify(received)}`);
      expect(received.includes(2)).toBe(true);
      expect(received.includes(888)).toBe(false);
    },
  });
}

function defaultBridgeOriginTest(name: string, renderer: "native" | "cef") {
  return defineTest({
    name,
    category: "RPC",
    description: `Test that by default a view keeps RPC on the http origin it starts on and loses it on another origin (${renderer} renderer)`,
    timeout: 30000,
    async run({ createWindow, log }) {
      const page = () =>
        new Response("<!doctype html><html><body>served</body></html>", {
          headers: { "content-type": "text/html" },
        });
      const first = Bun.serve({ hostname: "127.0.0.1", port: 0, fetch: page });
      const second = Bun.serve({ hostname: "127.0.0.1", port: 0, fetch: page });
      try {
        const received: number[] = [];
        const rpc = createTestHarnessRPC(3000, {
          bunStressMessages: { record: (id: number) => received.push(id) } as any,
        });
        const win = await createWindow({
          url: `http://127.0.0.1:${first.port}/`,
          rpc,
          renderer,
          title: `Default bridge origin (${renderer})`,
        });
        await sleep(2000);
        win.webview.executeJavascript(postToBridges(4));
        await sleep(1000);

        log(`Navigating to another origin (port ${second.port})`);
        win.webview.loadURL(`http://127.0.0.1:${second.port}/`);
        await sleep(2500);
        win.webview.executeJavascript(postToBridges(890));
        await sleep(1000);

        log(`Bun received: ${JSON.stringify(received)}`);
        expect(received.includes(4)).toBe(true);
        expect(received.includes(890)).toBe(false);
      } finally {
        first.stop(true);
        second.stop(true);
      }
    },
  });
}

function inlineHtmlBridgeTest(name: string, renderer: "native" | "cef") {
  return defineTest({
    name,
    category: "RPC",
    description: `Test that a view created with inline html keeps its RPC bridge by default (${renderer} renderer)`,
    timeout: 20000,
    async run({ createWindow, log }) {
      const received: number[] = [];
      const rpc = createTestHarnessRPC(3000, {
        bunStressMessages: { record: (id: number) => received.push(id) } as any,
      });
      const win = await createWindow({
        html: "<!doctype html><html><body>inline</body></html>",
        rpc,
        renderer,
        title: `Inline html bridge (${renderer})`,
      });
      await sleep(2000);
      win.webview.executeJavascript(postToBridges(3));
      await sleep(1000);
      log(`Bun received: ${JSON.stringify(received)}`);
      expect(received.includes(3)).toBe(true);
    },
  });
}

// CEF injects the preload by rewriting the HTML response; a document without
// <head> must still get it (and keep its doctype's standards mode).
function headlessPreloadTest(name: string, body: string, compatMode: string) {
  return defineTest({
    name,
    category: "RPC",
    description: "Test that CEF injects the preload into a document that has no <head>",
    timeout: 25000,
    async run({ createWindow, log }) {
      const server = Bun.serve({
        hostname: "127.0.0.1",
        port: 0,
        fetch: () => new Response(body, { headers: { "content-type": "text/html" } }),
      });
      try {
        const rpc = createTestHarnessRPC(3000);
        const win = await createWindow({
          url: `http://127.0.0.1:${server.port}/`,
          rpc,
          renderer: "cef",
          title: name,
        });
        await sleep(2000);
        // The page can't answer RPC, so it reports by navigating to the
        // harness (a default bridge origin) with the result in the query.
        win.webview.executeJavascript(
          `location.href = "views://test-harness/index.html?probe=" + [typeof window.__electrobun, document.compatMode].join(",")`,
        );
        await sleep(2500);
        const search = await rpc.request.evaluateJavascriptWithResponse({ script: "return location.search" });
        log(`probe: ${search}`);
        expect(search).toBe(`?probe=object,${compatMode}`);
      } finally {
        server.stop(true);
      }
    },
  });
}

export const rpcTests = [
	defineTest({
		name: "Webview tag BrowserView created event",
		category: "BrowserView",
		description:
			"Attach typed RPC when an electrobun-webview tag creates its BrowserView",
		timeout: 20000,
		async run({ createWindow, log }) {
			const tagRpc = createTestHarnessRPC(10000);
			let callbackView: BrowserView | undefined;
			let resolveDomReady: (() => void) | undefined;
			const domReady = new Promise<void>((resolve) => {
				resolveDomReady = resolve;
			});

			const removeCreatedListener = BrowserView.on("created", (view) => {
				callbackView = view;
				expect(BrowserView.getById(view.id)).toBe(view);
				tagRpc.setTransport(view.createTransport());
				view.on("dom-ready", () => resolveDomReady?.());
			});

			try {
				await createWindow({
					url: "views://test-oopif/index.html",
					title: "Webview tag created event",
					renderer: "cef", // Intentionally exercises system fallback without CEF.
				});

				await domReady;
				expect(callbackView).toBeDefined();
				const product = await tagRpc.request.multiply({ a: 6, b: 7 });
				expect(product).toBe(42);
				log(`Attached typed RPC to BrowserView ${callbackView!.id}`);
			} finally {
				removeCreatedListener();
			}
		},
	}),

  defineTest({
    name: "bun to webview: request with response",
    category: "RPC",
    description: "Test that bun can make RPC requests to webview and receive responses",
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC();
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "RPC Test",
        renderer: 'cef',
      });

      // Wait longer for webview to be ready when running in parallel
      // CEF takes longer to initialize under heavy load
      await sleep(2000);

      log("Calling webview.multiply({ a: 6, b: 7 })");
      const result = await win.webview.rpc?.request.multiply({ a: 6, b: 7 });

      expect(result).toBe(42);
      log(`Got result: ${result}`);
    },
  }),

  defineTest({
    name: "shared rpc survives closing the older window",
    category: "RPC",
    description: "Test that an rpc instance handed to a newer window keeps working after the older window closes",
    timeout: 15000,
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC();
      const older = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "Shared RPC (older)",
      });
      await sleep(1000);
      await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "Shared RPC (newer)",
      });
      await sleep(2000);

      log("Closing the older window");
      older.close();
      await sleep(500);

      log("Calling multiply on the newer window through the shared rpc");
      const result = await rpc.request.multiply({ a: 6, b: 7 });
      expect(result).toBe(42);
    },
  }),

  defineTest({
    name: "rpc socket keeps the page connection and hides the key",
    category: "RPC",
    description: "Test that a connection without the webview's key can't displace the page's RPC socket, and that page scripts can't read the raw key",
    timeout: 20000,
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC(3000);
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "RPC Socket Test",
      });
      await sleep(2000);
      expect(await rpc.request.multiply({ a: 6, b: 7 })).toBe(42);

      const keyType = await rpc.request.evaluateJavascriptWithResponse({
        script: "return typeof window.__electrobunSecretKeyBytes",
      });
      expect(keyType).toBe("undefined");

      const before = await rpc.request.getHostSocketStressState();
      expect(before.readyState).toBe(1);

      log("Opening a connection without the key for the same webview");
      const rogue = new WebSocket(
        `ws://127.0.0.1:${before.hostSocketPort}/socket?webviewId=${win.webview.id}`,
      );
      await new Promise<void>((resolve) => {
        rogue.addEventListener("open", () => resolve());
        rogue.addEventListener("error", () => resolve());
        setTimeout(resolve, 2000);
      });
      if (rogue.readyState === WebSocket.OPEN) rogue.send("not encrypted");
      await sleep(1000);
      rogue.close();

      const after = await rpc.request.getHostSocketStressState();
      expect(after.readyState).toBe(1);
      expect(await rpc.request.multiply({ a: 6, b: 7 })).toBe(42);
    },
  }),

  iframeBridgeTest("cross-origin iframe can't reach RPC (system webview)", "native"),
  iframeBridgeTest("cross-origin iframe can't reach RPC (CEF)", "cef"),
  bridgeOriginsTest("bridgeOrigins keeps RPC off other pages (system webview)", "native"),
  bridgeOriginsTest("bridgeOrigins keeps RPC off other pages (CEF)", "cef"),
  defaultBridgeOriginTest("default bridge origin is the starting http origin (system webview)", "native"),
  defaultBridgeOriginTest("default bridge origin is the starting http origin (CEF)", "cef"),
  inlineHtmlBridgeTest("inline html views keep their bridge (system webview)", "native"),
  inlineHtmlBridgeTest("inline html views keep their bridge (CEF)", "cef"),

  defineTest({
    name: "bridge messages without the view's token are dropped",
    category: "RPC",
    description: "Test that a message posted straight to the native bridge, bypassing the preload's bridge wrapper, never reaches Bun",
    timeout: 20000,
    async run({ createWindow, log }) {
      const received: number[] = [];
      const rpc = createTestHarnessRPC(3000, {
        bunStressMessages: { record: (id: number) => received.push(id) } as any,
      });
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        renderer: "native",
        title: "Bridge token",
      });
      await sleep(2000);
      const packet = (id: number) =>
        JSON.stringify({ type: "message", id: "stressMessageToBun", payload: { id } });
      win.webview.executeJavascript(
        `try { window.webkit.messageHandlers.hostBridge.postMessage(${JSON.stringify(packet(901))}); } catch (e) {}`,
      );
      win.webview.executeJavascript(
        `window.__electrobunHostBridge.postMessage(${JSON.stringify(packet(5))});`,
      );
      await sleep(1000);
      log(`Bun received: ${JSON.stringify(received)}`);
      expect(received.includes(5)).toBe(true);
      expect(received.includes(901)).toBe(false);
    },
  }),

  defineTest({
    name: "rpc socket closes connections that never authenticate",
    category: "RPC",
    description: "Test that an idle page keeps its RPC socket (it authenticates with a hello on open) while a connection that never proves the key is closed",
    timeout: 40000,
    async run({ createWindow, log }) {
      // One socket server serves every view; read its port from a helper page
      // so the idle page below never sends anything but its hello.
      const helperRpc = createTestHarnessRPC(3000);
      await createWindow({ url: "views://test-harness/index.html", rpc: helperRpc, title: "RPC Auth Helper" });
      await sleep(2000);
      const { hostSocketPort } = await helperRpc.request.getHostSocketStressState();

      const rpc = createTestHarnessRPC(3000);
      const idle = await createWindow({ url: "views://test-harness/index.html", rpc, title: "RPC Auth Idle" });

      log("Opening a connection that never sends anything");
      const silent = new WebSocket(`ws://127.0.0.1:${hostSocketPort}/socket?webviewId=${idle.webview.id}`);
      await sleep(14000);

      expect(silent.readyState).toBe(WebSocket.CLOSED);
      const state = await rpc.request.getHostSocketStressState();
      expect(state.readyState).toBe(1);
      expect(await rpc.request.multiply({ a: 6, b: 7 })).toBe(42);
    },
  }),

  headlessPreloadTest("CEF preload in a head-less document with a doctype", "<!doctype html><body>headless</body>", "CSS1Compat"),
  headlessPreloadTest("CEF preload in a head-less document without a doctype", "<p>no doctype, no head</p>", "BackCompat"),

  defineTest({
    name: "webview to bun: request with response",
    category: "RPC",
    description: "Test that webview can make RPC requests to bun and receive responses",
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC();
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "RPC Test",
        renderer: 'cef',
      });

      await sleep(1000);

      // Execute JS in webview to call bun's add method
      log("Triggering webview to call bun.add({ a: 100, b: 23 })");
      const result = await win.webview.rpc?.request.evaluateJavascriptWithResponse({
        script: `return window.electrobun.rpc.request.add({ a: 100, b: 23 });`,
      });

      expect(result).toBe(123);
      log(`Got result: ${result}`);
    },
  }),

  defineTest({
    name: "webview to bun: per-request timeout override",
    category: "RPC",
    description: "Keep a modal-style request alive without changing other RPC timeouts",
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC();
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "RPC Timeout Override Test",
        renderer: BuildConfig.getSync().defaultRenderer,
      });

      await sleep(1000);

      const timedOut = await win.webview.rpc?.request.evaluateJavascriptWithResponse({
        script: `
          return window.electrobun.rpc.request.delayed(
            { ms: 150, value: "late" },
            { maxRequestTime: 20 },
          ).then(
            () => "unexpected success",
            error => error.message,
          );
        `,
      });
      expect(timedOut).toBe("RPC request timed out.");

      const dialogStyleResult = await win.webview.rpc?.request.evaluateJavascriptWithResponse({
        script: `
          return window.electrobun.rpc.request.delayed(
            { ms: 150, value: "dialog completed" },
            { maxRequestTime: Infinity },
          );
        `,
      });
      expect(dialogStyleResult).toBe("dialog completed");
      log("Finite override timed out and modal-style request completed");
    },
  }),

  defineTest({
    name: "RPC echo with string",
    category: "RPC",
    description: "Test echo with a simple string",
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC();
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "RPC Test",
        renderer: 'cef',
      });

      await sleep(1000);

      const testString = "Hello, Electrobun!";
      log(`Testing echo with: "${testString}"`);

      const result = await win.webview.rpc?.request.evaluateJavascriptWithResponse({
        script: `return window.electrobun.rpc.request.echo({ value: "${testString}" });`,
      });

      expect(result).toBe(testString);
      log(`Echo successful: ${result}`);
    },
  }),

  defineTest({
    name: "RPC native Unicode survives idle round trips",
    category: "RPC",
    description:
      "Verify mixed-script UTF-8 remains exact across the Windows native RPC fallback after idle periods",
    timeout: 30000,
    async run({ createWindow, log }) {
      const ready = createStressValueCollector<boolean>();
      const rpc = createTestHarnessRPC(20000, { ready: () => ready.set(true) });
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "Unicode RPC 测试 Тест",
        renderer: "native",
      });

      const webviewRpc = win.webview.rpc;
      if (!webviewRpc) {
        throw new Error("Expected webview RPC to be available");
      }

      await ready.wait("native test harness initialization");
      const scripts =
        "Български Ελληνικά 日本語 测试 한국어 café 🧪🚀";
      const boundaryPayload = Array.from(
        { length: 2048 },
        (_, index) => `${index}:${scripts}\n`,
      ).join("");

      log("Checking native RPC readiness with a short Unicode echo");
      expect(await webviewRpc.request.echoFromHost({ value: scripts })).toBe(scripts);
      for (let round = 0; round < 3; round++) {
        // Leave the transport idle between requests. This is safe to automate
        // while still exercising the resume-sensitive native transport path.
        await sleep(250);
        log(`Native Unicode round ${round + 1}: sending ${boundaryPayload.length} UTF-16 code units`);
        const result = await webviewRpc.request.echoFromHost({
          value: `${round}:${boundaryPayload}`,
        });
        expect(result).toBe(`${round}:${boundaryPayload}`);
        log(`Native Unicode round ${round + 1}: exact echo received`);
      }

      log(
        `Preserved ${boundaryPayload.length} UTF-16 code units over 3 native RPC round trips`,
      );
    },
  }),

  defineTest({
    name: "RPC large payload transfer",
    category: "RPC",
    description: "Test transferring 1MB of data via RPC",
    timeout: 30000,
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC();
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "RPC Test",
        renderer: 'cef',
      });

      await sleep(1000);

      const size = 1024 * 1024; // 1MB
      log(`Sending ${size} bytes to bun`);

      const result = await win.webview.rpc?.request.evaluateJavascriptWithResponse({
        script: `
          const bigData = 'x'.repeat(${size});
          return window.electrobun.rpc.request.echo({ value: bigData }).then(r => r.length);
        `,
      });

      expect(result).toBe(size);
      log(`Received ${result} bytes back`);
    },
  }),

  defineTest({
    name: "evaluateJavascriptWithResponse - sync",
    category: "RPC",
    description: "Test evaluating synchronous JavaScript in webview",
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC();
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "RPC Test",
        renderer: 'cef',
      });

      await sleep(1000);

      log("Evaluating: 2 + 2");
      const result = await win.webview.rpc?.request.evaluateJavascriptWithResponse({
        script: `return 2 + 2`,
      });

      expect(result).toBe(4);
      log(`Result: ${result}`);
    },
  }),

  defineTest({
    name: "evaluateJavascriptWithResponse - async/promise",
    category: "RPC",
    description: "Test evaluating async JavaScript that returns a promise",
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC();
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "RPC Test",
        renderer: 'cef',
      });

      // Wait longer for webview to be ready when running in parallel
      // CEF takes longer to initialize under heavy load
      await sleep(2000);

      log("Evaluating async script with 200ms delay");
      const startTime = Date.now();

      const result = await win.webview.rpc?.request.evaluateJavascriptWithResponse({
        script: `
          return new Promise(resolve => {
            setTimeout(() => resolve('delayed result'), 200);
          });
        `,
      });

      const elapsed = Date.now() - startTime;
      expect(result).toBe("delayed result");
      expect(elapsed).toBeGreaterThanOrEqual(200);
      log(`Result: ${result}, took ${elapsed}ms`);
    },
  }),

  defineTest({
    name: "evaluateJavascriptWithResponse - DOM access",
    category: "RPC",
    description: "Test that evaluated JS can access the DOM",
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC();
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "RPC Test Window",
        renderer: 'cef',
      });

      await sleep(1000);

      log("Getting H1 content via JS evaluation");
      const result = await win.webview.rpc?.request.evaluateJavascriptWithResponse({
        script: `return document.querySelector('h1')?.textContent`,
      });

      expect(result).toBe("Test Harness");
      log(`H1 content: ${result}`);
    },
  }),

  defineTest({
    name: "RPC getDocumentTitle",
    category: "RPC",
    description: "Test getting document title via RPC request",
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC();
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "RPC Test",
        renderer: 'cef',
      });

      await sleep(1000);

      log("Getting document title via RPC");
      const title = await win.webview.rpc?.request.getDocumentTitle({});

      expect(title).toBe("Test Harness");
      log(`Document title: ${title}`);
    },
  }),

  defineTest({
    name: "RPC stress: native burst delivery",
    category: "RPC",
    description: "Stress native renderer RPC bursts to catch dropped messages or responses",
    timeout: 120000,
    async run({ createWindow, log }) {
      const messageCount = 5000;
      const requestCount = 1000;
      const bunStressMessages = createStressMessageCollector();
      const webviewMessageSummary = createStressValueCollector<StressMessageStats>();
      const webviewRequestSummary = createStressValueCollector<StressRequestSummary>();
      const rpc = createTestHarnessRPC(30000, {
        bunStressMessages,
        webviewMessageSummary,
        webviewRequestSummary,
      });
      bunStressMessages.reset();
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "RPC Native Stress Test",
        renderer: 'native',
      });

      const webviewRpc = win.webview.rpc;
      if (!webviewRpc) {
        throw new Error("Expected webview RPC to be available");
      }

      await sleep(1000);
      const socketState = await waitForHostSocketOpen(webviewRpc);
      log(`Host socket open before native burst: ${JSON.stringify(socketState)}`);
      log("Starting webview stress burst via bun -> webview RPC message");
      webviewRpc.send.startStressFromBun({ messageCount, requestCount });

      log(`Sending ${messageCount} large fire-and-forget messages from bun to webview concurrently`);
      const outputPayload = "x".repeat(16384);
      for (let id = 0; id < messageCount; id++) {
        webviewRpc.send.stressMessageToWebview({
          id,
          payload: `${id}:${outputPayload}`,
        });
      }
      webviewRpc.send.finishStressMessageToWebview({ expectedCount: messageCount });

      log(`Waiting for ${messageCount} fire-and-forget messages from webview to bun`);
      const bunStats = await waitForStressStats(
        () => bunStressMessages.getStats(messageCount),
        messageCount,
        30000,
      );
      assertAllStressMessagesArrived("webview -> bun messages", bunStats);
      log(`Received all ${bunStats.count} webview -> bun messages`);

      log(`Waiting for ${requestCount} concurrent requests from webview to bun`);
      const webviewRequestResult = await webviewRequestSummary.wait(
        "webview -> bun request summary",
        45000,
      );
      expect(webviewRequestResult.received).toBe(requestCount);
      if (webviewRequestResult.errorCount > 0) {
        log(`Webview -> bun request errors: ${JSON.stringify(webviewRequestResult.errors)}`);
      }
      expect(webviewRequestResult.errorCount).toBe(0);
      expect(webviewRequestResult.mismatchCount).toBe(0);
      log(`Completed ${webviewRequestResult.received} webview -> bun requests`);

      const webviewStats = await webviewMessageSummary.wait(
        "bun -> webview message summary",
        30000,
      );
      assertAllStressMessagesArrived("bun -> webview messages", webviewStats);
      log(`Received all ${webviewStats.count} bun -> webview messages`);

      log(`Running ${requestCount} concurrent requests from bun to webview`);
      const bunRequestResults = await Promise.all(
        Array.from({ length: requestCount }, (_, id) =>
          webviewRpc.request.multiply({ a: id, b: 1 })
            .then((value: number) => ({ id, value }))
            .catch((error: Error) => ({ id, error: String(error?.message || error) })),
        ),
      );
      const bunRequestErrors = bunRequestResults.filter((result) => "error" in result);
      const bunRequestMismatches = bunRequestResults.filter(
        (result) => !("error" in result) && result.value !== result.id,
      );
      if (bunRequestErrors.length > 0) {
        log(`Bun -> webview request errors: ${JSON.stringify(bunRequestErrors.slice(0, 10))}`);
      }

      expect(bunRequestResults).toHaveLength(requestCount);
      expect(bunRequestErrors).toHaveLength(0);
      expect(bunRequestMismatches).toHaveLength(0);
      log(`Completed ${bunRequestResults.length} bun -> webview requests`);
    },
  }),

  defineTest({
    name: "RPC stress: native fallback to socket transition",
    category: "RPC",
    description: "Stress the webview-to-bun RPC transition from postMessage fallback to websocket transport",
    timeout: 120000,
    async run({ createWindow, log }) {
      const messageCount = 5000;
      const requestCount = 500;
      const enableSocketAt = Math.floor(messageCount / 2);
      const bunStressMessages = createStressMessageCollector();
      const webviewRequestSummary = createStressValueCollector<StressRequestSummary>();
      const rpc = createTestHarnessRPC(30000, {
        bunStressMessages,
        webviewRequestSummary,
      });
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "RPC Native Transport Transition Test",
        renderer: 'native',
      });

      const webviewRpc = win.webview.rpc;
      if (!webviewRpc) {
        throw new Error("Expected webview RPC to be available");
      }

      await sleep(1000);
      const socketState = await waitForHostSocketOpen(webviewRpc);
      log(`Host socket open before transition: ${JSON.stringify(socketState)}`);
      expect(socketState.socketUrl, "transition socket URL").toContain("ws://127.0.0.1:");
      expect(socketState.canSend, "transition socket send-ready").toBe(true);

      bunStressMessages.reset();
      log(
        `Starting ${messageCount} webview -> bun messages: first ${enableSocketAt} via fallback, rest via websocket`,
      );
      webviewRpc.send.startTransportTransitionStressFromBun({
        messageCount,
        requestCount,
        enableSocketAt,
      });

      const bunStats = await waitForStressStats(
        () => bunStressMessages.getStats(messageCount),
        messageCount,
        30000,
      );
      assertAllStressMessagesArrived("webview -> bun fallback/socket transition messages", bunStats);
      log(`Received all ${bunStats.count} transition messages`);

      const webviewRequestResult = await webviewRequestSummary.wait(
        "webview -> bun transition request summary",
        45000,
      );
      expect(webviewRequestResult.received).toBe(requestCount);
      if (webviewRequestResult.errorCount > 0) {
        log(`Webview -> bun transition request errors: ${JSON.stringify(webviewRequestResult.errors)}`);
      }
      expect(webviewRequestResult.errorCount).toBe(0);
      expect(webviewRequestResult.mismatchCount).toBe(0);
      log(`Completed ${webviewRequestResult.received} post-transition webview -> bun requests`);
    },
  }),

  defineTest({
    name: "RPC stress: native steady socket delivery",
    category: "RPC",
    description: "Send low-rate webview-to-bun RPC messages over websocket to catch persistent socket drops",
    timeout: 30000,
    async run({ createWindow, log }) {
      const messageCount = 30;
      const intervalMs = 100;
      const bunStressMessages = createStressMessageCollector();
      const socketSendSummary = createStressValueCollector<SocketSendSummary>();
      const rpc = createTestHarnessRPC(10000, {
        bunStressMessages,
        socketSendSummary,
      });
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "RPC Native Steady Socket Test",
        renderer: 'native',
      });

      const webviewRpc = win.webview.rpc;
      if (!webviewRpc) {
        throw new Error("Expected webview RPC to be available");
      }

      await sleep(1000);
      const socketState = await waitForHostSocketOpen(webviewRpc);
      log(`Host socket open before steady socket test: ${JSON.stringify(socketState)}`);
      expect(socketState.socketUrl, "steady socket URL").toContain("ws://127.0.0.1:");
      expect(socketState.canSend, "steady socket send-ready").toBe(true);

      bunStressMessages.reset();
      log(`Starting ${messageCount} webview -> bun websocket messages at ${intervalMs}ms intervals`);
      webviewRpc.send.startTimedSocketStressFromBun({
        messageCount,
        intervalMs,
      });

      const summary = await socketSendSummary.wait("webview socket send summary", 15000);
      log(`Socket send summary: ${JSON.stringify(summary)}`);

      const bunStats = await waitForStressStats(
        () => bunStressMessages.getStats(messageCount),
        messageCount,
        10000,
      );
      assertAllStressMessagesArrived("steady webview -> bun socket messages", bunStats);
      log(`Received all ${bunStats.count} steady socket messages`);
    },
  }),

  defineTest({
    name: "BrowserView.getAll",
    category: "BrowserView",
    description: "Test getting all browser views",
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC();

      // Get count before creating windows
      const viewsBefore = BrowserView.getAll();
      const countBefore = viewsBefore.length;
      log(`Views before: ${countBefore}`);

      // Create two windows
      const win1 = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "GetAll Test 1",
        renderer: 'cef',
      });
      const win2 = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "GetAll Test 2",
        renderer: 'cef',
      });

      await sleep(500);

      const viewsAfter = BrowserView.getAll();
      log(`Views after creating 2 windows: ${viewsAfter.length}`);

      // Should have at least 2 more views (the webviews for the windows we created)
      expect(viewsAfter.length).toBeGreaterThanOrEqual(countBefore + 2);

      // Verify our webviews are in the list
      const found1 = viewsAfter.find((v) => v.id === win1.webview.id);
      const found2 = viewsAfter.find((v) => v.id === win2.webview.id);

      expect(found1).toBeTruthy();
      expect(found2).toBeTruthy();

      log("BrowserView.getAll works correctly");
    },
  }),

  defineTest({
    name: "BrowserView.getById",
    category: "BrowserView",
    description: "Test getting a browser view by ID",
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC();
      const win = await createWindow({
        url: "views://test-harness/index.html",
        rpc,
        title: "GetById Test",
        renderer: 'cef',
      });

      await sleep(500);

      const webviewId = win.webview.id;
      log(`Looking up webview with ID: ${webviewId}`);

      const found = BrowserView.getById(webviewId);
      expect(found).toBeTruthy();
      expect(found?.id).toBe(webviewId);

      // Test non-existent ID
      const notFound = BrowserView.getById(999999);
      expect(notFound).toBeFalsy();

      log("BrowserView.getById works correctly");
    },
  }),
];
