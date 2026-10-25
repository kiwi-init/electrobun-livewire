import { ffi, Screen } from "../proc/native";
import electrobunEventEmitter from "../events/eventEmitter";
import { BrowserView } from "./BrowserView";
import type { NewWindowPolicy } from "./newWindowPolicy";
import { type Pointer } from "bun:ffi";
import { BuildConfig } from "./BuildConfig";
import { type RPCWithTransport } from "../../../shared/rpc.js";
import { WGPUView } from "./WGPUView";
import {
	clampToDisplays,
	loadWindowState,
	resolveWindowState,
	WindowStateSaver,
	type WindowStateOptions,
} from "./windowState";

const buildConfig = BuildConfig.getSync();
ffi.request.setExitOnLastWindowClosed({
	enabled: buildConfig.runtime?.exitOnLastWindowClosed ?? true,
});

export type WindowOptionsType<T = undefined> = {
	trafficLightOffset?: {
		x: number;
		y: number;
	};
	activate?: boolean;
	title: string;
	frame: {
		x?: number;
		y?: number;
		width: number;
		height: number;
	};
	url: string | null;
	html: string | null;
	preload: string | null;
	viewsRoot: string | null;
	allowedProtocols: {
		views?: boolean;
		appData?: boolean;
	};
	renderer: "native" | "cef";
	rpc?: T;
	styleMask?: {};
	// titleBarStyle options:
	// - 'default': normal titlebar with native window controls
	// - 'hidden': no titlebar, no native window controls (for fully custom chrome)
	// - 'hiddenInset': inset native controls on macOS; resize handles without
	//   a system titlebar on Windows and Linux's native GTK backend
	titleBarStyle: "hidden" | "hiddenInset" | "default";
	// transparent: when true, window background is transparent (see-through)
	transparent: boolean;
	// passthrough: when true, mouse events pass through transparent regions
	passthrough: boolean;
	// Enables macOS system spell checking for native WKWebView content.
	spellCheck: boolean;
	hidden?: boolean;
	navigationRules: string | null;
	// See BrowserViewOptions.newWindow.
	newWindow: NewWindowPolicy;
	// Pages of the window's view that get RPC; see BrowserViewOptions.
	bridgeOrigins: string[] | null;
	// See BrowserViewOptions.contentSecurityPolicy.
	contentSecurityPolicy: string | null;
	// See BrowserViewOptions.fileDragRoots.
	fileDragRoots: string[];
	// Sandbox mode: when true, disables RPC and only allows event emission
	// Use for untrusted content (remote URLs) to prevent malicious sites from
	// accessing internal APIs, creating OOPIFs, or communicating with Bun
	sandbox: boolean;
	// Remember this window's frame, maximized and full-screen state under this
	// name, and reopen it that way on the next launch (moved onto a visible
	// display if needed). `frame` is then only used the first time. Off by
	// default. See windowState.ts.
	windowState?: WindowStateOptions | null;
	// macOS: show a system material (NSVisualEffectView) behind the window's
	// webview, as in Finder's sidebar. The page needs a transparent
	// background to show it; with the CEF renderer the window also needs
	// `transparent: true`. Ignored on other platforms.
	vibrancy?: WindowVibrancy | null;
};

export type VibrancyMaterial =
	| "titlebar"
	| "selection"
	| "menu"
	| "popover"
	| "sidebar"
	| "header-view"
	| "sheet"
	| "window-background"
	| "hud"
	| "fullscreen-ui"
	| "tooltip"
	| "content-background"
	| "under-window"
	| "under-page";

export type WindowVibrancy =
	| VibrancyMaterial
	| {
			material: VibrancyMaterial;
			// What the material blurs: what's behind the window (the default),
			// or the window's own content below it.
			blending?: "behind-window" | "within-window";
			// "follow-window" (the default) goes flat while the window is
			// inactive, like system apps.
			state?: "follow-window" | "active" | "inactive";
	  };

const defaultOptions: WindowOptionsType = {
	title: "Electrobun",
	frame: {
		width: 800,
		height: 600,
	},
	url: "https://electrobun.dev",
	html: null,
	preload: null,
	viewsRoot: null,
	allowedProtocols: { views: true, appData: false },
	renderer: buildConfig.defaultRenderer,
	titleBarStyle: "default",
	transparent: false,
	passthrough: false,
	spellCheck: false,
	hidden: false,
	navigationRules: null,
	newWindow: "deny",
	bridgeOrigins: null,
	contentSecurityPolicy: null,
	fileDragRoots: [],
	sandbox: false,
};

export const BrowserWindowMap: {
	[id: number]: BrowserWindow<RPCWithTransport>;
} = {};

