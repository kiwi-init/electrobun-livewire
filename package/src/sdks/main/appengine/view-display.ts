// AppEngine's default display: the UI runs in a BrowserView on top of the
// app's window, with its own throwaway partition, the `window.engine` preload
// and AppEngine's RPC. The engine replaces the view when a UI hangs.

import { appEnginePageScript } from "../../../preload/.generated/compiled";
import type { AppEngineRPC, EngineInvokeResult } from "../../../shared/appengine-protocol";
import { BrowserView } from "../core/BrowserView";
import type { BrowserWindow } from "../core/BrowserWindow";

export type ViewDisplayEngine = {
	invoke(cap: string, args: unknown): Promise<EngineInvokeResult>;
	onReady(href: string): void;
	onBootError(href: string, message: string): void;
	onRuntimeError(href: string, message: string): Promise<void>;
	onRendererGone(): void;
};

const stripQuery = (url: string) => url.split(/[?#]/)[0]!;

export class ViewDisplay {
	private view: BrowserView | null = null;
	private rpc: ReturnType<typeof makeRpc> | null = null;
	private generation = 0;

	constructor(
		private readonly engine: ViewDisplayEngine,
		private readonly window: BrowserWindow,
		private readonly safeModeUrl: string,
		private readonly prelude: string,
	) {}

	getView(): BrowserView | null {
		return this.view;
	}

	mount(url: string) {
		const frame = this.window.getFrame();
		const rpc = makeRpc(this.engine);
		this.view = new BrowserView({
			windowId: this.window.id,
			url,
			// Ephemeral: nothing the page stores outlives the view, so UI data
			// lives in engine storage, which the engine can snapshot and guard.
			partition: `appengine-${++this.generation}`,
			preload: this.prelude + appEnginePageScript,
			rpc,
			allowedProtocols: { views: true, appData: true },
			navigationRules: JSON.stringify(["^*", "appdata://ui/*", `${stripQuery(this.safeModeUrl)}*`]),
			frame: { x: 0, y: 0, width: frame.width, height: frame.height },
			autoResize: true,
		});
		this.rpc = rpc;
		const view = this.view;
		view.on("render-process-gone", () => {
			if (view !== this.view) return;
			// Wake recovery killed a frozen process and reloads the same UI, which
			// boots again: not the UI's failure.
			if (view.rendererExitExpected) {
				view.rendererExitExpected = null;
				return;
			}
			this.engine.onRendererGone();
		});
	}

	load(url: string) {
		this.view?.loadURL(url);
	}

	// A UI stuck in a loop can't be navigated away from: kill its renderer and
	// show the next URL in a fresh view.
	replace(url: string) {
		const old = this.view;
		old?.terminateRenderer();
		this.mount(url);
		old?.remove();
	}

	async prepareSwap(toVersion: string): Promise<unknown> {
		const result = await this.rpc?.request.prepareSwap({ toVersion });
		return result?.state ?? null;
	}
}

function makeRpc(engine: ViewDisplayEngine) {
	return BrowserView.defineRPC<AppEngineRPC>({
		maxRequestTime: 10_000,
		handlers: {
			requests: {
				invoke: ({ cap, args }) => engine.invoke(cap, args),
			},
			messages: {
				ready: ({ href }) => engine.onReady(href),
				bootError: ({ href, message }) => engine.onBootError(href, message),
				runtimeError: ({ href, message }) => void engine.onRuntimeError(href, message),
			},
		},
	});
}
