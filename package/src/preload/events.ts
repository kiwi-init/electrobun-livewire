// Shared Event Emission for webview lifecycle events
// Uses __electrobunEventBridge which is available on ALL webviews (including sandboxed)
// Falls back to __electrobunInternalBridge for backwards compatibility until native code
// is updated to include the eventBridge handler
// This is a one-way channel for emitting events to native/bun - no RPC capability

import "./globals.d.ts";

// Post a webview event to native code now.
export function postWebviewEvent(eventName: string, detail: string) {
	// Prefer eventBridge (available on all webviews), fall back to internalBridge
	// (for backwards compatibility until native code adds eventBridge handler)
	const bridge =
		window.__electrobunEventBridge || window.__electrobunInternalBridge;
	bridge?.postMessage(
		JSON.stringify({
			id: "webviewEvent",
			type: "message",
			payload: {
				id: window.__electrobunWebviewId,
				eventName,
				detail,
			},
		}),
	);
}

// Emit a webview event to native code
export function emitWebviewEvent(eventName: string, detail: string) {
	// setTimeout works around a race condition with Bun FFI
	setTimeout(() => postWebviewEvent(eventName, detail));
}

type DomReadyTarget = Pick<Window, "addEventListener" | "top" | "document">;

// dom-ready goes out once every load listener has run, ahead of the tasks
// queued behind the load event: from a microtask that pageshow queues, as
// pageshow fires right after load in the same task. A setTimeout(0) alone waits
// behind whatever the host queued meanwhile, such as a push the page then
// renders: ~150 ms in most launches of a Linux app on the JSC host, and in some
// under Bun, all with the window still hidden (T1290). The timer stays as the
// fallback for an engine whose pageshow comes later; the first to run sends it,
// once per load.
export function initDomReady(
	target: DomReadyTarget = window,
	send: (href: string) => void = (href) => postWebviewEvent("dom-ready", href),
	later: (fn: () => void) => void = (fn) => setTimeout(fn),
	soon: (fn: () => void) => void = queueMicrotask,
) {
	let sent = true;
	const once = () => {
		if (sent) return;
		sent = true;
		send(target.document.location.href);
	};
	target.addEventListener("load", () => {
		if (target !== target.top) return;
		sent = false;
		later(once);
	});
	target.addEventListener("pageshow", (event) => {
		// A page restored from the back/forward cache gets no load, so no dom-ready.
		if (target !== target.top || (event as PageTransitionEvent).persisted) return;
		soon(once);
	});
}

export function initHostMessageBridge(
	targetWindow: Window = window,
	emit: typeof emitWebviewEvent = emitWebviewEvent,
) {
	targetWindow.__electrobunSendToHost = (message: unknown) => {
		emit("host-message", JSON.stringify(message));
	};
}

// Set up standard lifecycle event listeners
export function initLifecycleEvents() {
	// Emit dom-ready when page loads (top-level window only)
	initDomReady();

	// Track in-page navigation
	window.addEventListener("popstate", () => {
		emitWebviewEvent("did-navigate-in-page", window.location.href);
	});

	window.addEventListener("hashchange", () => {
		emitWebviewEvent("did-navigate-in-page", window.location.href);
	});
}

// Track cmd key state for SPA navigation detection
let cmdKeyHeld = false;
let cmdKeyTimestamp = 0;
const CMD_KEY_THRESHOLD_MS = 500;

export function isCmdHeld(): boolean {
	if (cmdKeyHeld) return true;
	return (
		Date.now() - cmdKeyTimestamp < CMD_KEY_THRESHOLD_MS && cmdKeyTimestamp > 0
	);
}

// Set up cmd+click detection for opening links in new windows
export function initCmdClickHandling() {
	window.addEventListener(
		"keydown",
		(event) => {
			if (event.key === "Meta" || event.metaKey) {
				cmdKeyHeld = true;
				cmdKeyTimestamp = Date.now();
			}
		},
		true,
	);

	window.addEventListener(
		"keyup",
		(event) => {
			if (event.key === "Meta") {
				cmdKeyHeld = false;
				cmdKeyTimestamp = Date.now();
			}
		},
		true,
	);

	window.addEventListener("blur", () => {
		cmdKeyHeld = false;
	});

	// Intercept cmd+clicks on anchors before SPA frameworks can handle them
	window.addEventListener(
		"click",
		(event) => {
			if (event.metaKey || event.ctrlKey) {
				const anchor = (event.target as HTMLElement)?.closest?.("a");
				if (anchor && (anchor as HTMLAnchorElement).href) {
					event.preventDefault();
					event.stopPropagation();
					event.stopImmediatePropagation();
					emitWebviewEvent(
						"new-window-open",
						JSON.stringify({
							url: (anchor as HTMLAnchorElement).href,
							isCmdClick: true,
							isSPANavigation: false,
						}),
					);
				}
			}
		},
		true,
	);
}

// Intercept SPA navigation (history.pushState/replaceState) when cmd is held
export function initSPANavigationInterception() {
	const originalPushState = history.pushState;
	const originalReplaceState = history.replaceState;

	history.pushState = function (
		state: unknown,
		title: string,
		url?: string | URL | null,
	) {
		if (isCmdHeld() && url) {
			const resolvedUrl = new URL(String(url), window.location.href).href;
			emitWebviewEvent(
				"new-window-open",
				JSON.stringify({
					url: resolvedUrl,
					isCmdClick: true,
					isSPANavigation: true,
				}),
			);
			return;
		}
		return originalPushState.apply(this, [state, title, url]);
	};

	history.replaceState = function (
		state: unknown,
		title: string,
		url?: string | URL | null,
	) {
		if (isCmdHeld() && url) {
			const resolvedUrl = new URL(String(url), window.location.href).href;
			emitWebviewEvent(
				"new-window-open",
				JSON.stringify({
					url: resolvedUrl,
					isCmdClick: true,
					isSPANavigation: true,
				}),
			);
			return;
		}
		return originalReplaceState.apply(this, [state, title, url]);
	};
}

export function shouldApplyOverscrollPrevention(platform: string): boolean {
	// WebKitGTK stops scrolling the root viewport with the mouse wheel when
	// overscroll-behavior is set on html/body. Linux desktop compositors do not
	// need the bounce-prevention workaround, so leave root scrolling untouched.
	// Use the native host marker because applications can legitimately override
	// navigator.userAgent through chromiumFlags.
	return platform !== "linux";
}

// Prevent overscroll bounce effect on platforms that support it without
// disabling root viewport scrolling.
export function initOverscrollPrevention(
	targetDocument: Document = document,
	platform: string = window.__electrobunPlatform,
) {
	if (!shouldApplyOverscrollPrevention(platform)) return;

	targetDocument.addEventListener("DOMContentLoaded", () => {
		const style = targetDocument.createElement("style");
		style.type = "text/css";
		style.appendChild(
			targetDocument.createTextNode(
				"html, body { overscroll-behavior: none; }",
			),
		);
		targetDocument.head.appendChild(style);
	});
}
