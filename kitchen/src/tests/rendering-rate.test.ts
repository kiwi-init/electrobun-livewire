// Rendering rate - a native webview's requestAnimationFrame keeps up with the display

import { defineTest, expect, sleep } from "../test-framework/types";
import { createTestHarnessRPC } from "./rpc.test";

// Frames per second over one second of requestAnimationFrame, after a short warm-up.
const RAF_PROBE = `
  return new Promise((resolve) => {
    let start = 0;
    let frames = 0;
    let warm = 10;
    const tick = (t) => {
      if (warm > 0) {
        warm--;
        requestAnimationFrame(tick);
        return;
      }
      if (!start) start = t;
      else frames++;
      if (t - start < 1000) requestAnimationFrame(tick);
      else resolve(Math.round((frames * 1000) / (t - start)));
    };
    requestAnimationFrame(tick);
  });
`;

// The main display's refresh rate, from System Information ("1920 x 1080 @ 75.00Hz").
// 60 when it doesn't say, as on some virtual displays.
function mainDisplayHz(): number {
  const result = Bun.spawnSync(["system_profiler", "SPDisplaysDataType", "-json"]);
  try {
    const gpus = JSON.parse(result.stdout.toString()).SPDisplaysDataType ?? [];
    for (const gpu of gpus) {
      for (const display of gpu.spdisplays_ndrvs ?? []) {
        if (display.spdisplays_main !== "spdisplays_yes") continue;
        const hz = /@\s*([\d.]+)\s*Hz/i.exec(display._spdisplays_resolution ?? display.spdisplays_resolution ?? "");
        if (hz) return Number(hz[1]);
      }
    }
  } catch {
    // No readable report: assume the common 60 Hz.
  }
  return 60;
}

export const renderingRateTests = [
  defineTest({
    name: "requestAnimationFrame keeps up with the display",
    category: "Rendering",
    description: "A native webview renders at the main display's refresh rate, above 60 fps on a 120 Hz display (WebKit's near-60fps pacing is off)",
    timeout: 30000,
    requires: { platform: ["darwin"], renderer: "native" },
    async run({ createWindow, log }) {
      const rpc = createTestHarnessRPC(5000);
      await createWindow({ url: "views://test-harness/index.html", rpc, renderer: "native", title: "Rendering rate" });
      await sleep(1500);
      const hz = mainDisplayHz();
      const fps = await rpc.request.evaluateJavascriptWithResponse({ script: RAF_PROBE });
      log(`main display ${hz} Hz, requestAnimationFrame ${fps} fps`);
      // Above 60 Hz the page must beat WebKit's near-60 pacing; a 60 Hz (or virtual) display only shows frames keep coming.
      expect(fps).toBeGreaterThan(hz > 61 ? Math.max(61, Math.min(hz, 240) * 0.85) : 20);
    },
  }),
];
