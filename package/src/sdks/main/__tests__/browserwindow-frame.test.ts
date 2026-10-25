import { afterAll, describe, expect, test } from "bun:test";
import { installFakeFFI } from "./fakeFFI";

const nativeFrame = { x: 160, y: 140, width: 500, height: 380 };
const fake = installFakeFFI({ getWindowFrame: () => ({ ...nativeFrame }) });
const { BrowserWindow } = await import("../core/BrowserWindow");

afterAll(() => fake.uninstall());

describe("BrowserWindow frame", () => {
	test("a window recreated from getFrame() is created with that exact frame", () => {
		const first = new BrowserWindow({ url: null, frame: { x: 10, y: 20, width: 300, height: 200 } });
		const frame = first.getFrame();
		expect(frame).toEqual(nativeFrame);

		new BrowserWindow({ url: null, frame });
		expect(fake.callsTo("createWindow").at(-1)?.["frame"]).toEqual(nativeFrame);
	});

	test("setFrame and setSize hand the content size to native unchanged", () => {
		const win = new BrowserWindow({ url: null, frame: { x: 10, y: 20, width: 300, height: 200 } });
		win.setFrame(40, 50, 560, 420);
		win.setSize(520, 400);

		expect(fake.callsTo("setWindowFrame").at(-1)).toEqual({ winId: win.id, x: 40, y: 50, width: 560, height: 420 });
		expect(fake.callsTo("setWindowSize").at(-1)).toEqual({ winId: win.id, width: 520, height: 400 });
		expect(win.frame).toEqual({ x: 40, y: 50, width: 520, height: 400 });
	});
});
