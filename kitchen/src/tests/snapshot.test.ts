// Snapshot Tests - BrowserView.snapshot() returns what the view shows as a PNG

import { inflateSync } from "zlib";
import { defineTest, expect, sleep } from "../test-framework/types";

type Rgb = [number, number, number];

function centerPixel(dataUrl: string): { width: number; height: number; rgb: Rgb } {
  const prefix = "data:image/png;base64,";
  expect(dataUrl.startsWith(prefix), "snapshot is a PNG data URL").toBe(true);
  const png = Buffer.from(dataUrl.slice(prefix.length), "base64");
  expect(png.subarray(0, 8).toString("hex"), "PNG signature").toBe("89504e470d0a1a0a");

  let width = 0;
  let height = 0;
  let bpp = 0;
  const idat: Buffer[] = [];
  for (let offset = 8; offset < png.length; ) {
    const length = png.readUInt32BE(offset);
    const type = png.toString("ascii", offset + 4, offset + 8);
    const data = png.subarray(offset + 8, offset + 8 + length);
    offset += 12 + length;
    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      const bitDepth = data[8];
      const colorType = data[9];
      if (bitDepth !== 8 || (colorType !== 2 && colorType !== 6) || data[12] !== 0) {
        throw new Error(`unexpected PNG format: depth ${bitDepth}, color type ${colorType}, interlace ${data[12]}`);
      }
      bpp = colorType === 6 ? 4 : 3;
    } else if (type === "IDAT") {
      idat.push(data);
    } else if (type === "IEND") {
      break;
    }
  }

  const raw = inflateSync(Buffer.concat(idat));
  const stride = width * bpp;
  const rows = Math.floor(height / 2) + 1;
  const out = new Uint8Array(rows * stride);
  for (let y = 0; y < rows; y += 1) {
    const filter = raw[y * (stride + 1)]!;
    for (let x = 0; x < stride; x += 1) {
      const value = raw[y * (stride + 1) + 1 + x]!;
      const left = x >= bpp ? out[y * stride + x - bpp]! : 0;
      const up = y > 0 ? out[(y - 1) * stride + x]! : 0;
      const upLeft = y > 0 && x >= bpp ? out[(y - 1) * stride + x - bpp]! : 0;
      let predicted = 0;
      if (filter === 1) predicted = left;
      else if (filter === 2) predicted = up;
      else if (filter === 3) predicted = Math.floor((left + up) / 2);
      else if (filter === 4) {
        const p = left + up - upLeft;
        const pa = Math.abs(p - left);
        const pb = Math.abs(p - up);
        const pc = Math.abs(p - upLeft);
        predicted = pa <= pb && pa <= pc ? left : pb <= pc ? up : upLeft;
      }
      out[y * stride + x] = (value + predicted) & 0xff;
    }
  }
  const at = (rows - 1) * stride + Math.floor(width / 2) * bpp;
  return { width, height, rgb: [out[at]!, out[at + 1]!, out[at + 2]!] };
}

function expectMostly(rgb: Rgb, channel: 0 | 1 | 2, label: string) {
  for (const [index, value] of rgb.entries()) {
    if (index === channel) expect(value, `${label}: channel ${index} of ${rgb}`).toBeGreaterThan(180);
    else expect(value, `${label}: channel ${index} of ${rgb}`).toBeLessThan(90);
  }
}

async function paint(webview: any, color: string) {
  webview.executeJavascript(`document.body.style.background = "${color}"`);
  await sleep(400);
}

export const snapshotTests = [
  defineTest({
    name: "BrowserView.snapshot",
    category: "Snapshot",
    description: "snapshot() returns a PNG of what the view shows now, also while its window is hidden or minimized",
    requires: { platform: "darwin", renderer: "native" },
    timeout: 30000,
    async run({ createWindow, log }) {
      const win = await createWindow({
        html: '<html><body style="margin:0;height:100vh;background:rgb(255,0,0)"></body></html>',
        width: 400,
        height: 300,
        title: "Snapshot Test",
        renderer: "native",
      });
      await sleep(1000);

      let started = performance.now();
      const shown = centerPixel(await win.webview.snapshot());
      log(`visible: ${shown.width}x${shown.height} in ${Math.round(performance.now() - started)} ms, center ${shown.rgb}`);
      expect(shown.width).toBeGreaterThanOrEqual(400);
      expect(shown.height).toBeGreaterThan(0);
      expectMostly(shown.rgb, 0, "visible window");

      await paint(win.webview, "rgb(0,0,255)");
      expectMostly(centerPixel(await win.webview.snapshot()).rgb, 2, "after a repaint");

      win.window.hide();
      await sleep(300);
      await paint(win.webview, "rgb(0,255,0)");
      started = performance.now();
      const hidden = centerPixel(await win.webview.snapshot());
      log(`hidden: ${hidden.width}x${hidden.height} in ${Math.round(performance.now() - started)} ms, center ${hidden.rgb}`);
      expectMostly(hidden.rgb, 1, "hidden window");

      win.window.show();
      win.window.minimize();
      await sleep(800);
      await paint(win.webview, "rgb(255,0,0)");
      const minimized = centerPixel(await win.webview.snapshot());
      log(`minimized: center ${minimized.rgb}`);
      expectMostly(minimized.rgb, 0, "minimized window");
    },
  }),
];
