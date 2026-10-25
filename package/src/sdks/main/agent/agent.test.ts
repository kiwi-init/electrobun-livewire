import { afterAll, beforeAll, describe, expect, test } from "bun:test";
import { mkdtempSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { toolList } from "./cli";
import { settleAgentEval, settleAgentEvalNoBridge, waitForAgentEval } from "./evalResults";
import {
	clickScript,
	consoleScript,
	isExpression,
	outlineScript,
	textScript,
	typeScript,
	waitForScript,
	wrapEval,
} from "./pageScripts";
import { callAgent, listAgentApps, selectAgentApp } from "./protocol";
import { createAgentServer, type AgentAction, type AgentHost } from "./server";

const AsyncFunction = (async () => {}).constructor as FunctionConstructor;

// Runs an injected script the way a page would, with a bridge that delivers
// results straight to settleAgentEval.
function runInFakePage(code: string) {
	const window = {
		__electrobunInternalBridge: {
			__electrobunSigned: true,
			postMessage(batch: string) {
				for (const message of JSON.parse(batch)) {
					const parsed = JSON.parse(message);
					expect(parsed).toMatchObject({ type: "message", id: "agentEvalResult" });
					settleAgentEval(parsed.payload);
				}
			},
		},
	};
	new Function("window", code)(window);
}

describe("page scripts", () => {
	test("tells expressions from statement bodies", () => {
		expect(isExpression("document.title")).toBe(true);
		expect(isExpression("{ a: 1 }")).toBe(true);
		expect(isExpression("await fetch('/x')")).toBe(true);
		expect(isExpression("const a = 1; return a")).toBe(false);
		expect(isExpression("return 1")).toBe(false);
	});

	test("every generated script parses", () => {
		for (const body of [
			clickScript("#go"),
			typeScript("input[name='q']", 'say "hi"\n', true),
			textScript(undefined, 100),
			textScript(".main", 100),
			waitForScript("#ready", 500, true),
			outlineScript(50),
			consoleScript(),
		]) {
			expect(() => new AsyncFunction(body)).not.toThrow();
			expect(() => new Function(wrapEval(body, "t"))).not.toThrow();
		}
	});

	test("an evaluation round-trips its value through the bridge", async () => {
		const pending = waitForAgentEval("tok-1", 1_000);
		runInFakePage(wrapEval("({ n: 40 + 2, list: [1, 2], missing: undefined })", "tok-1"));
		expect(await pending).toEqual({ ok: true, value: { n: 42, list: [1, 2] } });
	});

	test("statement bodies, awaits, cycles and bigints serialize", async () => {
		const pending = waitForAgentEval("tok-2", 1_000);
		runInFakePage(
			wrapEval(
				"const a = { big: 10n }; a.self = a; await Promise.resolve(); return a;",
				"tok-2",
			),
		);
		expect(await pending).toEqual({ ok: true, value: { big: "10", self: "[Circular]" } });
	});

	test("a thrown error comes back as the failure", async () => {
		const pending = waitForAgentEval("tok-3", 1_000);
		runInFakePage(wrapEval("throw new Error('nope')", "tok-3"));
		const outcome = await pending;
		expect(outcome.ok).toBe(false);
		expect(!outcome.ok && outcome.error).toContain("Error: nope");
	});

	test("an evaluation that never answers times out", async () => {
		const outcome = await waitForAgentEval("tok-never", 20);
		expect(outcome.ok).toBe(false);
	});

	test("a page without the internal bridge says so over the event bridge", async () => {
		const posted: string[] = [];
		const page = {
			__electrobunWebviewId: 7,
			__electrobunEventBridge: { postMessage: (message: string) => posted.push(message) },
		};
		const pending = waitForAgentEval("tok-nb", 1_000);
		new Function("window", "location", wrapEval("document.title", "tok-nb"))(page, { href: "https://other.example/x" });
		await Promise.resolve();
		await new Promise((resolve) => setTimeout(resolve, 0));
		expect(posted).toHaveLength(1);
		const message = JSON.parse(posted[0]!);
		expect(message).toMatchObject({ id: "webviewEvent", type: "message", payload: { id: 7, eventName: "agent-eval-no-bridge" } });
		// The host side: native.ts routes this event to settleAgentEvalNoBridge.
		settleAgentEvalNoBridge(message.payload.detail);
		const outcome = await pending;
		expect(outcome.ok).toBe(false);
		expect(!outcome.ok && outcome.error).toContain("https://other.example/x");
		expect(!outcome.ok && outcome.error).toContain("bridgeOrigins");
		// CEF leaves a raw, unsigned internal bridge on pages outside bridgeOrigins:
		// it must be treated as absent, not posted to.
		const raw: string[] = [];
		const cefPage = { ...page, __electrobunInternalBridge: { postMessage: (message: string) => raw.push(message) } };
		posted.length = 0;
		const pendingCef = waitForAgentEval("tok-cef", 1_000);
		new Function("window", "location", wrapEval("1", "tok-cef"))(cefPage, { href: "https://cef.example/" });
		await new Promise((resolve) => setTimeout(resolve, 0));
		expect(raw).toHaveLength(0);
		expect(JSON.parse(posted[0]!).payload.eventName).toBe("agent-eval-no-bridge");
		settleAgentEvalNoBridge(JSON.parse(posted[0]!).payload.detail);
		expect((await pendingCef).ok).toBe(false);
		// Only failures travel this way: a forged "value" isn't accepted.
		expect(() => settleAgentEvalNoBridge("not json")).not.toThrow();
	});

	test("results for unknown tokens are ignored", () => {
		expect(() => settleAgentEval({ token: "unknown", ok: true, value: 1 })).not.toThrow();
		expect(() => settleAgentEval(null)).not.toThrow();
	});
});

describe.skipIf(process.platform === "win32")("agent server", () => {
	// macOS limits socket paths to 104 bytes, so stay under /tmp.
	const directory = mkdtempSync("/tmp/lw-agent-test-");
	const evaluated: Array<{ viewId: number; source: string }> = [];
	const navigated: string[] = [];
	const host: AgentHost = {
		appName: () => "Test App",
		listWindows: () => [
			{
				id: 2,
				title: "Second",
				frame: { x: 0, y: 0, width: 10, height: 10 },
				views: [{ id: 21, windowId: 2, url: "views://two", renderer: "native", sandbox: true }],
			},
			{
				id: 1,
				title: "Main",
				frame: { x: 0, y: 0, width: 800, height: 600 },
				views: [
					{ id: 11, windowId: 1, url: "views://main", renderer: "native", sandbox: false },
					{ id: 12, windowId: 1, url: "views://tag", renderer: "native", sandbox: false },
				],
			},
		],
		evaluate: async (viewId, source, timeoutMs) => {
			evaluated.push({ viewId, source });
			const token = `server-${evaluated.length}`;
			const pending = waitForAgentEval(token, timeoutMs);
			// Element-free page: the DOM helpers fail, plain expressions work.
			runInFakePage(wrapEval(source, token));
			return pending;
		},
		navigate: (_viewId, url) => navigated.push(url),
		snapshot: async (viewId) => (viewId === 11 ? "data:image/png;base64,iVBORw0KGgo=" : "not an image"),
		focusWindow: () => {},
	};
	const actions = new Map<string, AgentAction>([
		[
			"greet",
			{
				description: "Says hello",
				params: { name: "who to greet" },
				handler: ({ name }) => ({ greeting: `hello ${name}` }),
			},
		],
	]);
	const server = createAgentServer({ host, actions, directory, pid: process.pid });
	let socket = "";

	beforeAll(async () => {
		socket = (await server.start()).socket;
	});
	afterAll(() => {
		server.stop();
		rmSync(directory, { recursive: true, force: true });
	});

	test("announces itself with a user-only socket and record", () => {
		expect(statSync(directory).mode & 0o777).toBe(0o700);
		expect(statSync(socket).mode & 0o777).toBe(0o600);
		const [record] = listAgentApps(directory);
		expect(record).toMatchObject({ pid: process.pid, name: "Test App", socket });
		expect(selectAgentApp([record!], "test app").pid).toBe(process.pid);
		expect(() => selectAgentApp([record!], "other")).toThrow(/no running app matches/);
	});

	test("reports app info and windows", async () => {
		const info = (await callAgent(socket, "app.info")) as { name: string; methods: string[] };
		expect(info.name).toBe("Test App");
		expect(info.methods).toContain("view.eval");
		const windows = (await callAgent(socket, "windows.list")) as Array<{ id: number }>;
		expect(windows.map((window) => window.id)).toEqual([2, 1]);
	});

	test("evaluates in the oldest window's main view by default, or the one named", async () => {
		expect(await callAgent(socket, "view.eval", { js: "6 * 7" })).toBe(42);
		expect(evaluated.at(-1)!.viewId).toBe(11);
		await callAgent(socket, "view.eval", { js: "1", view: 12 });
		expect(evaluated.at(-1)!.viewId).toBe(12);
		await callAgent(socket, "view.eval", { js: "1", window: 1 });
		expect(evaluated.at(-1)!.viewId).toBe(11);
	});

	test("refuses sandboxed views and unknown targets", async () => {
		await expect(callAgent(socket, "view.eval", { js: "1", view: 21 })).rejects.toThrow(/sandboxed/);
		await expect(callAgent(socket, "view.eval", { js: "1", view: 99 })).rejects.toThrow(/no view 99/);
		await expect(callAgent(socket, "view.click", {})).rejects.toThrow(/"selector"/);
		await expect(callAgent(socket, "nope")).rejects.toThrow(/unknown method/);
	});

	test("passes page errors back", async () => {
		await expect(callAgent(socket, "view.eval", { js: "missingThing.x" })).rejects.toThrow(/missingThing/);
	});

	test("returns screenshots as base64 PNG", async () => {
		expect(await callAgent(socket, "view.screenshot")).toEqual({ view: 11, mimeType: "image/png", base64: "iVBORw0KGgo=" });
		await expect(callAgent(socket, "view.screenshot", { view: 12 })).rejects.toThrow(/no image/);
	});

	test("navigates and runs registered actions", async () => {
		await callAgent(socket, "view.navigate", { url: "https://example.com" });
		expect(navigated).toEqual(["https://example.com"]);
		expect(await callAgent(socket, "actions.list")).toEqual([
			{ name: "greet", description: "Says hello", params: { name: "who to greet" } },
		]);
		expect(await callAgent(socket, "actions.invoke", { name: "greet", params: { name: "raven" } })).toEqual({
			greeting: "hello raven",
		});
	});

	test("answers several requests on one connection, in any order", async () => {
		const net = await import("node:net");
		const answers = await new Promise<string[]>((resolve) => {
			const client = net.createConnection(socket, () => {
				client.write(
					`${JSON.stringify({ jsonrpc: "2.0", id: "a", method: "app.info" })}\nnot json\n${JSON.stringify({ jsonrpc: "2.0", id: "b", method: "view.eval", params: { js: "2" } })}\n`,
				);
			});
			let buffer = "";
			client.on("data", (chunk) => {
				buffer += chunk;
				const lines = buffer.trim().split("\n");
				if (lines.length === 3) {
					client.end();
					resolve(lines);
				}
			});
		});
		const byId = Object.fromEntries(answers.map((line) => [JSON.parse(line).id, JSON.parse(line)]));
		expect(byId["a"].result.name).toBe("Test App");
		expect(byId["b"].result).toBe(2);
		expect(byId["null"].error.code).toBe(-32700);
	});

	test("forgets apps whose process is gone", () => {
		const stale = join(directory, "999999.json");
		writeFileSync(stale, JSON.stringify({ pid: 999999, name: "Gone", socket: join(directory, "999999.sock"), startedAt: "2000-01-01T00:00:00Z" }));
		expect(listAgentApps(directory).map((app) => app.pid)).toEqual([process.pid]);
		expect(readdirSync(directory)).not.toContain("999999.json");
	});
});

describe("MCP tools", () => {
	test("every tool has an object schema and its required fields exist", () => {
		const tools = toolList();
		expect(tools.map((tool) => tool.name)).toContain("livewire_outline");
		for (const tool of tools) {
			expect(tool.inputSchema.type).toBe("object");
			for (const field of (tool.inputSchema as { required?: string[] }).required ?? []) {
				expect(Object.keys(tool.inputSchema.properties)).toContain(field);
			}
		}
	});
});
