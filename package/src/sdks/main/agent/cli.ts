#!/usr/bin/env bun
// Drives running Livewire apps that have agent control on.
//
//   bun cli.ts apps                         running apps
//   bun cli.ts [--app NAME|PID] call METHOD [JSON]
//   bun cli.ts [--app NAME|PID] eval JS     shorthand for call view.eval
//   bun cli.ts [--app NAME|PID] screenshot [FILE.png] [VIEW]
//   bun cli.ts mcp                          MCP server on stdio
//
// For Claude Code: claude mcp add livewire -- bun <path>/cli.ts mcp

import { writeFileSync } from "node:fs";
import { createInterface } from "node:readline";
import { callAgent, listAgentApps, selectAgentApp } from "./protocol";

type ToolSpec = {
	method: string;
	description: string;
	properties: Record<string, { type: string; description: string }>;
	required?: string[];
};

const target = {
	view: { type: "number", description: "View id (from livewire_windows). Defaults to the first window's main view." },
	window: { type: "number", description: "Window id; uses that window's main view." },
};
const app = { type: "string", description: "App name or pid when several are running. Defaults to the newest." };

const TOOLS: Record<string, ToolSpec> = {
	livewire_windows: {
		method: "windows.list",
		description: "List the app's windows and the views (webviews) in each, with ids, titles, frames and URLs.",
		properties: {},
	},
	livewire_outline: {
		method: "view.outline",
		description: "List the clickable and editable controls in a view (links, buttons, inputs…) with a CSS selector, role and label for each. Start here to find what to click or type into.",
		properties: { ...target, limit: { type: "number", description: "Max controls (default 200)." } },
	},
	livewire_text: {
		method: "view.text",
		description: "Visible text of the page, or of one element.",
		properties: { ...target, selector: { type: "string", description: "CSS selector; whole page if omitted." }, maxChars: { type: "number", description: "Default 20000." } },
	},
	livewire_click: {
		method: "view.click",
		description: "Click the element matching a CSS selector (scrolls it into view, focuses it, sends pointer/mouse events and click()).",
		properties: { ...target, selector: { type: "string", description: "CSS selector." } },
		required: ["selector"],
	},
	livewire_type: {
		method: "view.type",
		description: "Set the text of an input, textarea or contenteditable, firing input/change events. submit=true also presses Enter and submits its form.",
		properties: { ...target, selector: { type: "string", description: "CSS selector." }, text: { type: "string", description: "Text to enter (replaces the current value)." }, submit: { type: "boolean", description: "Press Enter afterwards." } },
		required: ["selector", "text"],
	},
	livewire_wait_for: {
		method: "view.waitFor",
		description: "Wait until an element matching a selector exists (or is visible).",
		properties: { ...target, selector: { type: "string", description: "CSS selector." }, timeoutMs: { type: "number", description: "Default 10000." }, visible: { type: "boolean", description: "Also require it to be visible." } },
		required: ["selector"],
	},
	livewire_screenshot: {
		method: "view.screenshot",
		description: "Take a PNG screenshot of a view, also while its window is hidden or minimized. macOS native views today.",
		properties: { ...target },
	},
	livewire_eval: {
		method: "view.eval",
		description: "Run JavaScript in a view and return its (awaited) result. Pass an expression (`document.title`) or a function body with `return`.",
		properties: { ...target, js: { type: "string", description: "Expression or function body." }, timeoutMs: { type: "number", description: "Default 10000." } },
		required: ["js"],
	},
	livewire_console: {
		method: "view.console",
		description: "Console output and uncaught errors from a view since the last call. The first call starts capturing.",
		properties: { ...target },
	},
	livewire_navigate: {
		method: "view.navigate",
		description: "Load a URL in a view.",
		properties: { ...target, url: { type: "string", description: "URL to load." } },
		required: ["url"],
	},
	livewire_focus_window: {
		method: "window.focus",
		description: "Bring a window to the front.",
		properties: { window: target.window },
		required: ["window"],
	},
	livewire_actions: {
		method: "actions.list",
		description: "List the actions this app exposes to agents (registered with AgentControl.registerAction).",
		properties: {},
	},
	livewire_invoke: {
		method: "actions.invoke",
		description: "Run one of the app's registered actions.",
		properties: { name: { type: "string", description: "Action name." }, params: { type: "object", description: "Action parameters." } },
		required: ["name"],
	},
};

async function call(appSelector: string | undefined, method: string, params: Record<string, unknown>) {
	const record = selectAgentApp(listAgentApps(), appSelector);
	return callAgent(record.socket, method, params);
}

