// File drag out of a view: startFileDrag and fileDragRoots

import { mkdirSync, mkdtempSync, symlinkSync, writeFileSync } from "fs";
import { tmpdir } from "os";
import { join } from "path";
import { defineTest, expect, sleep } from "../test-framework/types";
import { createTestHarnessRPC } from "./rpc.test";

// An exports folder the view may drag from, a private one next to it, and a
// link from the first into the second.
function fixture() {
  const dir = mkdtempSync(join(tmpdir(), "kitchen-file-drag-"));
  const exports = join(dir, "exports");
  const privateDir = join(dir, "private");
  mkdirSync(exports);
  mkdirSync(privateDir);
  writeFileSync(join(exports, "report.txt"), "report");
  writeFileSync(join(privateDir, "secret.txt"), "secret");
  symlinkSync(join(privateDir, "secret.txt"), join(exports, "link.txt"));
  return { dir, exports, privateDir };
}

async function openHarness(createWindow: any, fileDragRoots?: string[]) {
  const rpc = createTestHarnessRPC(5000);
  const win = await createWindow({
    url: "views://test-harness/index.html",
    rpc,
    renderer: "native",
    title: "File drag",
    fileDragRoots,
  });
  await sleep(1500);
  // What the page's startFileDrag() from electrobun/view resolves to.
  const drag = (paths: string | string[]) =>
    rpc.request.evaluateJavascriptWithResponse({
      script: `return window.__electrobun.startFileDrag(${JSON.stringify(paths)})`,
    }) as Promise<{ started: boolean; reason?: string }>;
  return { win, drag };
}

export const fileDragTests = [
  defineTest({
    name: "a page can't drag files out of a view without fileDragRoots",
    category: "File drag",
    description: "startFileDrag() from a page is refused when the app gave its view no fileDragRoots",
    timeout: 20000,
    requires: { platform: "darwin", renderer: "native" },
    async run({ createWindow, log }) {
      const { exports } = fixture();
      const { drag } = await openHarness(createWindow);
      const result = await drag(join(exports, "report.txt"));
      log(JSON.stringify(result));
      expect(result.started).toBe(false);
      expect(result.reason).toContain("no fileDragRoots");
    },
  }),

  defineTest({
    name: "a page can only drag files inside its view's fileDragRoots",
    category: "File drag",
    description: "Paths outside the roots are refused however they're spelled (.., a symlink); a path inside gets as far as the native check, which wants a pressed mouse button",
    timeout: 20000,
    requires: { platform: "darwin", renderer: "native" },
    async run({ createWindow, log }) {
      const { exports, privateDir } = fixture();
      const { win, drag } = await openHarness(createWindow, [exports]);
      for (const path of [
        join(privateDir, "secret.txt"),
        `${exports}/../private/secret.txt`,
        join(exports, "link.txt"),
        [join(exports, "report.txt"), join(privateDir, "secret.txt")],
      ]) {
        const result = await drag(path);
        log(`${JSON.stringify(path)}: ${JSON.stringify(result)}`);
        expect(result.started).toBe(false);
        expect(result.reason).toContain("outside this view's fileDragRoots");
      }

      // Inside the roots, but no mouse button is down, so no session starts:
      // one started now would drop wherever the user clicks next.
      const inside = await drag(join(exports, "report.txt"));
      log(`inside, no button down: ${JSON.stringify(inside)}`);
      expect(inside.started).toBe(false);
      expect(inside.reason).toContain("left mouse button");
      expect(win.webview.startFileDrag(join(exports, "report.txt"))).toBe(false);
    },
  }),

  defineTest({
    name: "Drag a file out of a view to Finder",
    category: "File drag (Interactive)",
    description: "A page starts a drag of a file in its view's fileDragRoots; dropping it on the Desktop copies the file there",
    instructions: [
      "A window with a card labeled \"Kitchen drag test.txt\" opens",
      "Drag the card onto the Desktop or a Finder window: a copy of the file appears there (the original stays)",
      "Then move the mouse over the window: its status should say buttons 0, not 1",
      "Delete the copy afterwards",
    ],
    interactive: true,
    timeout: 300000,
    requires: { platform: "darwin" },
    async run({ createWindow, log }) {
      const { exports } = fixture();
      const path = join(exports, "Kitchen drag test.txt");
      writeFileSync(path, "Dragged out of an Electrobun view.\n");
      const win = await createWindow({
        html: `<!doctype html><body style="font:16px system-ui;padding:24px">
          <div id="card" draggable="true" style="width:220px;padding:24px;border-radius:12px;background:#e8eefc;cursor:grab">📄 Kitchen drag test.txt</div>
          <p id="status">Drag the card to the Desktop</p>
          <script>
            const status = document.getElementById("status");
            document.getElementById("card").addEventListener("dragstart", (event) => {
              event.preventDefault();
              window.__electrobun.startFileDrag(${JSON.stringify(path)}).then((result) => {
                status.textContent = "startFileDrag: " + JSON.stringify(result);
              });
            });
            document.addEventListener("mousemove", (event) => {
              status.dataset.buttons = event.buttons;
              status.textContent = status.textContent.replace(/ · buttons \\d+$/, "") + " · buttons " + event.buttons;
            });
          </script></body>`,
        width: 420,
        height: 260,
        title: "Drag a file out",
        renderer: "native",
        fileDragRoots: [exports],
      });
      await new Promise<void>((resolve) => {
        win.webview.on("file-drag-end", (event: any) => {
          log(`file-drag-end: ${JSON.stringify(event.data.detail)}`);
          if (event.data.detail.operation === "copy") resolve();
        });
      });
      await sleep(3000);
      win.window.close();
    },
  }),
];
