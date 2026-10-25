// Screen API Tests

import { defineTest, expect } from "../test-framework/types";
import { Screen } from "electrobun/main";

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

// A 200x200 page in four colors, filling a window with no title bar.
const QUADRANTS = `<!doctype html><html><body style="margin:0;display:grid;grid-template:1fr 1fr/1fr 1fr;width:100vw;height:100vh">
<div style="background:rgb(255,0,0)"></div><div style="background:rgb(0,255,0)"></div>
<div style="background:rgb(0,0,255)"></div><div style="background:rgb(255,255,0)"></div></body></html>`;

function pixelAt(pixels: Uint8Array, width: number, x: number, y: number) {
  const offset = (y * width + x) * 4;
  return [pixels[offset]!, pixels[offset + 1]!, pixels[offset + 2]!, pixels[offset + 3]!];
}

// Display color management can move a channel by a few steps either way.
function expectColor(actual: number[], expected: number[], what: string) {
  const off = expected.some((channel, i) => Math.abs(channel - actual[i]!) > 12);
  if (off) throw new Error(`${what}: expected about rgb(${expected.join(",")}), got rgba(${actual.join(",")})`);
}

export const screenTests = [
  defineTest({
    name: "getPrimaryDisplay",
    category: "Screen",
    description: "Test getting primary display information",
    async run({ log }) {
      const primary = Screen.getPrimaryDisplay();

      expect(primary).toBeTruthy();
      expect(primary.bounds).toBeTruthy();
      expect(primary.bounds.width).toBeGreaterThan(0);
      expect(primary.bounds.height).toBeGreaterThan(0);
      expect(primary.isPrimary).toBe(true);

      log(`Primary display: ${primary.bounds.width}x${primary.bounds.height}`);
      log(`Scale factor: ${primary.scaleFactor}x`);
      log(`Work area: ${primary.workArea.width}x${primary.workArea.height}`);
    },
  }),

  defineTest({
    name: "getAllDisplays",
    category: "Screen",
    description: "Test getting all connected displays",
    async run({ log }) {
      const displays = Screen.getAllDisplays();

      expect(Array.isArray(displays)).toBe(true);
      expect(displays.length).toBeGreaterThanOrEqual(1);

      // At least one should be primary
      const primaryCount = displays.filter((d) => d.isPrimary).length;
      expect(primaryCount).toBe(1);

      log(`Found ${displays.length} display(s)`);
      displays.forEach((display, i) => {
        log(
          `  Display ${i}: ${display.bounds.width}x${display.bounds.height} @ (${display.bounds.x}, ${display.bounds.y})`
        );
      });
    },
  }),

  defineTest({
    name: "getCursorScreenPoint",
    category: "Screen",
    description: "Test getting cursor position",
    async run({ log }) {
      const point = Screen.getCursorScreenPoint();

      expect(point).toBeTruthy();
      expect(typeof point.x).toBe("number");
      expect(typeof point.y).toBe("number");

      log(`Cursor position: (${point.x}, ${point.y})`);
    },
  }),

  defineTest({
    name: "captureRegion",
    category: "Screen",
    description: "Capture a small screen region as row-major RGBA pixels",
    async run({ log }) {
      const primary = Screen.getPrimaryDisplay();
      const width = 2;
      const height = 2;
      const pixels = Screen.captureRegion({
        x: Math.floor(primary.bounds.x + primary.bounds.width / 2) - 1,
        y: Math.floor(primary.bounds.y + primary.bounds.height / 2) - 1,
        width,
        height,
      });

      if (pixels === null) {
        if (process.platform === "win32") {
          throw new Error("captureRegion unexpectedly returned null on Windows");
        }
        if (process.platform === "darwin" && Screen.hasCaptureAccess()) {
          throw new Error("captureRegion returned null although Screen Recording is granted");
        }
        log(
          "Screen capture is unavailable (permission not granted or unsupported display backend)",
        );
        return;
      }

      expect(pixels).toBeInstanceOf(Uint8Array);
      expect(pixels.length).toBe(width * height * 4);
      for (let offset = 3; offset < pixels.length; offset += 4) {
        expect(pixels[offset]).toBe(255);
      }
      expect(Screen.hasCaptureAccess()).toBe(true);
      log(`Captured ${width}x${height} RGBA pixels`);
    },
  }),

  defineTest({
    name: "captureRegion returns the screen's pixels top row first, one per point",
    category: "Screen",
    description: "A window painted in four colors is captured at its screen position: each quadrant has its color, so rows aren't flipped or scaled",
    requires: { platform: "darwin" },
    timeout: 20000,
    async run({ createWindow, log }) {
      const win = await createWindow({ html: QUADRANTS, width: 200, height: 200, x: 300, y: 300, titleBarStyle: "hidden", renderer: "native" });
      win.window.setAlwaysOnTop(true);
      await sleep(1000);
      const frame = win.window.getFrame();
      log(`window frame: ${JSON.stringify(frame)}`);
      const started = performance.now();
      const pixels = Screen.captureRegion({ x: frame.x, y: frame.y, width: 200, height: 200 });
      log(`captured in ${Math.round(performance.now() - started)} ms`);
      if (!pixels) throw new Error("captureRegion returned null: is Screen Recording granted?");
      expectColor(pixelAt(pixels, 200, 50, 50), [255, 0, 0], "top left");
      expectColor(pixelAt(pixels, 200, 150, 50), [0, 255, 0], "top right");
      expectColor(pixelAt(pixels, 200, 50, 150), [0, 0, 255], "bottom left");
      expectColor(pixelAt(pixels, 200, 150, 150), [255, 255, 0], "bottom right");
    },
  }),

  defineTest({
    name: "captureRegion fills the part of a region off every display with black",
    category: "Screen",
    description: "A region hanging off the left edge of the desktop is still captured; its off-screen columns are opaque black",
    requires: { platform: "darwin" },
    timeout: 20000,
    async run({ createWindow, log }) {
      const left = Math.min(...Screen.getAllDisplays().map((display) => display.bounds.x));
      const top = Screen.getAllDisplays().find((display) => display.bounds.x === left)!.bounds.y;
      const win = await createWindow({ html: QUADRANTS, width: 200, height: 200, x: left, y: top + 300, titleBarStyle: "hidden", renderer: "native" });
      win.window.setAlwaysOnTop(true);
      await sleep(1000);
      const frame = win.window.getFrame();
      log(`window frame: ${JSON.stringify(frame)}, desktop's left edge ${left}`);
      // 20 points left of the desktop, then 40 points of the window's red quadrant.
      const pixels = Screen.captureRegion({ x: frame.x - 20, y: frame.y + 40, width: 60, height: 20 });
      if (!pixels) throw new Error("captureRegion returned null for a region partly off the desktop");
      expectColor(pixelAt(pixels, 60, 5, 10), [0, 0, 0], "off the desktop");
      expect(pixelAt(pixels, 60, 5, 10)[3]).toBe(255);
      expectColor(pixelAt(pixels, 60, 40, 10), [255, 0, 0], "on the window");
    },
  }),

  defineTest({
    name: "Display bounds vs workArea",
    category: "Screen",
    description: "Test that workArea is within or equal to bounds",
    async run({ log }) {
      const primary = Screen.getPrimaryDisplay();

      // Work area should be <= bounds (accounts for dock, menu bar)
      expect(primary.workArea.width).toBeLessThanOrEqual(primary.bounds.width);
      expect(primary.workArea.height).toBeLessThanOrEqual(primary.bounds.height);

      log(`Bounds: ${primary.bounds.width}x${primary.bounds.height}`);
      log(`Work area: ${primary.workArea.width}x${primary.workArea.height}`);

      const dockSpace = primary.bounds.height - primary.workArea.height;
      log(`Space taken by dock/menubar: ${dockSpace}px vertical`);
    },
  }),
];
