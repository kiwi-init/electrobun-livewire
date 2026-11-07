// A sandboxed subframe can't post a trusted view's events (T6816)

import { defineTest, expect, sleep } from "../test-framework/types";
import { createTestHarnessRPC } from "./rpc.test";

const FORGER = `<script>
addEventListener("message", (e) => {
  const { id, replay } = e.data || {};
  const bridge = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.eventBridge;
  const post = (message) => {
    try {
      bridge.postMessage(message);
      return true;
    } catch (err) {
      return String(err);
    }
  };
  const event = (eventName, detail) => JSON.stringify({ id: "webviewEvent", type: "message", payload: { id, eventName, detail } });
  const head = replay && /^([0-9a-f]{64})(\\d+):/.exec(replay);
  parent.postMessage({
    forged: {
      bridge: !!bridge,
      newWindow: post(event("new-window-open", JSON.stringify({ url: "https://example.com/forged", policy: "deny" }))),
      domReady: post(event("dom-ready", "https://example.com/forged")),
      hostMessage: post(event("host-message", JSON.stringify({ from: "forged" }))),
      replay: replay ? post(replay) : null,
      bumped: head ? post(head[1] + (Number(head[2]) + 1000) + replay.slice(head[0].length - 1)) : null,
      asToken: replay ? post(replay.slice(0, 32) + event("host-message", JSON.stringify({ from: "forged" }))) : null,
    },
  }, "*");
});
parent.postMessage({ forgerReady: true }, "*");
<\/script>`;

export const eventBridgeTests = [
  defineTest({
    name: "a sandboxed subframe's forged events never reach the host",
    category: "Event bridge",
    description:
      "A trusted view's sandboxed subframe posts unsigned new-window-open, dom-ready and host-message events, replays one its top page signed, bumps its counter and uses its head as a token; none arrive, the same from a reloaded top page doesn't either, and the top page's own events still do",
    timeout: 40000,
    requires: { platform: ["darwin", "linux"], renderer: "native" },
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC(5000);
      const win = await createWindow({ url: "views://test-harness/index.html", rpc, renderer: "native", title: "Event Bridge" });
      await sleep(1500);
      const evaluate = (script: string) => rpc.request.evaluateJavascriptWithResponse({ script }) as Promise<any>;
      const got = { newWindow: [] as any[], domReady: [] as any[], host: [] as any[] };
      win.webview.on("new-window-open", (e: any) => got.newWindow.push(e.data.detail));
      win.webview.on("dom-ready", (e: any) => got.domReady.push(e.data.detail));
      win.webview.on("host-message", (e: any) => got.host.push(e.data.detail));

      const signed = await evaluate(`return (async () => {
        const raw = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.eventBridge;
        if (!raw) return { raw: false, captured: null };
        const real = raw.postMessage;
        let captured = null;
        raw.postMessage = function (message) {
          if (captured === null && String(message).includes("host-message")) captured = String(message);
          return real.call(this, message);
        };
        window.__electrobunSendToHost({ from: "top" });
        await new Promise((r) => setTimeout(r, 300));
        delete raw.postMessage;
        return { raw: true, captured };
      })();`);
      log(`the top page's signed host-message, captured: ${signed.captured ? `${signed.captured.length} chars` : "none"}`);
      expect(signed.raw).toBe(true);
      expect(typeof signed.captured).toBe("string");

      const forged = await evaluate(`return (async () => {
        window.__forged = null;
        const ready = new Promise((r) => addEventListener("message", (e) => { if (e.data && e.data.forgerReady) r(true); }));
        addEventListener("message", (e) => { if (e.data && e.data.forged) window.__forged = e.data.forged; });
        const frame = document.createElement("iframe");
        frame.sandbox = "allow-scripts";
        frame.srcdoc = ${JSON.stringify(FORGER)};
        document.body.appendChild(frame);
        await Promise.race([ready, new Promise((r) => setTimeout(r, 5000))]);
        frame.contentWindow.postMessage({ id: window.__electrobunWebviewId, replay: ${JSON.stringify(signed.captured)} }, "*");
        for (let i = 0; i < 50 && !window.__forged; i++) await new Promise((r) => setTimeout(r, 100));
        return window.__forged;
      })();`);
      log(`the subframe posted: ${JSON.stringify(forged)}`);
      expect(forged?.bridge).toBe(true);
      expect(forged?.newWindow).toBe(true);
      expect(forged?.domReady).toBe(true);
      expect(forged?.hostMessage).toBe(true);
      expect(forged?.replay).toBe(true);
      expect(forged?.bumped).toBe(true);
      expect(forged?.asToken).toBe(true);

      await sleep(1000);
      await evaluate(`window.__electrobunSendToHost({ from: "top, after" }); return true;`);
      await sleep(1000);
      log(`host saw: new-window-open ${JSON.stringify(got.newWindow)}, dom-ready ${JSON.stringify(got.domReady)}, host-message ${JSON.stringify(got.host)}`);
      expect(got.newWindow).toHaveLength(0);
      expect(got.domReady).toHaveLength(0);
      expect(got.host.filter((m) => m?.from === "forged")).toHaveLength(0);
      expect(got.host.filter((m) => m?.from === "top")).toHaveLength(1);
      expect(got.host.filter((m) => m?.from === "top, after")).toHaveLength(1);

      win.webview.loadURL("views://test-harness/index.html");
      await sleep(1500);
      const again = await evaluate(`
        const raw = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.eventBridge;
        const captured = ${JSON.stringify(signed.captured)};
        const head = /^([0-9a-f]{64})(\\d+):/.exec(captured);
        const forged = JSON.stringify({ id: "webviewEvent", type: "message", payload: { id: window.__electrobunWebviewId, eventName: "host-message", detail: JSON.stringify({ from: "forged" }) } });
        raw.postMessage(captured);
        raw.postMessage(head[1] + (Number(head[2]) + 2000) + captured.slice(head[0].length - 1));
        raw.postMessage(captured.slice(0, 32) + forged);
        window.__electrobunSendToHost({ from: "reloaded" });
        return true;
      `);
      expect(again).toBe(true);
      await sleep(1000);
      log(`after a reload, host saw: dom-ready ${JSON.stringify(got.domReady)}, host-message ${JSON.stringify(got.host)}`);
      expect(got.domReady.length).toBeGreaterThanOrEqual(1);
      expect(got.host.filter((m) => m?.from === "forged")).toHaveLength(0);
      expect(got.host.filter((m) => m?.from === "top")).toHaveLength(1);
      expect(got.host.filter((m) => m?.from === "reloaded")).toHaveLength(1);
    },
  }),
];
