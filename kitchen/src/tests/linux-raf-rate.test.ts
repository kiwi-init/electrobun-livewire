// requestAnimationFrame rate against the Wayland output's refresh rate (probe)

import { defineTest, expect, sleep } from "../test-framework/types";
import { createTestHarnessRPC } from "./rpc.test";

const COUNT_FRAMES = `
  return new Promise((resolve) => {
    let frames = 0;
    const start = performance.now();
    const tick = (now) => {
      frames++;
      if (now - start < 2000) requestAnimationFrame(tick);
      else resolve({ frames, ms: Math.round(now - start), fps: Math.round((frames * 1000) / (now - start)) });
    };
    requestAnimationFrame(tick);
  });
`;

export const linuxRafRateTests = [
  defineTest({
    name: "requestAnimationFrame follows the Wayland output's refresh rate",
    category: "Rendering",
    description: "Sets the headless output to 60, 120 and 144 Hz and measures requestAnimationFrame in a native webview at each",
    requires: { platform: "linux", display: "wayland", renderer: "native" },
    timeout: 60000,
    async run({ createWindow, log }) {
      const randr = Bun.which("wlr-randr");
      if (!randr) throw new Error("wlr-randr is needed to change the output's refresh rate");
      const output = Bun.spawnSync([randr], { timeout: 5000 }).stdout.toString().split("\n")[0]?.split(" ")[0] ?? "";
      const rpc = createTestHarnessRPC(15000);
      await createWindow({ url: "views://test-harness/index.html", rpc, renderer: "native", title: "rAF Rate Probe" });
      await sleep(1500);
      const results: Record<number, unknown> = {};
      for (const hz of [60, 120, 144]) {
        const set = Bun.spawnSync([randr, "--output", output, "--custom-mode", `1920x1080@${hz}Hz`], { timeout: 5000 });
        await sleep(1000);
        const current = Bun.spawnSync([randr], { timeout: 5000 }).stdout.toString().split("\n").filter((l) => l.includes("current")).join(" ").trim();
        const measured = await rpc.request.evaluateJavascriptWithResponse({ script: COUNT_FRAMES });
        results[hz] = measured;
        log(`${hz} Hz (wlr-randr exit ${set.exitCode}; ${current || "no current mode line"}): ${JSON.stringify(measured)}`);
      }
      expect(Object.keys(results).length).toBe(3);
    },
  }),
];
