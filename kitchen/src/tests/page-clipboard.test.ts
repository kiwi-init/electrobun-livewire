// Page clipboard: what a page copies with navigator.clipboard reaches other apps

import { readdirSync } from "fs";
import { join } from "path";
import { defineTest, expect, sleep } from "../test-framework/types";
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

function swayEnv(): Record<string, string> {
  const runtime = process.env["XDG_RUNTIME_DIR"] ?? "";
  let sock = process.env["SWAYSOCK"] ?? "";
  try {
    sock ||= join(runtime, readdirSync(runtime).find((f) => f.startsWith("sway-ipc.") && f.endsWith(".sock")) ?? "");
  } catch {
    // No runtime dir to look in: swaymsg reports it.
  }
  return { ...(process.env as Record<string, string>), SWAYSOCK: sock };
}

type SwayNode = { name?: string | null; focused?: boolean; type?: string; rect?: { x: number; y: number; width: number; height: number }; nodes?: SwayNode[]; floating_nodes?: SwayNode[] };

function windowCenter(title: string, log: (line: string) => void): [number, number] | null {
  const tree = Bun.spawnSync(["swaymsg", "-t", "get_tree", "-r"], { timeout: 5000, env: swayEnv() });
  if (tree.exitCode !== 0) {
    log(`swaymsg get_tree: exit ${tree.exitCode}, ${tree.stderr.toString().trim()} (SWAYSOCK=${swayEnv()["SWAYSOCK"]})`);
    return null;
  }
  const views: SwayNode[] = [];
  const walk = (n: SwayNode) => {
    if ((n.type === "con" || n.type === "floating_con") && n.rect && !(n.nodes?.length || n.floating_nodes?.length)) views.push(n);
    for (const c of [...(n.nodes ?? []), ...(n.floating_nodes ?? [])]) walk(c);
  };
  try {
    walk(JSON.parse(tree.stdout.toString()) as SwayNode);
  } catch {
    // Not sway's JSON: report it below as no window.
  }
  log(`sway views: ${JSON.stringify(views.map((v) => ({ name: v.name, focused: v.focused, rect: v.rect })))}`);
  const hit = views.find((v) => v.name === title) ?? views.find((v) => v.focused) ?? null;
  return hit?.rect ? [Math.round(hit.rect.x + hit.rect.width / 2), Math.round(hit.rect.y + hit.rect.height / 2)] : null;
}

export const pageClipboardTests = [
  defineTest({
    name: "Text and a PNG the page copies reach the system clipboard",
    category: "Utils",
    description: "A real click on the page runs navigator.clipboard.writeText, then write with an image/png ClipboardItem; wl-paste reads each",
    requires: { platform: "linux", display: "wayland", renderer: "native", foreground: true },
    timeout: 30000,
    async run({ createWindow, log }) {
      const wlPaste = Bun.which("wl-paste");
      if (!wlPaste || !Bun.which("swaymsg")) throw new Error("needs sway and wl-clipboard");
      const title = "Page Clipboard Test";
      const rpc = createTestHarnessRPC(8000);
      const win = await createWindow({ url: "views://test-harness/index.html", rpc, renderer: "native", title });
      await sleep(1500);
      win.window.focus();
      await rpc.request.evaluateJavascriptWithResponse({ script: ARM });
      const center = windowCenter(title, log);
      log(`window center: ${JSON.stringify(center)}`);
      if (!center) throw new Error("couldn't find the window in sway's tree");
      const click = async () => {
        Bun.spawnSync(["swaymsg", "seat", "-", "cursor", "set", String(center[0]), String(center[1])], { timeout: 5000, env: swayEnv() });
        Bun.spawnSync(["swaymsg", "seat", "-", "cursor", "press", "button1"], { timeout: 5000, env: swayEnv() });
        Bun.spawnSync(["swaymsg", "seat", "-", "cursor", "release", "button1"], { timeout: 5000, env: swayEnv() });
        await sleep(1200);
      };
      const text = `from the page ${Date.now()}`;
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
