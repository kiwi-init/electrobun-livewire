// The milestone 1 check for mainProcess "jsc" (#141): Livewire's unmodified
// SDK on whatever runs this main process. Exits 0 when typed RPC works both
// ways, window events arrive and closing the last window quits; 1 otherwise.
import { BrowserWindow, BrowserView } from "electrobun/bun";
import type { TestRPC } from "../shared/rpc";

const runtime = typeof (globalThis as { Bun?: { version?: string } }).Bun?.version === "string" && !String(Bun.version).includes("livewire") ? "bun" : "system JavaScriptCore";
const log = (message: string) => console.log("[jsc-sdk] " + message);
// Focus takes the foreground from whoever uses the Mac, so it's checked only
// where asked (CI): LIVEWIRE_SDK_FOCUS=1.
const checkFocus = process.env.LIVEWIRE_SDK_FOCUS === "1";
const state = { hello: false, add: false, resize: false, focus: !checkFocus, close: false };
const failTimer = setTimeout(() => {
	log("FAIL timeout " + JSON.stringify(state));
	process.exit(1);
}, 20000);

const rpc = BrowserView.defineRPC<TestRPC>({
	maxRequestTime: 5000,
	handlers: {
		requests: {
			hello: ({ name }) => {
				state.hello = true;
				log("view -> main hello(" + name + ")");
				return "hi " + name + ", from a main process on " + runtime;
			},
		},
		messages: {
			ready: () => void check(),
		},
	},
});

const win = new BrowserWindow({
	title: "Livewire SDK on " + runtime,
	frame: { x: 0, y: 0, width: 640, height: 420 },
	url: "views://main/index.html",
	rpc,
	activate: false,
});
win.on("resize", () => {
	state.resize = true;
});
win.on("focus", () => {
	state.focus = true;
});
win.on("close", () => {
	state.close = true;
	log("close event");
});

async function check() {
	const sum = await win.webview.rpc!.request.add({ a: 40, b: 2 });
	state.add = sum === 42;
	log("main -> view add(40, 2) = " + sum);
	win.setSize(700, 460);
	if (checkFocus) win.focus();
	setTimeout(async () => {
		log("checks " + JSON.stringify(state));
		if (state.hello && state.add && state.resize && state.focus) {
			clearTimeout(failTimer);
			// LIVEWIRE_SDK_HOLD_MS keeps the window open before quitting, to measure memory.
			await new Promise((resolve) => setTimeout(resolve, Number(process.env.LIVEWIRE_SDK_HOLD_MS) || 0));
			log("PASS, closing the last window");
			// Quitting is the core's job from here: exitOnLastWindowClosed.
			setTimeout(() => {
				log("FAIL still running 5 s after the last window closed");
				process.exit(1);
			}, 5000);
			win.close();
		} else {
			log("FAIL");
			process.exit(1);
		}
	}, 500);
}
