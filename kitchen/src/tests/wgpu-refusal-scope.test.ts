import { describe, expect, test } from "bun:test";
import { readFileSync } from "node:fs";

const adapter = readFileSync("kitchen/src/tests/wgpu-adapter.test.ts", "utf8");
const extended = readFileSync("kitchen/src/tests/wgpu-adapter-extended.test.ts", "utf8");

describe("the wgpu tests' refusal acceptance is Wayland-only", () => {
  test("the adapter test rethrows the surface error when the Wayland env is absent", () => {
    expect(adapter).toContain('if (!process.env["KITCHEN_HEADLESS_WAYLAND"]) throw surfaceError;');
    expect(adapter.indexOf("KITCHEN_HEADLESS_WAYLAND")).toBeLessThan(adapter.indexOf("WGPU surface needs an X11 window"));
  });

  test("the extended helper rethrows when the Wayland env is absent", () => {
    expect(extended).toContain("if (!process.env[\"KITCHEN_HEADLESS_WAYLAND\"]) throw err;");
    expect(extended.indexOf("KITCHEN_HEADLESS_WAYLAND")).toBeLessThan(extended.indexOf("WGPU surface needs an X11 window"));
  });

  test("the native error symbol is declared only for Linux, where it is exported", () => {
    const native = readFileSync("package/src/sdks/main/proc/native.ts", "utf8");
    expect(native).toContain('...(process.platform === "linux"\n\t\t\t\t? {\n\t\t\t\t\twgpuGetLastSurfaceError');
  });
});
