// Agent control: lets an AI agent (or any local tool) drive this app's windows
// and views over a user-only unix socket. Off unless the app opts in.

import { randomUUID } from "node:crypto";
import { basename } from "node:path";
import { BrowserView } from "../core/BrowserView";
import { BrowserWindowMap } from "../core/BrowserWindow";
import { BuildConfig } from "../core/BuildConfig";
import { waitForAgentEval } from "./evalResults";
import { wrapEval } from "./pageScripts";
import type { AgentAppRecord } from "./protocol";
import {
	createAgentServer,
	type AgentAction,
	type AgentHost,
	type AgentViewInfo,
} from "./server";

export type { AgentAction, AgentAppRecord };

const actions = new Map<string, AgentAction>();
let server: ReturnType<typeof createAgentServer> | null = null;
let appName: string | undefined;

function defaultAppName(): string {
	const bundle = process.execPath.match(/([^/\\]+)\.app[/\\]/);
	return bundle?.[1] ?? basename(process.execPath).replace(/\.exe$/i, "");
}

function describeView(view: BrowserView): AgentViewInfo {
	return {
		id: view.id,
		windowId: view.windowId,
		url: view.url,
		renderer: view.renderer,
		sandbox: view.sandbox,
	};
}

const electrobunHost: AgentHost = {
	appName: () => appName ?? defaultAppName(),
	listWindows: () =>
		Object.values(BrowserWindowMap).map((window) => {
			let frame = null;
			try {
				frame = window.getFrame();
			} catch {}
			// The window's own webview first, then any tag or child views.
			const views = BrowserView.getAll()
				.filter((view) => view.windowId === window.id && !view.isRemoved)
				.sort((a, b) =>
					a.id === window.webviewId ? -1 : b.id === window.webviewId ? 1 : a.id - b.id,
				)
				.map(describeView);
			return { id: window.id, title: window.title, frame, views };
		}),
	evaluate: (viewId, source, timeoutMs) => {
		const view = BrowserView.getById(viewId);
		if (!view || view.isRemoved) {
			return Promise.resolve({ ok: false, error: `no view ${viewId}` });
		}
		const token = randomUUID();
		const result = waitForAgentEval(token, timeoutMs);
		view.executeJavascript(wrapEval(source, token));
		return result;
	},
	snapshot: async (viewId, timeoutMs) => {
		const view = BrowserView.getById(viewId);
		if (!view || view.isRemoved) throw new Error(`no view ${viewId}`);
		return view.snapshot({ timeoutMs });
	},
	navigate: (viewId, url) => {
		BrowserView.getById(viewId)?.loadURL(url);
	},
	focusWindow: (windowId) => {
		BrowserWindowMap[windowId]?.focus();
	},
};

export const AgentControl = {
	/** True when `ELECTROBUN_AGENT_CONTROL=1` or `runtime.agentControl: true` asks for it. */
	isRequested(): boolean {
		const env = process.env["ELECTROBUN_AGENT_CONTROL"];
		if (env === "1" || env === "true") return true;
		try {
			return BuildConfig.getSync().runtime?.["agentControl"] === true;
		} catch {
			return false;
		}
	},

	/** Starts listening (idempotent) and returns where agents can reach this app. */
	async start(options: { name?: string } = {}): Promise<AgentAppRecord> {
		if (process.platform === "win32") {
			throw new Error("agent control isn't available on Windows yet");
		}
		if (options.name) appName = options.name;
		if (!server) {
			server = createAgentServer({ host: electrobunHost, actions });
			process.once("exit", () => server?.stop());
		}
		return server.start();
	},

	stop(): void {
		server?.stop();
		server = null;
	},

	/**
	 * Exposes an app-level capability to agents, like a Siri quick action:
	 * `AgentControl.registerAction("open-room", { description, params, handler })`.
	 * Returns a function that unregisters it.
	 */
	registerAction(name: string, action: AgentAction): () => void {
		actions.set(name, action);
		return () => {
			if (actions.get(name) === action) actions.delete(name);
		};
	},

	get record(): AgentAppRecord | null {
		return server?.record ?? null;
	},
};

export function autoStartAgentControl(): void {
	if (!AgentControl.isRequested()) return;
	AgentControl.start()
		.then((record) => console.log(`[agent-control] listening on ${record.socket}`))
		.catch((error) => console.error("[agent-control] failed to start:", error));
}
