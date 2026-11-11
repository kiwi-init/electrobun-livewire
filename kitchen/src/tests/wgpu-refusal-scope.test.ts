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

  test("an unrelated surface error under Wayland fails every body", async () => {
    process.env.KITCHEN_HEADLESS_WAYLAND = "sway";
    mock.restore();
    mock.module("electrobun/main", () => ({
      GpuWindow: class {
        close() {}
      },
      WGPU: { native: {} },
      webgpu: {
        install() {},
        createContext() {
          throw new Error("WGPU instance creation failed");
        },
      },
    }));
    const accepted: string[] = [];
    for (const entry of sixBodies()) {
      try {
        await entry.run({ log() {} } as never);
        accepted.push(entry.name);
      } catch {}
    }
    expect(accepted).toEqual([]);
    delete process.env.KITCHEN_HEADLESS_WAYLAND;
  });
});
