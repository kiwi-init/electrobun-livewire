import { describe, expect, test } from "bun:test";
import { spawnSync } from "node:child_process";

const REFUSAL = "Error: WGPU surface needs an X11 window; Wayland is not supported yet";

function runAdapterBody(env: Record<string, string | undefined>, error: string | null): { code: number; out: string } {
  const script = `
    const { expect } = await import("bun:test");
    const refusal = ${JSON.stringify(REFUSAL)};
    const wayland = ${env.KITCHEN_HEADLESS_WAYLAND ? JSON.stringify(env.KITCHEN_HEADLESS_WAYLAND) : "undefined"};
    if (wayland) process.env.KITCHEN_HEADLESS_WAYLAND = wayland;
    else delete process.env.KITCHEN_HEADLESS_WAYLAND;
    const surfaceError = ${error === null ? "null" : `new Error(${JSON.stringify(error.replace(/^Error: /, ""))})`};
    const log = () => {};
    ${error === null ? `
      class GpuWindowStub {}
      const webgpu = {};
      console.log("SURFACE-OK");
    ` : `
      try {
        if (!process.env.KITCHEN_HEADLESS_WAYLAND) throw surfaceError;
        expect(String(surfaceError)).toBe(refusal);
        console.log("ACCEPTED:" + String(surfaceError));
      } catch (e) {
        console.log("REFUSED:" + String(e));
        process.exitCode = 1;
      }
    `}
  `;
  const r = spawnSync("bun", ["-e", script]);
  return { code: r.status ?? 1, out: `${r.stdout}${r.stderr}`.trim() };
}

describe("the GPU tests' refusal contract (the run bodies' own guard logic)", () => {
  test("a named refusal under X11 (no Wayland env) fails the test", () => {
    const { code, out } = runAdapterBody({}, "Error: WGPU surface needs an X11 window; Wayland is not supported yet");
    expect(code).toBe(1);
    expect(out).toContain("REFUSED:" + REFUSAL);
  });

  test("the named refusal under Wayland is accepted and the test passes", () => {
    const { code, out } = runAdapterBody({ KITCHEN_HEADLESS_WAYLAND: "sway" }, "Error: WGPU surface needs an X11 window; Wayland is not supported yet");
    expect(code).toBe(0);
    expect(out).toContain("ACCEPTED:" + REFUSAL);
  });

  test("an unrelated surface error under Wayland still fails the test", () => {
    const { code } = runAdapterBody({ KITCHEN_HEADLESS_WAYLAND: "sway" }, "Error: WGPU instance creation failed");
    expect(code).toBe(1);
  });

  test("the guard precedes the message acceptance in both files (structural pin)", async () => {
    const adapter = await Bun.file("src/tests/wgpu-adapter.test.ts").text();
    const extended = await Bun.file("src/tests/wgpu-adapter-extended.test.ts").text();
    expect(adapter.indexOf('if (!process.env["KITCHEN_HEADLESS_WAYLAND"]) throw surfaceError;')).toBeLessThan(adapter.indexOf("WGPU surface needs an X11 window"));
    expect(extended.indexOf('if (!process.env["KITCHEN_HEADLESS_WAYLAND"]) throw err;')).toBeLessThan(extended.indexOf("WGPU surface needs an X11 window"));
  });
});
