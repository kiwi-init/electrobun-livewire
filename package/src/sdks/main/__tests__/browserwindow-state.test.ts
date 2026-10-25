import { afterAll, afterEach, beforeEach, describe, expect, test } from "bun:test";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { installFakeFFI } from "./fakeFFI";

// The native side as far as these tests need it: each window's frame and
// whether it's maximized, full screen or minimized.
type NativeWindow = { frame: { x: number; y: number; width: number; height: number }; maximized: boolean; fullScreen: boolean; minimized: boolean };
const windows = new Map<number, NativeWindow>();
let nextId = 1000;
const fake = installFakeFFI({
	createWindow: ({ frame }) => {
		const id = nextId++;
		windows.set(id, { frame: { ...(frame as NativeWindow["frame"]) }, maximized: false, fullScreen: false, minimized: false });
		return id;
	},
	getWindowFrame: ({ winId }) => ({ ...windows.get(Number(winId))!.frame }),
	isWindowMaximized: ({ winId }) => windows.get(Number(winId))!.maximized,
	isWindowFullScreen: ({ winId }) => windows.get(Number(winId))!.fullScreen,
	isWindowMinimized: ({ winId }) => windows.get(Number(winId))!.minimized,
});
const { BrowserWindow } = await import("../core/BrowserWindow");
const { clampToDisplays } = await import("../core/windowState");
const { Screen } = await import("../proc/native");
const { default: electrobunEventEmitter } = await import("../events/eventEmitter");

afterAll(() => fake.uninstall());

let dir: string;
let file: string;
const getAllDisplays = Screen.getAllDisplays;
beforeEach(() => {
	dir = mkdtempSync(join(tmpdir(), "lw-window-state-"));
	file = join(dir, "window-state.json");
	fake.calls.length = 0;
});
afterEach(() => {
	Screen.getAllDisplays = getAllDisplays;
	rmSync(dir, { recursive: true, force: true });
});

const display = (x: number, y: number, width: number, height: number, isPrimary = false) => ({
	id: x,
	bounds: { x, y, width, height },
	workArea: { x, y: y + 25, width, height: height - 25 },
	scaleFactor: 2,
	isPrimary,
});

function open(extra: Record<string, unknown> = {}) {
	return new BrowserWindow({ url: null, frame: { x: 10, y: 40, width: 300, height: 200 }, windowState: { id: "main", file }, ...extra });
}

// Changes the native window, then fires the event native would.
function nativeResize(win: { id: number }, change: Partial<NativeWindow>) {
	const native = windows.get(win.id)!;
	Object.assign(native, change);
	electrobunEventEmitter.emitEvent(electrobunEventEmitter.events.window.resize({ id: win.id, ...native.frame }), win.id);
}

function nativeClose(win: { id: number }) {
	electrobunEventEmitter.emitEvent(electrobunEventEmitter.events.window.close({ id: win.id }), win.id);
}

const saved = () => JSON.parse(readFileSync(file, "utf8"))["main"];

