// Page clipboard: what a page copies with navigator.clipboard reaches other apps

import { defineTest, expect, sleep } from "../test-framework/types";
import { Utils } from "electrobun/main";
import { createTestHarnessRPC } from "./rpc.test";

const COPY_PNG = `
  const canvas = document.createElement("canvas");
  canvas.width = 32;
  canvas.height = 24;
  const context = canvas.getContext("2d");
  context.fillStyle = "#3366ff";
  context.fillRect(0, 0, 32, 24);
  const png = new Promise((ok, no) => canvas.toBlob((blob) => (blob ? ok(blob) : no(new Error("no PNG"))), "image/png"));
  return navigator.clipboard
    .write([new ClipboardItem({ "image/png": png })])
    .then(() => ({ wrote: true }), (e) => ({ wrote: false, error: String(e && e.name) + ": " + String(e && e.message) }));
`;

export const pageClipboardTests = [
  defineTest({
    name: "A PNG the page copies reaches the system clipboard",
    category: "Utils",
    description: "navigator.clipboard.write with an image/png ClipboardItem puts a PNG on the clipboard other apps read",
    requires: { platform: "linux", renderer: "native", foreground: true },
    timeout: 20000,
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC(8000);
      const win = await createWindow({ url: "views://test-harness/index.html", rpc, renderer: "native", title: "Page Clipboard Test" });
      await sleep(1500);
      win.window.focus();
      await sleep(300);
      if (process.env["KITCHEN_HEADLESS_WAYLAND"]) {
        const wtype = Bun.which("wtype");
        if (wtype) Bun.spawnSync([wtype, "-k", "Shift_L"], { timeout: 5000 });
        await sleep(100);
      }
      Utils.clipboardWriteText(`before ${Date.now()}`);
      await sleep(200);

      const result = await rpc.request.evaluateJavascriptWithResponse({ script: COPY_PNG });
      log(`navigator.clipboard.write: ${JSON.stringify(result)}`);
      await sleep(500);
      const formats = Utils.clipboardAvailableFormats();
      log(`clipboard formats: ${formats.join(", ")}`);
      const wlPaste = process.env["WAYLAND_DISPLAY"] ? Bun.which("wl-paste") : null;
      if (wlPaste) {
        const types = Bun.spawnSync([wlPaste, "--list-types"], { timeout: 5000 });
        log(`wl-paste --list-types: ${types.stdout.toString().trim() || types.stderr.toString().trim()}`);
      }
      const image = Utils.clipboardReadImage();
      log(`clipboardReadImage: ${image ? `${image.length} bytes` : "null"}`);
      expect(result.wrote).toBe(true);
      expect(image !== null && image.length > 0).toBe(true);
    },
  }),
];