// Clean up JS wrapper state when a window closes. Native child cleanup is core-owned.
electrobunEventEmitter.on("close", (event: { data: { id: number } }) => {
	const windowId = event.data.id;
	delete BrowserWindowMap[windowId];

	// Clean up all webviews associated with this window
	for (const view of BrowserView.getAll()) {
		if (view.windowId === windowId) {
			view.remove();
		}
	}

	// Clean up all WGPU views associated with this window
	const wgpuViews = WGPUView.getAll().filter(v => v.windowId === windowId);
	for (const view of wgpuViews) {
		try {
			view.remove();
		} catch (e) {
			console.error(`Error cleaning up WGPU view ${view.id}:`, e);
		}
	}

});

export class BrowserWindow<T extends RPCWithTransport = RPCWithTransport> {
	id = 0;
	title: string = "Electrobun";
	state: "creating" | "created" = "creating";
	url: string | null = null;
	html: string | null = null;
	preload: string | null = null;
	viewsRoot: string | null = null;
	allowedProtocols: { views: boolean; appData: boolean } = {
		views: true,
		appData: false,
	};
	renderer: "native" | "cef" = "native";
	transparent: boolean = false;
	passthrough: boolean = false;
	spellCheck: boolean = false;
	hidden: boolean = false;
	trafficLightOffset: { x: number; y: number } = { x: 0, y: 0 };
	navigationRules: string | null = null;
	newWindow: NewWindowPolicy = "deny";
	bridgeOrigins: string[] | null = null;
	contentSecurityPolicy: string | null = null;
	fileDragRoots: string[] = [];
	// Sandbox mode disables RPC and only allows event emission (for untrusted content)
	sandbox: boolean = false;
	frame: {
		x: number;
		y: number;
		width: number;
		height: number;
	} = {
		x: 0,
		y: 0,
		width: 800,
		height: 600,
	};
	// todo (yoav): make this an array of ids or something
	webviewId!: number;
	private stateSaver: WindowStateSaver | null = null;
	// Saved maximized/full-screen state to apply once a hidden window is shown.
	private pendingRestore: { maximized: boolean; fullScreen: boolean } | null = null;

	get ptr(): Pointer | null {
		return ffi.request.getWindowPointer({ winId: this.id }) as Pointer | null;
	}

	constructor(options: Partial<WindowOptionsType<T>> = defaultOptions) {
		this.title = options.title || "New Window";
		const stateLocation = options.windowState ? resolveWindowState(options.windowState) : null;
		const saved = stateLocation ? loadWindowState(stateLocation.file, stateLocation.id) : null;
		if (saved) {
			options = { ...options, frame: clampToDisplays(saved.frame, Screen.getAllDisplays()) };
		}
		const centered = options.frame?.x === undefined || options.frame?.y === undefined;
		this.frame = options.frame
			? {
				x: options.frame.x ?? 0,
				y: options.frame.y ?? 0,
				width: options.frame.width,
				height: options.frame.height,
			}
			: { x: 0, y: 0, width: defaultOptions.frame.width, height: defaultOptions.frame.height };
		this.url = options.url || null;
		this.html = options.html || null;
		this.preload = options.preload || null;
		this.viewsRoot = options.viewsRoot || null;
		this.allowedProtocols = {
			views: options.allowedProtocols?.views ?? true,
			appData: options.allowedProtocols?.appData ?? false,
		};
		this.renderer = options.renderer || defaultOptions.renderer;
		this.transparent = options.transparent ?? false;
		this.passthrough = options.passthrough ?? false;
		this.spellCheck = options.spellCheck ?? false;
		this.hidden = options.hidden ?? false;
		this.trafficLightOffset = {
			x: options.trafficLightOffset?.x ?? 0,
			y: options.trafficLightOffset?.y ?? 0,
		};
		this.navigationRules = options.navigationRules || null;
		this.newWindow = options.newWindow ?? "deny";
		this.bridgeOrigins = options.bridgeOrigins ?? null;
		this.contentSecurityPolicy = options.contentSecurityPolicy ?? null;
		this.fileDragRoots = options.fileDragRoots ?? [];
		this.sandbox = options.sandbox ?? false;

		this.init(options, centered);
		if (options.vibrancy) this.setVibrancy(options.vibrancy);

		if (stateLocation) {
			this.stateSaver = new WindowStateSaver(stateLocation.file, stateLocation.id, {
				frame: { ...this.frame },
				maximized: saved?.maximized ?? false,
				fullScreen: saved?.fullScreen ?? false,
			});
			const save = () => this.saveState();
			this.on("resize", save);
			this.on("move", save);
			this.on("close", () => this.stateSaver?.flush());
			if (saved && (saved.maximized || saved.fullScreen)) {
				this.pendingRestore = { maximized: saved.maximized, fullScreen: saved.fullScreen };
				// Maximizing or entering full screen would show a hidden window.
				if (!this.hidden) this.applyPendingRestore();
			}
			// Saved now so a window that's never moved still reopens here.
			this.stateSaver.flush();
		}
	}

