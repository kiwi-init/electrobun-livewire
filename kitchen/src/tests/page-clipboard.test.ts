// Page clipboard: what a page copies with navigator.clipboard reaches other apps

import { defineTest, expect, sleep } from "../test-framework/types";
import { Utils } from "electrobun/main";
import { createTestHarnessRPC } from "./rpc.test";

const ARM = `
  window.kitchenCopies = [];
  const button = document.createElement("button");
  button.id = "kitchen-copy";
  Object.assign(button.style, { position: "fixed", inset: "0", width: "100vw", height: "100vh", zIndex: "99999", opacity: "0.01" });
  button.addEventListener("click", () => {
    const record = (kind, p) => p.then(() => window.kitchenCopies.push({ kind, ok: true }), (e) => window.kitchenCopies.push({ kind, ok: false, error: String(e && e.name) + ": " + String(e && e.message) }));
    const active = navigator.userActivation ? navigator.userActivation.isActive : "no userActivation API";
    window.kitchenCopies.push({ kind: "activation", ok: active });
    if (window.kitchenMode === "text") return record("writeText", navigator.clipboard.writeText(window.kitchenText));
    const canvas = document.createElement("canvas");
    canvas.width = 32;
    canvas.height = 24;
    const context = canvas.getContext("2d");
    context.fillStyle = "#3366ff";
    context.fillRect(0, 0, 32, 24);
    const png = new Promise((ok, no) => canvas.toBlob((blob) => (blob ? ok(blob) : no(new Error("no PNG"))), "image/png"));
    return record("write image/png", navigator.clipboard.write([new ClipboardItem({ "image/png": png })]));
  });
  document.body.append(button);
  return true;
`;

export const pageClipboardTests = [
  defineTest({
    name: "Text and a PNG the page copies reach the system clipboard",
    category: "Utils",
    description: "A click with a user gesture (WebKit's native evaluate) runs navigator.clipboard.writeText, then write with an image/png ClipboardItem; wl-paste reads each",
    requires: { platform: "linux", display: "wayland", renderer: "native", foreground: true },
    timeout: 30000,
    async run({ createWindow, log }) {
      const wlPaste = Bun.which("wl-paste");
      const wtype = Bun.which("wtype");
      if (!wlPaste || !wtype) throw new Error("needs wl-clipboard and wtype");
      const rpc = createTestHarnessRPC(8000);
      const win = await createWindow({ url: "views://test-harness/index.html", rpc, renderer: "native", title: "Page Clipboard Test" });
      await sleep(1500);
      win.window.focus();
      await rpc.request.evaluateJavascriptWithResponse({ script: ARM });
      // A compositor takes a selection only from a client with a recent input
      // event, as a real click gives; wtype's key press stands in for one.
      const click = async () => {
        win.window.focus();
        await sleep(100);
        Bun.spawnSync([wtype, "-k", "Shift_L"], { timeout: 5000 });
        await sleep(100);
        win.webview.executeJavascript(`document.getElementById("kitchen-copy").click()`);
        await sleep(1200);
      };
      const text = `from the page ${Date.now()}`;
      Utils.clipboardWriteText("");
      await rpc.request.evaluateJavascriptWithResponse({ script: `window.kitchenMode = "text"; window.kitchenText = ${JSON.stringify(text)}; return true;` });
      await click();
      const pastedText = Bun.spawnSync([wlPaste, "--no-newline"], { timeout: 5000 }).stdout.toString();
      log(`after writeText: ${JSON.stringify(await rpc.request.evaluateJavascriptWithResponse({ script: "return window.kitchenCopies" }))}; wl-paste: ${JSON.stringify(pastedText.slice(0, 60))}`);
      await rpc.request.evaluateJavascriptWithResponse({ script: `window.kitchenMode = "image"; window.kitchenCopies = []; return true;` });
      await click();
      const types = Bun.spawnSync([wlPaste, "--list-types"], { timeout: 5000 }).stdout.toString().trim();
      log(`after write image/png: ${JSON.stringify(await rpc.request.evaluateJavascriptWithResponse({ script: "return window.kitchenCopies" }))}; wl-paste --list-types: ${JSON.stringify(types)}`);
      expect(pastedText).toBe(text);
      expect(types.split("\n")).toContain("image/png");
    },
  }),
];
