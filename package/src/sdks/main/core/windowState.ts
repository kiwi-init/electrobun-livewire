// Window state restore: remembers a window's frame, and whether it was
// maximized or full screen, so it reopens the same way on the next launch.
//
// Opt-in per window with `new BrowserWindow({ windowState: "main" })`. States
// are kept in userData/window-state.json, outside what appdata:// serves.

import { mkdirSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { randomBytes } from "node:crypto";
import type { Display, Rectangle } from "../proc/native";

export type WindowStateOptions =
	| string
	| {
			// The name the state is saved under; one per kind of window.
			id: string;
			// Defaults to userData/window-state.json.
			file?: string;
	  };

export type SavedWindowState = {
	// The window's normal frame (not maximized or full screen): top-left
	// corner plus content size, the same as BrowserWindow's frame.
	frame: Rectangle;
	maximized: boolean;
	fullScreen: boolean;
};

// The smallest size a restored window is given, so a bad file can't make it
// vanish.
const MIN_WIDTH = 100;
const MIN_HEIGHT = 50;
const SAVE_DELAY_MS = 500;

// Savers with a write still waiting, flushed if the app quits first.
const pending = new Set<WindowStateSaver>();
let flushOnExit = false;

export function resolveWindowState(options: WindowStateOptions): { id: string; file: string } {
	const { id, file } = typeof options === "string" ? { id: options, file: undefined } : options;
	if (typeof id !== "string" || id.length === 0) throw new Error("windowState needs a non-empty id");
	if (file) return { id, file };
	// Required lazily: Utils needs the app's runtime, which unit tests don't have.
	const { paths } = require("./Utils") as typeof import("./Utils");
	return { id, file: join(paths.userData, "window-state.json") };
}

export function loadWindowState(file: string, id: string): SavedWindowState | null {
	return parseState(readStates(file)[id]);
}

// Batches a window's changes into one write every SAVE_DELAY_MS; `flush`
// writes right away (on close).
export class WindowStateSaver {
	private state: SavedWindowState;
	private timer: ReturnType<typeof setTimeout> | null = null;

	constructor(
		private readonly file: string,
		private readonly id: string,
		initial: SavedWindowState,
	) {
		this.state = { ...initial, frame: { ...initial.frame } };
	}

	// `frame` is the window's current frame; it only becomes the saved normal
	// frame while the window is neither maximized nor full screen, so
	// restoring after unmaximize goes back to where it was.
	update(frame: Rectangle, maximized: boolean, fullScreen: boolean) {
		const next: SavedWindowState = {
			frame: maximized || fullScreen ? this.state.frame : { x: frame.x, y: frame.y, width: frame.width, height: frame.height },
			maximized,
			fullScreen,
		};
		if (sameState(next, this.state)) return;
		this.state = next;
		if (this.timer) return;
		this.timer = setTimeout(() => this.flush(), SAVE_DELAY_MS);
		pending.add(this);
		if (!flushOnExit) {
			flushOnExit = true;
			process.on("exit", () => {
				for (const saver of pending) saver.flush();
			});
		}
	}

	flush() {
		if (this.timer) clearTimeout(this.timer);
		this.timer = null;
		pending.delete(this);
		try {
			const states = readStates(this.file);
			states[this.id] = this.state;
			writeAtomic(this.file, JSON.stringify(states, null, "\t"));
		} catch (error) {
			console.error(`[electrobun] couldn't save window state "${this.id}":`, error);
		}
	}

	get current(): SavedWindowState {
		return this.state;
	}
}

// Moves and shrinks `frame` onto a display, so a window never reopens
// off-screen, for example after the monitor it was on is unplugged. A frame
// that fits in a display's work area is kept as it is. Otherwise it goes to
// the display it overlaps most (the primary one when it overlaps none),
// shrunk to fit that display's work area if needed. With no display
// information, the frame is kept.
export function clampToDisplays(frame: Rectangle, displays: Display[]): Rectangle {
	const areas = displays.map((display) => ({ area: display.workArea, primary: display.isPrimary })).filter(({ area }) => area.width > 0 && area.height > 0);
	if (areas.length === 0) return frame;
	if (areas.some(({ area }) => contains(area, frame))) return frame;

	let best = areas.find(({ primary }) => primary) ?? areas[0]!;
	let bestOverlap = 0;
	for (const candidate of areas) {
		const overlap = overlapArea(candidate.area, frame);
		if (overlap > bestOverlap) {
			best = candidate;
			bestOverlap = overlap;
		}
	}
	const area = best.area;
	const width = Math.min(frame.width, area.width);
	const height = Math.min(frame.height, area.height);
	return {
		x: clamp(frame.x, area.x, area.x + area.width - width),
		y: clamp(frame.y, area.y, area.y + area.height - height),
		width,
		height,
	};
}

function contains(outer: Rectangle, inner: Rectangle) {
	return (
		inner.x >= outer.x &&
		inner.y >= outer.y &&
		inner.x + inner.width <= outer.x + outer.width &&
		inner.y + inner.height <= outer.y + outer.height
	);
}

function overlapArea(a: Rectangle, b: Rectangle) {
	const width = Math.min(a.x + a.width, b.x + b.width) - Math.max(a.x, b.x);
	const height = Math.min(a.y + a.height, b.y + b.height) - Math.max(a.y, b.y);
	return width > 0 && height > 0 ? width * height : 0;
}

function clamp(value: number, min: number, max: number) {
	return Math.min(Math.max(value, min), max);
}

function sameState(a: SavedWindowState, b: SavedWindowState) {
	return (
		a.maximized === b.maximized &&
		a.fullScreen === b.fullScreen &&
		a.frame.x === b.frame.x &&
		a.frame.y === b.frame.y &&
		a.frame.width === b.frame.width &&
		a.frame.height === b.frame.height
	);
}

function readStates(file: string): Record<string, unknown> {
	try {
		const value: unknown = JSON.parse(readFileSync(file, "utf8"));
		if (typeof value === "object" && value !== null && !Array.isArray(value)) {
			// A null prototype, so an id like "__proto__" is a plain key.
			return Object.assign(Object.create(null) as Record<string, unknown>, value);
		}
	} catch {
		// Missing or unreadable: start over.
	}
	return Object.create(null) as Record<string, unknown>;
}

function parseState(value: unknown): SavedWindowState | null {
	if (typeof value !== "object" || value === null) return null;
	const { frame, maximized, fullScreen } = value as Record<string, unknown> & { frame?: Record<string, unknown> };
	if (typeof frame !== "object" || frame === null) return null;
	const { x, y, width, height } = frame;
	if (![x, y, width, height].every((n) => typeof n === "number" && Number.isFinite(n))) return null;
	return {
		frame: {
			x: x as number,
			y: y as number,
			width: Math.max(MIN_WIDTH, width as number),
			height: Math.max(MIN_HEIGHT, height as number),
		},
		maximized: maximized === true,
		fullScreen: fullScreen === true,
	};
}

function writeAtomic(file: string, contents: string) {
	mkdirSync(dirname(file), { recursive: true });
	const tmp = `${file}.${randomBytes(4).toString("hex")}.tmp`;
	writeFileSync(tmp, contents);
	renameSync(tmp, file);
}
