import { afterEach, describe, expect, test } from "bun:test";
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync, utimesSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { EngineStore, StorageError, storageCapabilities } from "./storage";

const dirs: string[] = [];

function tempDir() {
	const dir = mkdtempSync(join(tmpdir(), "lw-storage-"));
	dirs.push(dir);
	return dir;
}

afterEach(() => {
	for (const dir of dirs.splice(0)) rmSync(dir, { recursive: true, force: true });
});

function code(fn: () => unknown): string | null {
	try {
		fn();
		return null;
	} catch (error) {
		return error instanceof StorageError ? error.code : `not a StorageError: ${error}`;
	}
}

describe("EngineStore", () => {
	test("keeps values across reopening", () => {
		const dir = tempDir();
		const store = new EngineStore({ dir });
		store.set("todos", [{ text: "milk", done: false }]);
		store.set("count", 3);
		store.delete("count");
		const reopened = new EngineStore({ dir });
		expect(reopened.get("todos")).toEqual([{ text: "milk", done: false }]);
		expect(reopened.get("count")).toBeNull();
		expect(reopened.keys()).toEqual(["todos"]);
	});

	test("returns copies, so callers can't change stored values in place", () => {
		const store = new EngineStore({ dir: tempDir() });
		const value = { list: [1] };
		store.set("v", value);
		value.list.push(2);
		(store.get("v") as { list: number[] }).list.push(3);
		expect(store.get("v")).toEqual({ list: [1] });
	});

	test("a batch applies all its ops or none", () => {
		const dir = tempDir();
		const store = new EngineStore({ dir });
		store.set("a", 1);
		store.batch([
			{ op: "set", key: "b", value: 2 },
			{ op: "delete", key: "a" },
		]);
		expect(store.keys().sort()).toEqual(["b"]);

		const bad = [
			{ op: "set", key: "c", value: 3 },
			{ op: "rename", key: "b" },
		] as never;
		expect(code(() => store.batch(bad))).toBe("E_BAD_ARGS");
		expect(code(() => store.batch([{ op: "set", key: "c", value: 3 }, { op: "set", key: "", value: 4 }]))).toBe("E_BAD_ARGS");
		expect(store.get("c")).toBeNull();
		expect(new EngineStore({ dir }).keys()).toEqual(["b"]);
	});

	test("refuses a write over the quota and keeps the old data", () => {
		const dir = tempDir();
		const store = new EngineStore({ dir, maxBytes: 1024 });
		store.set("small", "x".repeat(100));
		expect(code(() => store.set("big", "x".repeat(2000)))).toBe("E_QUOTA");
		expect(store.get("big")).toBeNull();
		expect(new EngineStore({ dir }).get("small")).toBe("x".repeat(100));
	});

	test("counts the quota in bytes, not characters", () => {
		const store = new EngineStore({ dir: tempDir(), maxBytes: 1024 });
		// 400 characters, 1200 bytes in UTF-8.
		expect(code(() => store.set("k", "€".repeat(400)))).toBe("E_QUOTA");
	});

	test("refuses values JSON can't hold instead of changing them", () => {
		const store = new EngineStore({ dir: tempDir() });
		expect(code(() => store.set("k", undefined))).toBe("E_BAD_ARGS");
		expect(code(() => store.set("k", () => 1))).toBe("E_BAD_ARGS");
		expect(code(() => store.set("k", 10n))).toBe("E_BAD_ARGS");
		const cyclic: Record<string, unknown> = {};
		cyclic["self"] = cyclic;
		expect(code(() => store.set("k", cyclic))).toBe("E_BAD_ARGS");
		expect(store.keys()).toEqual([]);
	});

	test("treats keys like __proto__ as plain keys", () => {
		const dir = tempDir();
		const store = new EngineStore({ dir });
		store.set("__proto__", { polluted: true });
		store.set("constructor", 1);
		const reopened = new EngineStore({ dir });
		expect(reopened.get("__proto__")).toEqual({ polluted: true });
		expect(reopened.get("constructor")).toBe(1);
		expect(({} as Record<string, unknown>)["polluted"]).toBeUndefined();
	});

	test("refuses bad keys", () => {
		const store = new EngineStore({ dir: tempDir() });
		expect(code(() => store.get(""))).toBe("E_BAD_ARGS");
		expect(code(() => store.get(7 as never))).toBe("E_BAD_ARGS");
		expect(code(() => store.set("k".repeat(257), 1))).toBe("E_BAD_ARGS");
		expect(code(() => store.batch("nope" as never))).toBe("E_BAD_ARGS");
		expect(code(() => store.batch([null as never]))).toBe("E_BAD_ARGS");
	});

	test("moves an unreadable file aside and starts empty", () => {
		const dir = tempDir();
		writeFileSync(join(dir, "storage.json"), "{not json");
		const store = new EngineStore({ dir });
		expect(store.keys()).toEqual([]);
		const aside = readdirSync(dir).find((entry) => entry.startsWith("storage.json.corrupt-"));
		expect(aside).toBeDefined();
		expect(readFileSync(join(dir, aside!), "utf8")).toBe("{not json");
	});

	test("sweeps temp files a crash left behind", () => {
		const dir = tempDir();
		new EngineStore({ dir }).snapshot("before-a");
		writeFileSync(join(dir, "storage.json.dead.tmp"), "{");
		writeFileSync(join(dir, "snapshots", "before-b.json.dead.tmp"), "{");
		new EngineStore({ dir });
		expect(readdirSync(dir).filter((entry) => entry.endsWith(".tmp"))).toEqual([]);
		expect(readdirSync(join(dir, "snapshots"))).toEqual(["before-a.json"]);
	});
});

