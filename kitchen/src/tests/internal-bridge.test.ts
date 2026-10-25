// Internal bridge messages act only on the page's own view, window and tags

import { BrowserView, WGPUView } from "electrobun/main";
import { defineTest, expect, sleep } from "../test-framework/types";
import { createTestHarnessRPC } from "./rpc.test";

async function openHarness(createWindow: any, title: string) {
  const rpc = createTestHarnessRPC(5000);
  const win = await createWindow({ url: "views://test-harness/index.html", rpc, renderer: "native", title });
  await sleep(1500);
  const evaluate = (script: string) => rpc.request.evaluateJavascriptWithResponse({ script }) as Promise<any>;
  return { win, evaluate };
}

// Posts raw internal bridge messages from the page, as a page's own script can.
const postInternal = (messages: object[]) =>
  `window.__electrobunInternalBridge.postMessage(${JSON.stringify(JSON.stringify(messages.map((m) => JSON.stringify(m))))}); return true;`;

export const internalBridgeTests = [
  defineTest({
    name: "a page's internal bridge can't act on another view",
    category: "Internal bridge",
    description: "A page in view A names view B in tag messages (run JS, navigate, load HTML, change rules or preload, remove); B is untouched",
    timeout: 30000,
    requires: { renderer: "native" },
    async run({ createWindow, log }) {
      const a = await openHarness(createWindow, "View A");
      const b = await openHarness(createWindow, "View B");
      const target = b.win.webviewId;
      const view = BrowserView.getById(target)!;
      const before = await b.evaluate("return { href: location.href, title: document.title }");
      const rules = view.navigationRules;
      const preload = view.preload;

      await a.evaluate(postInternal([
        { type: "message", id: "webviewTagExecuteJavascript", payload: { id: target, js: "window.__hit = 'from A'" } },
        { type: "message", id: "webviewTagSetNavigationRules", payload: { id: target, rules: ["^*"] } },
        { type: "message", id: "webviewTagUpdatePreload", payload: { id: target, preload: "window.__preloaded = true" } },
      ]));
      await sleep(1000);
      const hit = await b.evaluate("return window.__hit ?? null");
      log(`after run-JS, rules and preload: B's __hit ${JSON.stringify(hit)}, rules ${JSON.stringify(view.navigationRules)}`);
      expect(hit).toBe(null);
      expect(view.navigationRules).toBe(rules);
      expect(view.preload).toBe(preload);

      await a.evaluate(postInternal([
        { type: "message", id: "webviewTagUpdateHtml", payload: { id: target, html: "<title>replaced</title>" } },
        { type: "message", id: "webviewTagUpdateSrc", payload: { id: target, url: "data:text/html,<title>navigated</title>" } },
        { type: "message", id: "webviewTagRemove", payload: { id: target } },
      ]));
      await sleep(1500);
      expect(view.isRemoved).toBe(false);
      const after = await b.evaluate("return { href: location.href, title: document.title }");
      log(`after load-HTML, navigate and remove: B before ${JSON.stringify(before)}, after ${JSON.stringify(after)}`);
      expect(after.href).toBe(before.href);
      expect(after.title).toBe(before.title);
    },
  }),

  defineTest({
    name: "a tag a page creates belongs to that page, and replies come back to it",
    category: "Internal bridge",
    description: "webviewTagInit with another view's hostWebviewId and windowId still makes a tag of the asking view in its own window, and the response reaches the asking page, not the view it named",
    timeout: 30000,
    requires: { renderer: "native" },
    async run({ createWindow, log }) {
      const a = await openHarness(createWindow, "View A");
      const b = await openHarness(createWindow, "View B");
      // Record internal responses each page receives.
      const record = `
        window.__responses = [];
        const original = window.__electrobun.receiveInternalMessageFromHost;
        window.__electrobun.receiveInternalMessageFromHost = (msg) => { window.__responses.push(msg); original(msg); };
        return true;`;
      await a.evaluate(record);
      await b.evaluate(record);

      await a.evaluate(postInternal([{
        type: "request",
        method: "webviewTagInit",
        id: "spoofed-1",
        hostWebviewId: b.win.webviewId,
        params: {
          hostWebviewId: b.win.webviewId,
          windowId: b.win.id,
          html: "<p>tag</p>",
          frame: { x: 0, y: 0, width: 10, height: 10 },
          renderer: "native",
          navigationRules: null,
        },
      }]));
      await sleep(1500);

      const toA = (await a.evaluate("return window.__responses.filter((r) => r.id === 'spoofed-1')")) as any[];
      const toB = (await b.evaluate("return window.__responses.filter((r) => r.id === 'spoofed-1')")) as any[];
      log(`responses: A ${JSON.stringify(toA)}, B ${JSON.stringify(toB)}`);
      expect(toB.length).toBe(0);
      expect(toA.length).toBe(1);
      const tag = BrowserView.getById(toA[0].payload)!;
      expect(tag.hostWebviewId).toBe(a.win.webviewId);
      expect(tag.windowId).toBe(a.win.id);

      // A still drives its own tag.
      await a.evaluate(postInternal([{ type: "message", id: "webviewTagRemove", payload: { id: tag.id } }]));
      await sleep(800);
      log(`A's tag ${tag.id}: host ${tag.hostWebviewId}, window ${tag.windowId}, removed by A: ${tag.isRemoved}`);
      expect(tag.isRemoved).toBe(true);
    },
  }),

  defineTest({
    name: "a page's wgpu tags are its own",
    category: "Internal bridge",
    description: "An <electrobun-wgpu> tag's view belongs to the page that made it: another view's page can't remove it, its own page can",
    timeout: 30000,
    requires: { platform: "darwin", renderer: "native" },
    async run({ createWindow, log }) {
      const a = await openHarness(createWindow, "View A");
      const b = await openHarness(createWindow, "View B");
      const tagId = await a.evaluate(`
        const tag = document.createElement("electrobun-wgpu");
        tag.style.cssText = "display:block;width:40px;height:40px";
        document.body.appendChild(tag);
        return new Promise((resolve) => {
          let tries = 0;
          const poll = () => (tag.wgpuViewId != null || ++tries > 50 ? resolve(tag.wgpuViewId) : setTimeout(poll, 100));
          poll();
        });`);
      const tag = WGPUView.getById(tagId)!;
      log(`A's wgpu tag ${tagId}: host ${tag?.hostWebviewId}, window ${tag?.windowId}`);
      expect(tag.hostWebviewId).toBe(a.win.webviewId);
      expect(tag.windowId).toBe(a.win.id);

      await b.evaluate(postInternal([{ type: "message", id: "wgpuTagRemove", payload: { id: tagId } }]));
      await sleep(800);
      expect(WGPUView.getById(tagId)).toBe(tag);

      await a.evaluate(postInternal([{ type: "message", id: "wgpuTagRemove", payload: { id: tagId } }]));
      await sleep(800);
      expect(WGPUView.getById(tagId)).toBe(undefined);
    },
  }),
];
