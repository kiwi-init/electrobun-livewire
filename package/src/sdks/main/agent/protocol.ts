// Shared by the in-app agent server and the CLI/MCP client: where running apps
// announce themselves, and newline-delimited JSON-RPC 2.0 over a unix socket.

import {
	chmodSync,
	existsSync,
	lstatSync,
	mkdirSync,
	readFileSync,
	readdirSync,
	rmSync,
} from "node:fs";
import { createConnection } from "node:net";
import { userInfo } from "node:os";
import { join } from "node:path";

export const AGENT_PROTOCOL_VERSION = 1;

export type AgentAppRecord = {
	protocol: number;
	pid: number;
	name: string;
	socket: string;
	startedAt: string;
	platform: string;
};

export type JsonRpcRequest = {
	jsonrpc: "2.0";
	id: number | string;
	method: string;
	params?: Record<string, unknown>;
};

export type JsonRpcResponse =
	| { jsonrpc: "2.0"; id: number | string | null; result: unknown }
	| {
			jsonrpc: "2.0";
			id: number | string | null;
			error: { code: number; message: string };
	  };

// macOS caps unix socket paths at 104 bytes, so this lives directly under
// /tmp rather than the (long) per-user $TMPDIR.
export function agentDirectory(): string {
	const override = process.env["ELECTROBUN_AGENT_DIR"];
	if (override) return override;
	return join("/tmp", `livewire-agent-${userInfo().uid}`);
}

/**
 * Creates the per-user directory with mode 0700, refusing one that is a
 * symlink or owned by someone else.
 */
export function ensureAgentDirectory(directory = agentDirectory()): string {
	if (!existsSync(directory)) mkdirSync(directory, { recursive: true, mode: 0o700 });
	const stat = lstatSync(directory);
	if (stat.isSymbolicLink() || !stat.isDirectory()) {
		throw new Error(`${directory} must be a real directory`);
	}
	if (stat.uid !== userInfo().uid) {
		throw new Error(`${directory} is owned by another user`);
	}
	if ((stat.mode & 0o077) !== 0) chmodSync(directory, 0o700);
	return directory;
}

function isAlive(pid: number): boolean {
	try {
		process.kill(pid, 0);
		return true;
	} catch (error) {
		return (error as NodeJS.ErrnoException).code === "EPERM";
	}
}

/** Running apps with agent control on, newest first; stale records are removed. */
export function listAgentApps(directory = agentDirectory()): AgentAppRecord[] {
	if (!existsSync(directory)) return [];
	const apps: AgentAppRecord[] = [];
	for (const entry of readdirSync(directory)) {
		if (!entry.endsWith(".json")) continue;
		const path = join(directory, entry);
		let record: AgentAppRecord;
		try {
			record = JSON.parse(readFileSync(path, "utf8"));
		} catch {
			continue;
		}
		if (!isAlive(record.pid) || !existsSync(record.socket)) {
			rmSync(path, { force: true });
			rmSync(record.socket, { force: true });
			continue;
		}
		apps.push(record);
	}
	return apps.sort((a, b) => b.startedAt.localeCompare(a.startedAt));
}

/** Picks an app by pid or (case-insensitive) name; the newest one by default. */
export function selectAgentApp(
	apps: AgentAppRecord[],
	selector?: string | number,
): AgentAppRecord {
	if (apps.length === 0) {
		throw new Error(
			"no Livewire app with agent control is running (start one with ELECTROBUN_AGENT_CONTROL=1)",
		);
	}
	if (selector === undefined || selector === "") return apps[0]!;
	const wanted = String(selector).toLowerCase();
	const match = apps.find(
		(app) => String(app.pid) === wanted || app.name.toLowerCase() === wanted,
	);
	if (!match) {
		throw new Error(
			`no running app matches ${JSON.stringify(selector)}; running: ${apps
				.map((app) => `${app.name} (${app.pid})`)
				.join(", ")}`,
		);
	}
	return match;
}

/** One request over a fresh connection. */
export function callAgent(
	socketPath: string,
	method: string,
	params: Record<string, unknown> = {},
	timeoutMs = 30_000,
): Promise<unknown> {
	return new Promise((resolve, reject) => {
		const socket = createConnection(socketPath);
		let buffer = "";
		const timer = setTimeout(() => {
			socket.destroy();
			reject(new Error(`${method}: no response within ${timeoutMs}ms`));
		}, timeoutMs);
		const finish = (error: Error | null, value?: unknown) => {
			clearTimeout(timer);
			socket.end();
			if (error) reject(error);
			else resolve(value);
		};
		socket.on("connect", () => {
			const request: JsonRpcRequest = { jsonrpc: "2.0", id: 1, method, params };
			socket.write(`${JSON.stringify(request)}\n`);
		});
		socket.on("data", (chunk) => {
			buffer += chunk.toString();
			const newline = buffer.indexOf("\n");
			if (newline === -1) return;
			let response: JsonRpcResponse;
			try {
				response = JSON.parse(buffer.slice(0, newline));
			} catch (error) {
				finish(error as Error);
				return;
			}
			if ("error" in response) finish(new Error(response.error.message));
			else finish(null, response.result);
		});
		socket.on("error", (error) => finish(error));
	});
}
