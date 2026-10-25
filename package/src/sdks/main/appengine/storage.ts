// AppEngine engine storage: the UI's durable key/value store.
//
// It lives in the main process, under userData/appengine/storage, so it
// doesn't depend on the page's origin and pages can't read it through
// appdata://. The boot lifecycle snapshots it before a never-good UI's first
// boot and restores that snapshot when the boot fails (the `EngineStorage`
// hook). Pages reach it through the `storage.*` capabilities below.

import { closeSync, existsSync, fsyncSync, mkdirSync, openSync, readFileSync, readdirSync, renameSync, rmSync, statSync, writeSync } from "node:fs";
import { dirname, join } from "node:path";
import { randomBytes } from "node:crypto";

export type EngineStoreOptions = {
	// Where the store and its snapshots live. Defaults to
	// userData/appengine/storage, outside what appdata:// serves.
	dir?: string;
	// Largest the stored data may grow, as JSON bytes. Default 5 MB.
	maxBytes?: number;
	// Snapshots kept; older ones are deleted when a new one is taken. Default 5.
	keepSnapshots?: number;
};

export type StorageOp =
	| { op: "set"; key: string; value: unknown }
	| { op: "delete"; key: string };

export class StorageError extends Error {
	constructor(
		public readonly code: "E_BAD_ARGS" | "E_QUOTA",
		message: string,
	) {
		super(message);
	}
}

const DEFAULT_MAX_BYTES = 5 * 1024 * 1024;
const DEFAULT_KEEP_SNAPSHOTS = 5;
const MAX_KEY_LENGTH = 256;
const SNAPSHOT_NAME = /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/;

// One EngineStore per folder: it keeps the data in memory and rewrites the
// whole file on each write, so a second instance on the same folder would
// overwrite the first one's writes.
export class EngineStore {
	private readonly file: string;
	private readonly snapshotDir: string;
	private readonly maxBytes: number;
	private readonly keepSnapshots: number;
	// A Map, so keys like "__proto__" are plain keys.
	private data: Map<string, unknown>;

	// Opens the store in `options.dir`, or under userData when it's omitted.
	static async open(options: EngineStoreOptions = {}): Promise<EngineStore> {
		if (options.dir) return new EngineStore(options as EngineStoreOptions & { dir: string });
		// Imported lazily: Utils needs the app's runtime, which unit tests don't have.
		const { paths } = await import("../core/Utils");
		return new EngineStore({ ...options, dir: join(paths.userData, "appengine", "storage") });
	}

	constructor(options: EngineStoreOptions & { dir: string }) {
		this.file = join(options.dir, "storage.json");
		this.snapshotDir = join(options.dir, "snapshots");
		this.maxBytes = options.maxBytes ?? DEFAULT_MAX_BYTES;
		this.keepSnapshots = Math.max(1, options.keepSnapshots ?? DEFAULT_KEEP_SNAPSHOTS);
		mkdirSync(options.dir, { recursive: true });
		sweepTemp(options.dir);
		this.data = this.load();
	}

	// The stored value, or null when the key isn't set.
	get(key: string): unknown {
		checkKey(key);
		return this.data.has(key) ? structuredClone(this.data.get(key)) : null;
	}

	set(key: string, value: unknown) {
		this.batch([{ op: "set", key, value }]);
	}

	delete(key: string) {
		this.batch([{ op: "delete", key }]);
	}

	keys(): string[] {
		return [...this.data.keys()];
	}

	// All or nothing: the ops are checked and applied to a copy, which is
	// written once. A bad op or going over the quota changes nothing.
	batch(ops: StorageOp[]) {
		if (!Array.isArray(ops)) throw new StorageError("E_BAD_ARGS", "ops must be an array");
		const next = new Map(this.data);
		for (const op of ops) {
			if (typeof op !== "object" || op === null) throw new StorageError("E_BAD_ARGS", "each op must be an object");
			checkKey(op.key);
			if (op.op === "set") next.set(op.key, toJsonValue(op.value, op.key));
			else if (op.op === "delete") next.delete(op.key);
			else throw new StorageError("E_BAD_ARGS", `unknown op ${String((op as { op: unknown }).op)}`);
		}
		const json = serialize(next);
		const bytes = Buffer.byteLength(json);
		if (bytes > this.maxBytes) {
			throw new StorageError("E_QUOTA", `storage would be ${bytes} bytes, over the ${this.maxBytes}-byte quota`);
		}
		writeAtomic(this.file, json);
		this.data = next;
	}

	// Copies the current data aside under `name`, replacing an older snapshot
	// of that name.
	snapshot(name: string): void {
		checkSnapshotName(name);
		mkdirSync(this.snapshotDir, { recursive: true });
		writeAtomic(join(this.snapshotDir, `${name}.json`), serialize(this.data));
		this.pruneSnapshots(name);
	}

	// Puts the snapshot `name` back as the current data. False when there's
	// no such snapshot, and the data is left as it is.
	restore(name: string): boolean {
		checkSnapshotName(name);
		const file = join(this.snapshotDir, `${name}.json`);
		let json: string;
		try {
			json = readFileSync(file, "utf8");
		} catch {
			return false;
		}
		const data = parse(json);
		if (!data) return false;
		writeAtomic(this.file, json);
		this.data = data;
		return true;
	}