async function runTool(name: string, args: Record<string, unknown>): Promise<unknown> {
	if (name === "livewire_apps") return listAgentApps();
	const tool = TOOLS[name];
	if (!tool) throw new Error(`unknown tool ${name}`);
	const { app: appSelector, ...params } = args;
	return call(appSelector === undefined ? undefined : String(appSelector), tool.method, params);
}

function toolList() {
	return [
		{
			name: "livewire_apps",
			description: "List running Livewire apps that accept agent control (name, pid, socket).",
			inputSchema: { type: "object", properties: {} },
		},
		...Object.entries(TOOLS).map(([name, tool]) => ({
			name,
			description: tool.description,
			inputSchema: {
				type: "object",
				properties: { app, ...tool.properties },
				...(tool.required ? { required: tool.required } : {}),
			},
		})),
	];
}

function serveMcp() {
	const send = (message: unknown) => process.stdout.write(`${JSON.stringify(message)}\n`);
	const lines = createInterface({ input: process.stdin });
	lines.on("line", async (line) => {
		if (!line.trim()) return;
		let message: { id?: number | string; method?: string; params?: any };
		try {
			message = JSON.parse(line);
		} catch {
			send({ jsonrpc: "2.0", id: null, error: { code: -32700, message: "invalid JSON" } });
			return;
		}
		const { id, method, params } = message;
		if (id === undefined) return; // notifications (initialized, cancelled) need no reply
		const reply = (result: unknown) => send({ jsonrpc: "2.0", id, result });
		switch (method) {
			case "initialize":
				reply({
					protocolVersion: params?.protocolVersion ?? "2025-06-18",
					capabilities: { tools: {} },
					serverInfo: { name: "livewire-agent", version: "1.0.0" },
					instructions:
						"Drive running Livewire (Electrobun) apps. Start with livewire_windows, then livewire_outline to find selectors, then click/type/wait_for. Use livewire_eval for anything else.",
				});
				return;
			case "ping":
				reply({});
				return;
			case "tools/list":
				reply({ tools: toolList() });
				return;
			case "tools/call":
				try {
					const result = await runTool(params?.name, params?.arguments ?? {});
					if (params?.name === "livewire_screenshot") {
						const image = result as { view: number; mimeType: string; base64: string };
						reply({ content: [{ type: "image", data: image.base64, mimeType: image.mimeType }] });
						return;
					}
					reply({ content: [{ type: "text", text: JSON.stringify(result, null, 2) }] });
				} catch (error) {
					reply({
						content: [{ type: "text", text: error instanceof Error ? error.message : String(error) }],
						isError: true,
					});
				}
				return;
			default:
				send({ jsonrpc: "2.0", id, error: { code: -32601, message: `unknown method ${method}` } });
		}
	});
}

async function main(argv: string[]) {
	let appSelector: string | undefined;
	const appFlag = argv.indexOf("--app");
	if (appFlag !== -1) {
		appSelector = argv[appFlag + 1];
		argv.splice(appFlag, 2);
	}
	const [command, ...rest] = argv;
	switch (command) {
		case "mcp":
			serveMcp();
			return;
		case "apps":
			console.log(JSON.stringify(listAgentApps(), null, 2));
			return;
		case "call": {
			const [method, json] = rest;
			if (!method) throw new Error("usage: call METHOD [JSON params]");
			const result = await call(appSelector, method, json ? JSON.parse(json) : {});
			console.log(JSON.stringify(result, null, 2));
			return;
		}
		case "screenshot": {
			const [path = "livewire-screenshot.png", view] = rest;
			const image = (await call(appSelector, "view.screenshot", view ? { view: Number(view) } : {})) as {
				view: number;
				base64: string;
			};
			writeFileSync(path, Uint8Array.from(atob(image.base64), (char) => char.charCodeAt(0)));
			console.log(`view ${image.view} → ${path}`);
			return;
		}
		case "eval": {
			const js = rest.join(" ");
			if (!js) throw new Error("usage: eval JS");
			console.log(JSON.stringify(await call(appSelector, "view.eval", { js }), null, 2));
			return;
		}
		default:
			console.error("usage: cli.ts apps | [--app NAME|PID] call METHOD [JSON] | eval JS | screenshot [FILE.png] [VIEW] | mcp");
			process.exitCode = 2;
	}
}

if (import.meta.main) {
	main(process.argv.slice(2)).catch((error) => {
		console.error(error instanceof Error ? error.message : String(error));
		process.exitCode = 1;
	});
}

export { runTool, toolList };