describe("snapshots", () => {
	test("restore puts a snapshot back, on disk too", () => {
		const dir = tempDir();
		const store = new EngineStore({ dir });
		store.set("todos", ["milk"]);
		store.snapshot("before-abc");
		// The new UI migrates the data, then fails to boot.
		store.batch([
			{ op: "set", key: "todos", value: { v: 2, items: ["milk"] } },
			{ op: "set", key: "schema", value: 2 },
		]);
		expect(store.restore("before-abc")).toBe(true);
		expect(store.get("todos")).toEqual(["milk"]);
		expect(store.get("schema")).toBeNull();
		expect(new EngineStore({ dir }).get("todos")).toEqual(["milk"]);
	});

	test("a snapshot of an empty store restores to empty", () => {
		const store = new EngineStore({ dir: tempDir() });
		store.snapshot("before-first");
		store.set("k", 1);
		expect(store.restore("before-first")).toBe(true);
		expect(store.keys()).toEqual([]);
	});

	test("restore of a missing snapshot returns false and changes nothing", () => {
		const store = new EngineStore({ dir: tempDir() });
		store.set("k", 1);
		expect(store.restore("before-none")).toBe(false);
		expect(store.get("k")).toBe(1);
	});

	test("restore of an unreadable snapshot returns false and changes nothing", () => {
		const dir = tempDir();
		const store = new EngineStore({ dir });
		store.set("k", 1);
		store.snapshot("before-x");
		writeFileSync(join(dir, "snapshots", "before-x.json"), "garbage");
		expect(store.restore("before-x")).toBe(false);
		expect(store.get("k")).toBe(1);
	});

	test("snapshot names can't leave the snapshot folder", () => {
		const dir = tempDir();
		const store = new EngineStore({ dir });
		for (const name of ["../storage", "a/b", ".hidden", "", "x".repeat(200)]) {
			expect(code(() => store.snapshot(name))).toBe("E_BAD_ARGS");
			expect(code(() => store.restore(name))).toBe("E_BAD_ARGS");
		}
		expect(existsSync(join(dir, "storage.json.json"))).toBe(false);
	});

	test("keeps only the newest snapshots", () => {
		const dir = tempDir();
		const store = new EngineStore({ dir, keepSnapshots: 2 });
		const at = (name: string, seconds: number) => {
			store.snapshot(name);
			utimesSync(join(dir, "snapshots", `${name}.json`), seconds, seconds);
		};
		at("before-1", 1000);
		at("before-2", 2000);
		at("before-3", 3000);
		expect(store.snapshots().sort()).toEqual(["before-2", "before-3"]);
		// Taking a snapshot again keeps it, even when it was the oldest.
		at("before-2", 4000);
		expect(store.snapshots().sort()).toEqual(["before-2", "before-3"]);
	});

	test("matches the EngineStorage hook the boot lifecycle calls", () => {
		const store = new EngineStore({ dir: tempDir() });
		const hook: { snapshot(name: string): void; restore(name: string): boolean } = store;
		hook.snapshot("before-0123abcd");
		expect(hook.restore("before-0123abcd")).toBe(true);
	});
});

describe("storageCapabilities", () => {
	test("exposes the store to pages", () => {
		const store = new EngineStore({ dir: tempDir() });
		const caps = storageCapabilities(store);
		expect(caps["storage.set"]!({ key: "a", value: 1 })).toBe(true);
		expect(caps["storage.batch"]!({ ops: [{ op: "set", key: "b", value: 2 }] })).toBe(true);
		expect(caps["storage.get"]!({ key: "a" })).toBe(1);
		expect(caps["storage.delete"]!({ key: "a" })).toBe(true);
		expect(caps["storage.keys"]!(undefined)).toEqual(["b"]);
	});

	test("refuses args that aren't an object", () => {
		const caps = storageCapabilities(new EngineStore({ dir: tempDir() }));
		expect(code(() => caps["storage.get"]!(null))).toBe("E_BAD_ARGS");
		expect(code(() => caps["storage.set"]!("a"))).toBe("E_BAD_ARGS");
		expect(code(() => caps["storage.batch"]!({ ops: "x" }))).toBe("E_BAD_ARGS");
	});
});