	snapshots(): string[] {
		return this.listSnapshots().map((entry) => entry.name);
	}

	private load(): Map<string, unknown> {
		if (!existsSync(this.file)) return new Map();
		const data = parse(readFileSync(this.file, "utf8"));
		if (data) return data;
		// Keep an unreadable file for inspection instead of overwriting it on
		// the next write.
		const aside = `${this.file}.corrupt-${Date.now()}`;
		renameSync(this.file, aside);
		console.error(`AppEngine storage: ${this.file} was unreadable; moved it to ${aside} and started empty`);
		return new Map();
	}

	private listSnapshots() {
		let entries: string[];
		try {
			entries = readdirSync(this.snapshotDir);
		} catch {
			return [];
		}
		return entries
			.filter((entry) => entry.endsWith(".json"))
			.map((entry) => ({ name: entry.slice(0, -".json".length), mtime: statSync(join(this.snapshotDir, entry)).mtimeMs }))
			.sort((a, b) => b.mtime - a.mtime);
	}

	// Keeps the newest `keepSnapshots`, never deleting the one just taken.
	private pruneSnapshots(keep: string) {
		const older = this.listSnapshots().filter((entry) => entry.name !== keep);
		for (const entry of older.slice(this.keepSnapshots - 1)) {
			rmSync(join(this.snapshotDir, `${entry.name}.json`), { force: true });
		}
	}
}

// Handlers for the engine's `capabilities` option, so pages can use the
// store through `engine.invoke("storage.get", { key })` and friends.
export function storageCapabilities(store: EngineStore): Record<string, (args: unknown) => unknown> {
	return {
		"storage.get": (args) => store.get(argsOf(args).key as string),
		"storage.set": (args) => {
			const { key, value } = argsOf(args);
			store.set(key as string, value);
			return true;
		},
		"storage.delete": (args) => {
			store.delete(argsOf(args).key as string);
			return true;
		},
		"storage.batch": (args) => {
			store.batch(argsOf(args).ops as StorageOp[]);
			return true;
		},
		"storage.keys": () => store.keys(),
	};
}

// Args come from the page, so they can be anything.
function argsOf(args: unknown): { key?: unknown; value?: unknown; ops?: unknown } {
	if (typeof args !== "object" || args === null) throw new StorageError("E_BAD_ARGS", "args must be an object");
	return args;
}

function checkKey(key: unknown): asserts key is string {
	if (typeof key !== "string" || key.length === 0 || key.length > MAX_KEY_LENGTH) {
		throw new StorageError("E_BAD_ARGS", `key must be a non-empty string of up to ${MAX_KEY_LENGTH} characters`);
	}
}

function checkSnapshotName(name: unknown): asserts name is string {
	if (typeof name !== "string" || !SNAPSHOT_NAME.test(name)) {
		throw new StorageError("E_BAD_ARGS", `bad snapshot name ${JSON.stringify(name)}`);
	}
}

// Stores what reads back the same: a value that JSON can't represent is
// refused instead of being silently changed on disk.
function toJsonValue(value: unknown, key: string): unknown {
	let json: string | undefined;
	try {
		json = JSON.stringify(value);
	} catch {
		json = undefined;
	}
	if (json === undefined) throw new StorageError("E_BAD_ARGS", `value for ${JSON.stringify(key)} isn't JSON`);
	return JSON.parse(json);
}

function serialize(data: Map<string, unknown>): string {
	return JSON.stringify(Object.fromEntries(data));
}

function parse(json: string): Map<string, unknown> | null {
	try {
		const value: unknown = JSON.parse(json);
		if (typeof value !== "object" || value === null || Array.isArray(value)) return null;
		return new Map(Object.entries(value));
	} catch {
		return null;
	}
}

// Writes to a temp file, flushes it, then renames it over `file` and flushes
// the folder, so a crash or power loss leaves either the old contents or the
// new ones.
function writeAtomic(file: string, contents: string) {
	const tmp = `${file}.${randomBytes(4).toString("hex")}.tmp`;
	const fd = openSync(tmp, "w");
	try {
		writeSync(fd, contents);
		fsyncSync(fd);
	} finally {
		closeSync(fd);
	}
	renameSync(tmp, file);
	fsyncDir(dirname(file));
}

// The rename is only durable once the folder's entry is on disk. Windows
// can't open a folder to flush it, and NTFS journals the rename anyway.
function fsyncDir(dir: string) {
	if (process.platform === "win32") return;
	const fd = openSync(dir, "r");
	try {
		fsyncSync(fd);
	} finally {
		closeSync(fd);
	}
}

// Temp files left by a crash mid-write.
function sweepTemp(dir: string) {
	for (const folder of [dir, join(dir, "snapshots")]) {
		try {
			for (const entry of readdirSync(folder)) {
				if (entry.endsWith(".tmp")) rmSync(join(folder, entry), { force: true });
			}
		} catch {
			// Not created yet.
		}
	}
}
