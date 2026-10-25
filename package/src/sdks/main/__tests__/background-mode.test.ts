import { afterAll, afterEach, describe, expect, test } from "bun:test";
import { installFakeFFI } from "./fakeFFI";

let visible = true;
const fake = installFakeFFI({ isWindowVisible: () => visible });
const { BrowserWindow } = await import("../core/BrowserWindow");

afterAll(() => fake.uninstall());
afterEach(() => {
	delete process.env["ELECTROBUN_BACKGROUND"];
	visible = true;
	fake.calls.length = 0;
});

const methods = () => fake.calls.map((call) => call.method);

describe("background mode (ELECTROBUN_BACKGROUND=1)", () => {
	test("creating and showing a window never activates the app", () => {
		process.env["ELECTROBUN_BACKGROUND"] = "1";
		const win = new BrowserWindow({ url: null, activate: true });
		expect(fake.callsTo("createWindow").at(-1)?.["activate"]).toBe(false);
		win.show();
		expect(fake.callsTo("showWindow").at(-1)).toEqual({ winId: win.id, activate: false });
	});

	test("activate() orders a visible window front without activating, and leaves a hidden one hidden", () => {
		process.env["ELECTROBUN_BACKGROUND"] = "1";
		const win = new BrowserWindow({ url: null });
		win.activate();
		expect(methods()).not.toContain("activateWindow");
		expect(fake.callsTo("showWindow").at(-1)).toEqual({ winId: win.id, activate: false });

		fake.calls.length = 0;
		visible = false;
		win.activate();
		expect(methods()).not.toContain("activateWindow");
		expect(methods()).not.toContain("showWindow");
	});

	test("full screen is refused, leaving it is allowed", () => {
		process.env["ELECTROBUN_BACKGROUND"] = "1";
		const win = new BrowserWindow({ url: null });
		win.setFullScreen(true);
		expect(fake.callsTo("setWindowFullScreen")).toEqual([]);
		win.setFullScreen(false);
		expect(fake.callsTo("setWindowFullScreen")).toEqual([{ winId: win.id, fullScreen: false }]);
	});

	test("off by default: windows activate as before", () => {
		const win = new BrowserWindow({ url: null });
		expect(fake.callsTo("createWindow").at(-1)?.["activate"]).toBe(true);
		win.show();
		win.activate();
		win.setFullScreen(true);
		expect(fake.callsTo("showWindow").at(-1)).toEqual({ winId: win.id, activate: true });
		expect(fake.callsTo("activateWindow")).toEqual([{ winId: win.id }]);
		expect(fake.callsTo("setWindowFullScreen")).toEqual([{ winId: win.id, fullScreen: true }]);
	});
});