describe("BrowserWindow windowState", () => {
	test("is off by default: nothing is read or written", () => {
		new BrowserWindow({ url: null, frame: { x: 10, y: 40, width: 300, height: 200 } });
		expect(existsSync(file)).toBe(false);
	});

	test("the first launch uses the given frame and saves it", () => {
		open();
		expect(fake.callsTo("createWindow").at(-1)?.["frame"]).toEqual({ x: 10, y: 40, width: 300, height: 200 });
		expect(saved()).toEqual({ frame: { x: 10, y: 40, width: 300, height: 200 }, maximized: false, fullScreen: false });
	});

	test("a moved and resized window reopens where it was left", () => {
		const win = open();
		nativeResize(win, { frame: { x: 200, y: 120, width: 640, height: 480 } });
		nativeClose(win);
		expect(saved().frame).toEqual({ x: 200, y: 120, width: 640, height: 480 });

		open();
		expect(fake.callsTo("createWindow").at(-1)?.["frame"]).toEqual({ x: 200, y: 120, width: 640, height: 480 });
		expect(fake.callsTo("createWindow").at(-1)?.["centered"]).toBe(false);
	});

	test("changes are saved after a short delay without a close", async () => {
		const win = open();
		nativeResize(win, { frame: { x: 50, y: 60, width: 700, height: 500 } });
		expect(saved().frame).toEqual({ x: 10, y: 40, width: 300, height: 200 });
		await Bun.sleep(700);
		expect(saved().frame).toEqual({ x: 50, y: 60, width: 700, height: 500 });
	});

	test("maximized: keeps the normal frame and maximizes on reopen", () => {
		const win = open();
		nativeResize(win, { frame: { x: 100, y: 100, width: 500, height: 400 } });
		nativeResize(win, { frame: { x: 0, y: 25, width: 1440, height: 875 }, maximized: true });
		nativeClose(win);
		expect(saved()).toEqual({ frame: { x: 100, y: 100, width: 500, height: 400 }, maximized: true, fullScreen: false });

		const reopened = open();
		expect(fake.callsTo("createWindow").at(-1)?.["frame"]).toEqual({ x: 100, y: 100, width: 500, height: 400 });
		expect(fake.callsTo("maximizeWindow")).toEqual([{ winId: reopened.id }]);
		expect(fake.callsTo("setWindowFullScreen")).toEqual([]);
	});

	test("full screen reopens full screen", () => {
		const win = open();
		nativeResize(win, { frame: { x: 0, y: 0, width: 1440, height: 900 }, fullScreen: true });
		nativeClose(win);
		expect(saved()).toEqual({ frame: { x: 10, y: 40, width: 300, height: 200 }, maximized: false, fullScreen: true });

		const reopened = open();
		expect(fake.callsTo("setWindowFullScreen")).toEqual([{ winId: reopened.id, fullScreen: true }]);
	});

	test("unmaximizing saves the normal state again", () => {
		const win = open();
		nativeResize(win, { frame: { x: 0, y: 25, width: 1440, height: 875 }, maximized: true });
		nativeResize(win, { frame: { x: 10, y: 40, width: 300, height: 200 }, maximized: false });
		nativeClose(win);
		expect(saved()).toEqual({ frame: { x: 10, y: 40, width: 300, height: 200 }, maximized: false, fullScreen: false });
	});

	test("a hidden window is maximized when it's first shown, not before", () => {
		const win = open();
		nativeResize(win, { maximized: true });
		nativeClose(win);

		const hidden = open({ hidden: true });
		expect(fake.callsTo("maximizeWindow")).toEqual([]);
		hidden.show();
		hidden.show();
		expect(fake.callsTo("maximizeWindow")).toEqual([{ winId: hidden.id }]);
	});

	test("minimizing doesn't change what's saved", () => {
		const win = open();
		nativeResize(win, { frame: { x: -32000, y: -32000, width: 160, height: 28 }, minimized: true });
		nativeClose(win);
		expect(saved()).toEqual({ frame: { x: 10, y: 40, width: 300, height: 200 }, maximized: false, fullScreen: false });
	});

	test("a window left on an unplugged monitor reopens on a connected one", () => {
		const win = open();
		nativeResize(win, { frame: { x: 2000, y: 300, width: 800, height: 600 } });
		nativeClose(win);

		// Only the built-in display is left.
		Screen.getAllDisplays = () => [display(0, 0, 1440, 900, true)];
		open();
		expect(fake.callsTo("createWindow").at(-1)?.["frame"]).toEqual({ x: 640, y: 300, width: 800, height: 600 });
	});

	test("ignores a corrupt file or entry and keeps other windows' states", () => {
		writeFileSync(file, "{nope");
		open();
		expect(fake.callsTo("createWindow").at(-1)?.["frame"]).toEqual({ x: 10, y: 40, width: 300, height: 200 });

		writeFileSync(file, JSON.stringify({ main: { frame: { x: "a" } }, other: { frame: { x: 1, y: 2, width: 300, height: 300 }, maximized: false, fullScreen: false } }));
		open();
		expect(fake.callsTo("createWindow").at(-1)?.["frame"]).toEqual({ x: 10, y: 40, width: 300, height: 200 });
		expect(JSON.parse(readFileSync(file, "utf8"))["other"].frame).toEqual({ x: 1, y: 2, width: 300, height: 300 });
	});

	test("separate ids keep separate states", () => {
		const a = new BrowserWindow({ url: null, frame: { x: 10, y: 40, width: 300, height: 200 }, windowState: { id: "a", file } });
		const b = new BrowserWindow({ url: null, frame: { x: 10, y: 40, width: 300, height: 200 }, windowState: { id: "b", file } });
		nativeResize(a, { frame: { x: 1, y: 30, width: 400, height: 300 } });
		nativeResize(b, { frame: { x: 2, y: 30, width: 500, height: 300 } });
		nativeClose(a);
		nativeClose(b);
		const states = JSON.parse(readFileSync(file, "utf8"));
		expect(states["a"].frame.width).toBe(400);
		expect(states["b"].frame.width).toBe(500);
	});
});

describe("clampToDisplays", () => {
	const primary = display(0, 0, 1440, 900, true);
	const external = display(1440, 0, 2560, 1440);

	test("keeps a frame that fits a display's work area", () => {
		const frame = { x: 1600, y: 100, width: 1200, height: 800 };
		expect(clampToDisplays(frame, [primary, external])).toEqual(frame);
	});

	test("moves an off-screen frame onto the primary display", () => {
		expect(clampToDisplays({ x: -5000, y: -5000, width: 400, height: 300 }, [external, primary])).toEqual({ x: 0, y: 25, width: 400, height: 300 });
	});

	test("pulls a frame that hangs off an edge back onto the display it overlaps most", () => {
		expect(clampToDisplays({ x: 3800, y: 1300, width: 400, height: 300 }, [primary, external])).toEqual({ x: 3600, y: 1140, width: 400, height: 300 });
	});

	test("shrinks a frame larger than the display", () => {
		expect(clampToDisplays({ x: 0, y: 25, width: 3000, height: 2000 }, [primary])).toEqual({ x: 0, y: 25, width: 1440, height: 875 });
	});

	test("keeps the title bar below the menu bar", () => {
		expect(clampToDisplays({ x: 100, y: 0, width: 400, height: 300 }, [primary])).toEqual({ x: 100, y: 25, width: 400, height: 300 });
	});

	test("keeps the frame when there's no display information", () => {
		const frame = { x: -5000, y: 0, width: 400, height: 300 };
		expect(clampToDisplays(frame, [])).toEqual(frame);
	});
});
