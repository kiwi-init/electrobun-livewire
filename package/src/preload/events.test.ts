import { describe, expect, test } from "bun:test";
import {
	initDomReady,
	initHostMessageBridge,
	initOverscrollPrevention,
	shouldApplyOverscrollPrevention,
} from "./events";

describe("host message bridge", () => {
	test("serializes messages onto the event-only bridge", () => {
		const emitted: Array<{ eventName: string; detail: string }> = [];
		const targetWindow = {} as Window;

		initHostMessageBridge(targetWindow, (eventName, detail) => {
			emitted.push({ eventName, detail });
		});
		targetWindow.__electrobunSendToHost({ type: "ready", count: 2 });

		expect(emitted).toEqual([
			{
				eventName: "host-message",
				detail: '{"type":"ready","count":2}',
			},
		]);
	});
});

describe("dom-ready", () => {
	function page(top = true) {
		const listeners: Record<string, Array<(event: Event) => void>> = {};
		const target = {
			addEventListener(type: string, fn: (event: Event) => void) {
				(listeners[type] ??= []).push(fn);
			},
			document: { location: { href: "views://main/index.html" } },
		} as unknown as Window;
		(target as { top: unknown }).top = top ? target : {};
		const fire = (type: string, persisted = false) => {
			for (const fn of listeners[type] ?? []) fn({ type, persisted } as unknown as Event);
		};
		return { target, fire };
	}

	test("goes out from pageshow's microtask, ahead of the timer, once", () => {
		const { target, fire } = page();
		const sent: string[] = [];
		const timers: Array<() => void> = [];
		const microtasks: Array<() => void> = [];
		initDomReady(target, (href) => sent.push(href), (fn) => timers.push(fn), (fn) => microtasks.push(fn));
		fire("load");
		fire("pageshow");
		expect(sent).toEqual([]);
		for (const fn of microtasks) fn();
		expect(sent).toEqual(["views://main/index.html"]);
		for (const fn of timers) fn();
		expect(sent).toEqual(["views://main/index.html"]);
	});

	test("the timer sends it when pageshow doesn't come", () => {
		const { target, fire } = page();
		const sent: string[] = [];
		const timers: Array<() => void> = [];
		initDomReady(target, (href) => sent.push(href), (fn) => timers.push(fn), () => {});
		fire("load");
		for (const fn of timers) fn();
		expect(sent).toEqual(["views://main/index.html"]);
	});

	test("not for a page restored from the back/forward cache, nor a frame", () => {
		const restored = page();
		const frame = page(false);
		const sent: string[] = [];
		const run = (fn: () => void) => fn();
		initDomReady(restored.target, (href) => sent.push(href), run, run);
		initDomReady(frame.target, (href) => sent.push(href), run, run);
		restored.fire("pageshow", true);
		frame.fire("load");
		frame.fire("pageshow");
		expect(sent).toEqual([]);
	});

	test("again for the next load", () => {
		const { target, fire } = page();
		const sent: string[] = [];
		const run = (fn: () => void) => fn();
		initDomReady(target, (href) => sent.push(href), () => {}, run);
		fire("load");
		fire("pageshow");
		fire("load");
		fire("pageshow");
		expect(sent).toHaveLength(2);
	});
});

describe("overscroll prevention", () => {
	test("does not install the root overscroll rule on Linux", () => {
		let listenerRegistered = false;
		const targetDocument = {
			addEventListener() {
				listenerRegistered = true;
			},
		} as unknown as Document;

		initOverscrollPrevention(targetDocument, "linux");

		expect(listenerRegistered).toBe(false);
	});

	test("uses the native platform marker rather than a spoofable user agent", () => {
		expect(shouldApplyOverscrollPrevention("macos")).toBe(true);
		expect(shouldApplyOverscrollPrevention("windows")).toBe(true);
		expect(shouldApplyOverscrollPrevention("linux")).toBe(false);

		// A supported Linux chromiumFlags configuration may spoof a macOS UA.
		// The host marker remains Linux, so the root-scrolling fix still applies.
		const spoofedUserAgent =
			"Mozilla/5.0 (Macintosh; Intel Mac OS X 14_0) AppleWebKit/537.36";
		expect(spoofedUserAgent).toContain("Macintosh");
		expect(shouldApplyOverscrollPrevention("linux")).toBe(false);
	});
});
