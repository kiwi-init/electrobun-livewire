// New Window Tests - window.open and target=_blank under each newWindow policy

import { defineTest, expect, sleep } from "../test-framework/types";
import { createTestHarnessRPC } from "./rpc.test";

async function openHarness(createWindow: any, newWindow?: "deny" | "popup") {
  const rpc = createTestHarnessRPC(5000);
  const win = await createWindow({
    url: "views://test-harness/index.html",
    rpc,
    renderer: "native",
    title: "New Window Test",
    newWindow,
  });
  await sleep(1500);
  const opened: any[] = [];
  win.webview.on("new-window-open", (e: any) => opened.push(e.data.detail));
  return { rpc, win, opened };
}

export const newWindowTests = [
  defineTest({
    name: "window.open is denied by default",
    category: "New Window",
    description: "Without a newWindow policy, window.open returns null and new-window-open reports deny",
    timeout: 20000,
    requires: { platform: ["darwin", "linux"], renderer: "native" },
    async run({ createWindow, log }) {
      const { rpc, opened } = await openHarness(createWindow);
      const isNull = await rpc.request.evaluateJavascriptWithResponse({
        script: `return window.open("https://example.com/denied", "_blank") === null`,
      });
      await sleep(500);
      log(`new-window-open: ${JSON.stringify(opened)}`);
      expect(isNull).toBe(true);
      expect(opened).toHaveLength(1);
      expect(opened[0].url).toBe("https://example.com/denied");
      expect(opened[0].policy).toBe("deny");
    },
  }),

  defineTest({
    name: "window.open popup keeps window.opener",
    category: "New Window",
    description: "With newWindow: popup, window.open returns a live window whose page can postMessage its opener, without Electrobun's bridge",
    timeout: 20000,
    requires: { platform: ["darwin", "linux"], renderer: "native" },
    async run({ createWindow, log }) {
      const { rpc, opened } = await openHarness(createWindow, "popup");
      const result = await rpc.request.evaluateJavascriptWithResponse({
        script: `
          window.__fromPopup = null;
          window.addEventListener("message", (e) => { window.__fromPopup = e.data; });
          window.__popup = window.open("", "_blank", "width=420,height=320");
          if (!window.__popup) return { opened: false };
          window.__popup.document.write("<script>opener.postMessage('hello from popup', '*')<\\/script>");
          return { opened: true, bridge: typeof window.__popup.__electrobun };
        `,
      });
      await sleep(800);
      const message = await rpc.request.evaluateJavascriptWithResponse({ script: "return window.__fromPopup" });
      log(`popup: ${JSON.stringify(result)}, message: ${message}, events: ${JSON.stringify(opened)}`);
      expect(result.opened).toBe(true);
      expect(result.bridge).toBe("undefined");
      expect(message).toBe("hello from popup");
      expect(opened[0]?.policy).toBe("popup");

      const closed = await rpc.request.evaluateJavascriptWithResponse({
        script: `window.__popup.close(); return new Promise((r) => setTimeout(() => r(window.__popup.closed), 500))`,
      });
      expect(closed).toBe(true);
    },
  }),

  defineTest({
    name: "a popup can't navigate its opener",
    category: "New Window",
    description: "A popup setting opener.location is blocked, so a third-party popup can't move the app's page",
    timeout: 20000,
    requires: { platform: "darwin", renderer: "native" },
    async run({ createWindow, log }) {
      const { rpc } = await openHarness(createWindow, "popup");
      await rpc.request.evaluateJavascriptWithResponse({
        script: `
          window.__popup = window.open("", "_blank", "width=420,height=320");
          window.__popup.document.write("<script>opener.location.href = 'views://test-harness/index.html?hijacked'<\\/script>");
          return true;
        `,
      });
      await sleep(1500);
      const search = await rpc.request.evaluateJavascriptWithResponse({
        script: `window.__popup?.close(); return location.search`,
      });
      log(`opener location.search after the attempt: "${search}"`);
      expect(search).toBe("");
    },
  }),
];
