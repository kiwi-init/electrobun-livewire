import { afterAll, describe, expect, test } from "bun:test";
import { installFakeFFI } from "./fakeFFI";

const fake = installFakeFFI({ setWindowVibrancy: ({ material }) => material !== "bogus" });
const { BrowserWindow } = await import("../core/BrowserWindow");

afterAll(() => fake.uninstall());

describe("BrowserWindow vibrancy", () => {
	test("is off by default", () => {
		new BrowserWindow({ url: null });
		expect(fake.callsTo("setWindowVibrancy")).toEqual([]);
	});

	test("a material name uses behind-window blending that follows the window's active state", () => {
		const win = new BrowserWindow({ url: null, vibrancy: "sidebar" });
		expect(fake.callsTo("setWindowVibrancy").at(-1)).toEqual({ winId: win.id, material: "sidebar", blending: "behind-window", state: "follow-window" });
	});

	test("blending and state can be set", () => {
		const win = new BrowserWindow({ url: null, vibrancy: { material: "hud", blending: "within-window", state: "active" } });
		expect(fake.callsTo("setWindowVibrancy").at(-1)).toEqual({ winId: win.id, material: "hud", blending: "within-window", state: "active" });
	});

	test("setVibrancy(null) removes it, and reports what native returns", () => {
		const win = new BrowserWindow({ url: null });
		expect(win.setVibrancy(null)).toBe(true);
		expect(fake.callsTo("setWindowVibrancy").at(-1)).toEqual({ winId: win.id, material: "none", blending: "behind-window", state: "follow-window" });
		expect(win.setVibrancy("bogus" as never)).toBe(false);
	});
});