	private saveState() {
		if (!this.stateSaver || this.pendingRestore) return;
		// Minimizing keeps the frame and the state the window had.
		if (this.isMinimized()) return;
		this.stateSaver.update(this.getFrame(), this.isMaximized(), this.isFullScreen());
	}

	private applyPendingRestore() {
		const restore = this.pendingRestore;
		if (!restore) return;
		this.pendingRestore = null;
		if (restore.fullScreen) this.setFullScreen(true);
		else if (restore.maximized) this.maximize();
	}

	init(
		{
			rpc,
			styleMask,
			titleBarStyle,
			transparent,
			hidden,
			activate,
		}: Partial<WindowOptionsType<T>>,
		centered: boolean,
	) {
		const windowId = ffi.request.createWindow({
			title: this.title,
			url: this.url || "",
			frame: {
				width: this.frame.width,
				height: this.frame.height,
				x: this.frame.x,
				y: this.frame.y,
			},
			styleMask: {
				Borderless: false,
				Titled: true,
				Closable: true,
				Miniaturizable: true,
				Resizable: true,
				UnifiedTitleAndToolbar: false,
				FullScreen: false,
				FullSizeContentView: false,
				UtilityWindow: false,
				DocModalWindow: false,
				NonactivatingPanel: false,
				HUDWindow: false,
				...(styleMask || {}),
				// hiddenInset: transparent titlebar with inset native controls
				...(titleBarStyle === "hiddenInset"
					? {
							Titled: true,
							FullSizeContentView: true,
						}
					: {}),
				// hidden: no titlebar, no native controls (for fully custom chrome)
				...(titleBarStyle === "hidden"
					? {
							Titled: false,
							FullSizeContentView: true,
						}
					: {}),
			},
			titleBarStyle: titleBarStyle || "default",
			transparent: transparent ?? false,
			hidden: hidden ?? false,
			activate: activate ?? true,
			centered,
			trafficLightOffset: this.trafficLightOffset,
		});

		if (!windowId) {
			throw "Failed to create window";
		}

		this.id = windowId as number;
		if (centered) {
			this.frame = ffi.request.getWindowFrame({ winId: this.id });
		}

		BrowserWindowMap[this.id] = this;

		// todo (yoav): user should be able to override this and pass in their
		// own webview instance, or instances for attaching to the window.
		const webview = new BrowserView({
			// TODO: decide whether we want to keep sending url/html
			// here, if we're manually calling loadURL/loadHTML below
			// then we can remove it from the api here
			url: this.url,
			html: this.html,
			preload: this.preload,
			viewsRoot: this.viewsRoot,
			allowedProtocols: this.allowedProtocols,
			// frame: this.frame,
			renderer: this.renderer,
			frame: {
				x: 0,
				y: 0,
				width: this.frame.width,
				height: this.frame.height,
			},
			rpc,
			// todo: we need to send the window here and attach it in one go
			// then the view creation code in objc can toggle between offscreen
			// or on screen views depending on if windowId is null
			// does this mean browserView needs to track the windowId or handle it ephemerally?
			windowId: this.id,
			navigationRules: this.navigationRules,
			newWindow: this.newWindow,
			bridgeOrigins: this.bridgeOrigins,
			contentSecurityPolicy: this.contentSecurityPolicy,
			fileDragRoots: this.fileDragRoots,
			sandbox: this.sandbox,
			startPassthrough: this.passthrough,
			spellCheck: this.spellCheck,
		});

		this.webviewId = webview.id;
	}

	get webview() {
		// todo (yoav): we don't want this to be undefined, so maybe we should just
		// link directly to the browserview object instead of a getter
		return BrowserView.getById(this.webviewId) as BrowserView<T>;
	}

	static getById(id: number) {
		return BrowserWindowMap[id];
	}

	setTitle(title: string) {
		this.title = title;
		return ffi.request.setTitle({ winId: this.id, title });
	}

	/**
	 * Toggles macOS system spell checking for this window's native WKWebView.
	 * Returns false when the current renderer/platform does not support it.
	 */
	setSpellCheck(enabled: boolean): boolean {
		this.spellCheck = enabled;
		return this.webview?.setSpellCheck(enabled) ?? false;
	}

	close() {
		return ffi.request.closeWindow({ winId: this.id });
	}

	requestClose() {
		return ffi.request.requestWindowClose({ winId: this.id });
	}

	activate() {
		return ffi.request.activateWindow({ winId: this.id });
	}

	focus() {
		console.log(
			"[electrobun] BrowserWindow.focus() is deprecated. Use window.activate() instead.",
		);
		return this.activate();
	}

	show() {
		const result = ffi.request.showWindow({ winId: this.id, activate: true });
		this.applyPendingRestore();
		return result;
	}

