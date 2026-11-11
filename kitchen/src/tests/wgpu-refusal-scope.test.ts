import { describe, expect, mock, test } from "bun:test";

const reason = "WGPU surface needs an X11 window; Wayland is not supported yet";

mock.module("electrobun/main", () => ({
  GpuWindow: class {
    close() {}
  },
  WGPU: { native: {} },
  webgpu: {
    install() {},
    createContext() {
      throw new Error(reason);
    },
  },
}));

const { wgpuAdapterTests } = await import("./wgpu-adapter.test.ts");
const { wgpuAdapterExtendedTests } = await import("./wgpu-adapter-extended.test.ts");

const sixBodies = () => [wgpuAdapterTests[0], ...wgpuAdapterExtendedTests];

async function runAll(): Promise<string[]> {
  const accepted: string[] = [];
  for (const entry of sixBodies()) {
    try {
      await entry.run({ log() {} } as never);
      accepted.push(entry.name);
    } catch {}
  }
  return accepted;
}

describe("the real GPU test bodies' refusal contract (mocked surface)", () => {
  test("the named refusal on X11 (no Wayland env) fails every body", async () => {
    delete process.env.KITCHEN_HEADLESS_WAYLAND;
    delete process.env.WAYLAND_DISPLAY;
    process.env.DISPLAY = ":99";
    const accepted = await runAll();
    expect(accepted).toEqual([]);
  });

  test("the named refusal under Wayland is accepted by every body", async () => {
    process.env.KITCHEN_HEADLESS_WAYLAND = "sway";
    const accepted = await runAll();
    expect(accepted.length).toBe(sixBodies().length);
    delete process.env.KITCHEN_HEADLESS_WAYLAND;
  });

  test("an unrelated surface error under Wayland fails every body, with the mock provably reached", async () => {
    process.env.KITCHEN_HEADLESS_WAYLAND = "sway";
    let mockReached = 0;
    mock.restore();
    mock.module("electrobun/main", () => ({
      GpuWindow: class {
        close() {}
      },
      WGPU: { native: {} },
      webgpu: {
        install() {},
        createContext() {
          mockReached += 1;
          throw new Error("WGPU instance creation failed");
        },
      },
    }));
    const accepted: string[] = [];
    const propagated: string[] = [];
    for (const entry of sixBodies()) {
      try {
        await entry.run({ log() {} } as never);
        accepted.push(entry.name);
      } catch (error) {
        const text = String(error);
        if (text.includes("WGPU instance creation failed") || text.includes("WGPU surface needs an X11 window")) {
          propagated.push(entry.name);
        } else {
          propagated.push(entry.name + " (unexpected: " + text.slice(0, 60) + ")");
        }
      }
    }
    expect(mockReached).toBeGreaterThan(0);
    expect(accepted).toEqual([]);
    expect(propagated.filter((n) => n.includes("(unexpected:"))).toEqual([]);
    expect(propagated.length).toBe(sixBodies().length);
    delete process.env.KITCHEN_HEADLESS_WAYLAND;
  });
});
