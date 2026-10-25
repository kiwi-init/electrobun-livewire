// The agent control server: JSON-RPC 2.0, one request per line, on a unix
// socket only the current user can open. It knows nothing about Electrobun
// itself; a host adapter supplies windows, views and page evaluation.

import { chmodSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { createServer, type Server, type Socket } from "node:net";
import { join } from "node:path";
import type { AgentEvalOutcome } from "./evalResults";
import {
	clickScript,
	consoleScript,
	outlineScript,
	textScript,
	typeScript,
	waitForScript,
} from "./pageScripts";
import {
	AGENT_PROTOCOL_VERSION,
	ensureAgentDirectory,
	type AgentAppRecord,
	type JsonRpcRequest,
	type JsonRpcResponse,
} from "./protocol";

export type AgentViewInfo = {
	id: number;
	windowId: number;
	url: string | null;
	renderer: string;
	sandbox: boolean;
};

export type AgentWindowInfo = {
	id: number;
	title: string;
	frame: { x: number; y: number; width: number; height: number } | null;
	views: AgentViewInfo[];
};

export type AgentHost = {
	appName(): string;
	listWindows(): AgentWindowInfo[];
	evaluate(viewId: number, source: string, timeoutMs: number): Promise<AgentEvalOutcome>;
	navigate(viewId: number, url: string): void;
	/** PNG data URL of what the view shows. */
	snapshot(viewId: number, timeoutMs: number): Promise<string>;
	focusWindow(windowId: number): void;
};

export type AgentAction = {
	description: string;
	/** Parameter names mapped to a short description, shown to agents. */
	params?: Record<string, string>;
	handler: (params: Record<string, unknown>) => unknown;
};

export class AgentRequestError extends Error {
	constructor(
		readonly code: number,
		message: string,
	) {
		super(message);
	}
}

const MAX_LINE_BYTES = 1024 * 1024;
const DEFAULT_TIMEOUT_MS = 10_000;

function str(params: Record<string, unknown>, key: string, required = true): string | undefined {
	const value = params[key];
	if (value === undefined && !required) return undefined;
	if (typeof value !== "string" || value === "") {
		throw new AgentRequestError(-32602, `"${key}" must be a non-empty string`);
	}
	return value;
}

function num(params: Record<string, unknown>, key: string, fallback: number): number {
	const value = params[key];
	if (value === undefined) return fallback;
	if (typeof value !== "number" || !Number.isFinite(value) || value <= 0) {
		throw new AgentRequestError(-32602, `"${key}" must be a positive number`);
	}
	return value;
}

export function createAgentServer(options: {
	host: AgentHost;
	actions: Map<string, AgentAction>;
	directory?: string;
	pid?: number;
}) {
	const { host, actions } = options;
	const pid = options.pid ?? process.pid;
	let server: Server | null = null;
	let record: AgentAppRecord | null = null;

	// A request names a view directly, or a window (its first view); otherwise
	// the only view, or the first view of the oldest window.
	function resolveView(params: Record<string, unknown>): AgentViewInfo {
		const windows = host.listWindows();
		const views = windows.flatMap((window) => window.views);
		if (params["view"] !== undefined) {
			const view = views.find((candidate) => candidate.id === params["view"]);
			if (!view) throw new AgentRequestError(-32602, `no view ${params["view"]}`);
			return view;
		}
		if (params["window"] !== undefined) {
			const window = windows.find((candidate) => candidate.id === params["window"]);
			if (!window) throw new AgentRequestError(-32602, `no window ${params["window"]}`);
			const view = window.views[0];
			if (!view) throw new AgentRequestError(-32602, `window ${window.id} has no views`);
			return view;
		}
		const first = [...windows].sort((a, b) => a.id - b.id)[0]?.views[0] ?? views[0];
		if (!first) throw new AgentRequestError(-32000, "the app has no open views");
		return first;
	}

	async function run(params: Record<string, unknown>, source: string): Promise<unknown> {
		const view = resolveView(params);
		if (view.sandbox) {
			throw new AgentRequestError(
				-32000,
				`view ${view.id} is sandboxed, so it has no bridge to return results`,
			);
		}
		const outcome = await host.evaluate(
			view.id,
			source,
			num(params, "timeoutMs", DEFAULT_TIMEOUT_MS),
		);
		if (!outcome.ok) throw new AgentRequestError(-32000, outcome.error);
		return outcome.value;
	}

	const methods: Record<string, (params: Record<string, unknown>) => unknown> = {
		"app.info": () => ({
			name: host.appName(),
			pid,
			platform: process.platform,
			protocol: AGENT_PROTOCOL_VERSION,
			methods: Object.keys(methods),
		}),
		"windows.list": () => host.listWindows(),
		"window.focus": (params) => {
			const id = params["window"];
			if (!host.listWindows().some((window) => window.id === id)) {
				throw new AgentRequestError(-32602, `no window ${id}`);
			}
			host.focusWindow(id as number);
			return { focused: id };
		},
		"view.navigate": (params) => {
			const view = resolveView(params);
			host.navigate(view.id, str(params, "url")!);
			return { view: view.id };
		},
		"view.eval": (params) => run(params, str(params, "js")!),
		"view.click": (params) => run(params, clickScript(str(params, "selector")!)),
		"view.type": (params) =>
			run(
				params,
				typeScript(
					str(params, "selector")!,
					typeof params["text"] === "string" ? params["text"] : "",
					params["submit"] === true,
				),
			),
		"view.text": (params) =>
			run(params, textScript(str(params, "selector", false), num(params, "maxChars", 20_000))),
		"view.waitFor": (params) => {
			const timeoutMs = num(params, "timeoutMs", DEFAULT_TIMEOUT_MS);
			// The page-side wait needs headroom before the eval itself times out.
			return run(
				{ ...params, timeoutMs: timeoutMs + 2_000 },
				waitForScript(str(params, "selector")!, timeoutMs, params["visible"] === true),
			);
		},
		"view.outline": (params) => run(params, outlineScript(num(params, "limit", 200))),
		"view.screenshot": async (params) => {
			const view = resolveView(params);
			const dataUrl = await host.snapshot(view.id, num(params, "timeoutMs", DEFAULT_TIMEOUT_MS));
			const match = /^data:(image\/[a-z]+);base64,(.*)$/s.exec(dataUrl);
			if (!match) throw new AgentRequestError(-32000, "the view returned no image");
			return { view: view.id, mimeType: match[1], base64: match[2] };
		},
		"view.console": (params) => run(params, consoleScript()),
		"actions.list": () =>
			[...actions].map(([name, action]) => ({
				name,
				description: action.description,
				params: action.params ?? {},
			})),
		"actions.invoke": async (params) => {
			const name = str(params, "name")!;
			const action = actions.get(name);
			if (!action) throw new AgentRequestError(-32602, `no action "${name}"`);
			const args = params["params"];
			return (
				(await action.handler(
					args && typeof args === "object" ? (args as Record<string, unknown>) : {},
				)) ?? null
			);
		},
	};

	async function dispatch(request: JsonRpcRequest): Promise<JsonRpcResponse> {
		const id = request.id ?? null;
		const method = methods[request.method];
		if (!method) {
			return { jsonrpc: "2.0", id, error: { code: -32601, message: `unknown method ${request.method}` } };
		}
		try {
			const params = request.params && typeof request.params === "object" ? request.params : {};
			return { jsonrpc: "2.0", id, result: (await method(params)) ?? null };
		} catch (error) {
			const code = error instanceof AgentRequestError ? error.code : -32000;
			const message = error instanceof Error ? error.message : String(error);
			return { jsonrpc: "2.0", id, error: { code, message } };
		}
	}

	function serve(socket: Socket) {
		let buffer = "";
		socket.on("data", (chunk) => {
			buffer += chunk.toString();
			if (buffer.length > MAX_LINE_BYTES) {
				socket.destroy();
				return;
			}
			let newline: number;
			while ((newline = buffer.indexOf("\n")) !== -1) {
				const line = buffer.slice(0, newline).trim();
				buffer = buffer.slice(newline + 1);
				if (!line) continue;
				let request: JsonRpcRequest;
				try {
					request = JSON.parse(line);
				} catch {
					socket.write(`${JSON.stringify({ jsonrpc: "2.0", id: null, error: { code: -32700, message: "invalid JSON" } })}\n`);
					continue;
				}
				void dispatch(request).then((response) => {
					if (!socket.destroyed) socket.write(`${JSON.stringify(response)}\n`);
				});
			}
		});
		socket.on("error", () => {});
	}

	function removeFiles() {
		if (!record) return;
		rmSync(record.socket, { force: true });
		rmSync(record.socket.replace(/\.sock$/, ".json"), { force: true });
	}

	return {
		dispatch,
		get record() {
			return record;
		},
		start(): Promise<AgentAppRecord> {
			if (server && record) return Promise.resolve(record);
			const directory = ensureAgentDirectory(options.directory);
			const socketPath = join(directory, `${pid}.sock`);
			const recordPath = join(directory, `${pid}.json`);
			rmSync(socketPath, { force: true });
			return new Promise((resolve, reject) => {
				const listening = createServer(serve);
				listening.once("error", reject);
				listening.listen(socketPath, () => {
					chmodSync(socketPath, 0o600);
					record = {
						protocol: AGENT_PROTOCOL_VERSION,
						pid,
						name: host.appName(),
						socket: socketPath,
						startedAt: new Date().toISOString(),
						platform: process.platform,
					};
					const temporary = `${recordPath}.tmp`;
					writeFileSync(temporary, JSON.stringify(record, null, 2), { mode: 0o600 });
					renameSync(temporary, recordPath);
					server = listening;
					resolve(record);
				});
			});
		},
		stop() {
			server?.close();
			server = null;
			removeFiles();
			record = null;
		},
	};
}