	showInactive() {
		const result = ffi.request.showWindow({ winId: this.id, activate: false });
		this.applyPendingRestore();
		return result;
	}

	hide() {
		return ffi.request.hideWindow({ winId: this.id });
	}

	isVisible(): boolean {
		return ffi.request.isWindowVisible({ winId: this.id });
	}

	minimize() {
		return ffi.request.minimizeWindow({ winId: this.id });
	}

	unminimize() {
		return ffi.request.restoreWindow({ winId: this.id });
	}

	isMinimized(): boolean {
		return ffi.request.isWindowMinimized({ winId: this.id });
	}

	maximize() {
		return ffi.request.maximizeWindow({ winId: this.id });
	}

	unmaximize() {
		return ffi.request.unmaximizeWindow({ winId: this.id });
	}

	isMaximized(): boolean {
		return ffi.request.isWindowMaximized({ winId: this.id });
	}

	setFullScreen(fullScreen: boolean) {
		return ffi.request.setWindowFullScreen({ winId: this.id, fullScreen });
	}

	isFullScreen(): boolean {
		return ffi.request.isWindowFullScreen({ winId: this.id });
	}

	/**
	 * Sets or removes (`null`) the window's vibrancy material. Returns false
	 * when it isn't supported: an unknown material, or not macOS.
	 */
	setVibrancy(vibrancy: WindowVibrancy | null): boolean {
		const { material, blending = "behind-window", state = "follow-window" } =
			typeof vibrancy === "string" ? { material: vibrancy } : vibrancy ?? { material: "none" };
		return ffi.request.setWindowVibrancy({ winId: this.id, material, blending, state }) ?? false;
	}

	setAlwaysOnTop(alwaysOnTop: boolean) {
		return ffi.request.setWindowAlwaysOnTop({ winId: this.id, alwaysOnTop });
	}

	isAlwaysOnTop(): boolean {
		return ffi.request.isWindowAlwaysOnTop({ winId: this.id });
	}
	
	setVisibleOnAllWorkspaces(visibleOnAllWorkspaces: boolean) {
		return ffi.request.setWindowVisibleOnAllWorkspaces({ winId: this.id, visibleOnAllWorkspaces });
	}
	
	isVisibleOnAllWorkspaces(): boolean {
		return ffi.request.isWindowVisibleOnAllWorkspaces({ winId: this.id });
	}

	setPosition(x: number, y: number) {
		this.frame.x = x;
		this.frame.y = y;
		return ffi.request.setWindowPosition({ winId: this.id, x, y });
	}

	center() {
		ffi.request.centerWindow({ winId: this.id });
		this.frame = ffi.request.getWindowFrame({ winId: this.id });
	}

	setWindowButtonPosition(x: number, y: number) {
		return ffi.request.setWindowButtonPosition({ winId: this.id, x, y });
	}

	getWindowButtonPosition(): { x: number; y: number } {
		return ffi.request.getWindowButtonPosition({ winId: this.id });
	}

	setSize(width: number, height: number) {
		this.frame.width = width;
		this.frame.height = height;
		return ffi.request.setWindowSize({ winId: this.id, width, height });
	}

	/**
	 * A frame is the window's top-left corner and its content size (without
	 * the title bar), the same as the constructor's frame and the resize
	 * event, so `new BrowserWindow({ frame: win.getFrame() })` reproduces it.
	 */
	setFrame(x: number, y: number, width: number, height: number) {
		this.frame = { x, y, width, height };
		return ffi.request.setWindowFrame({ winId: this.id, x, y, width, height });
	}

	getFrame(): { x: number; y: number; width: number; height: number } {
		const frame = ffi.request.getWindowFrame({ winId: this.id });
		// Update internal state
		this.frame = frame;
		return frame;
	}

	getPosition(): { x: number; y: number } {
		const frame = this.getFrame();
		return { x: frame.x, y: frame.y };
	}

	getSize(): { width: number; height: number } {
		const frame = this.getFrame();
		return { width: frame.width, height: frame.height };
	}

	/**
	 * Set the page zoom level for the window's webview (WebKit only).
	 * @param zoomLevel - The zoom level (1.0 = 100%, 1.5 = 150%, etc.)
	 */
	setPageZoom(zoomLevel: number) {
		this.webview?.setPageZoom(zoomLevel);
	}

	/**
	 * Get the current page zoom level for the window's webview.
	 * @returns The current zoom level (1.0 = 100%)
	 */
	getPageZoom(): number {
		return this.webview?.getPageZoom() ?? 1.0;
	}

	// todo (yoav): move this to a class that also has off, append, prepend, etc.
	// name should only allow browserWindow events
	on(name: string, handler: (event: unknown) => void) {
		const specificName = `${name}-${this.id}`;
		electrobunEventEmitter.on(specificName, handler);
	}
}
