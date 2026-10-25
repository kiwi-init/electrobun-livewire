import { describe, expect, test } from "bun:test";
import { ENGINE_CONTRACT_GLOBAL } from "../../../shared/appengine-protocol";
import { Capabilities, contractRangeError, contractRefusal } from "./contract";
import type { UiManifest } from "./format";
import { StorageError } from "./storage";

function manifest(requiresContract: string): UiManifest {
	return {
		format: "livewire-manifest/1",
		keyId: "k",
		seq: 1,
		uiVersion: "ui-7",
		bundleUrl: "bundles/ui-7.uipack",
		sha256: "a".repeat(64),
		requiresContract,
		dataSchema: 1,
		publishedAt: 0,
		signature: "",
	};
}

describe("contract version gate", () => {
	test("accepts UIs whose range the engine satisfies", () => {
		expect(contractRefusal("1.2.0", manifest("^1.0.0"))).toBeNull();
		expect(contractRefusal("1.2.0", manifest(">=1.2.0 <2"))).toBeNull();
		expect(contractRefusal("1.2.0", manifest("1.x || 2.x"))).toBeNull();
	});

	test("refuses UIs that need a newer or different contract", () => {
		expect(contractRefusal("1.2.0", manifest("^1.3.0"))).toBe(
			'UI ui-7 requires contract "^1.3.0", but this engine implements 1.2.0',
		);
		expect(contractRefusal("1.2.0", manifest(">=2"))).not.toBeNull();
		expect(contractRefusal("2.0.0", manifest("^1.0.0"))).not.toBeNull();
	});

	test("refuses ranges it can't parse, which Bun.semver would treat as *", () => {
		for (const range of ["banana", "^", "", "  ", ">=", "latest", "^1.0.0 || banana", "1.0.0 ||", ">=1.0.0<2", "v1.0.0", "1.2.3.4"]) {
			expect(contractRangeError(range)).not.toBeNull();
			expect(contractRefusal("1.2.0", manifest(range))).toContain("UI ui-7: requiresContract");
		}
	});

	test("knows the node-semver range forms", () => {
		for (const range of ["*", "x", "1", "1.x", "1.2.x", "^0.0.1", "~1.2", "~>1.2", "=1.2.0", "1.2.0-rc.1", "1.2.0+build.5", ">= 1.0.0 < 2", "1.2.3 - 2.0.0", "^1.0.0 || ^2.0.0", " ^1.0.0 "]) {
			expect(contractRangeError(range)).toBeNull();
		}
	});
});

describe("capability allowlist", () => {
	const builtIns = { "engine.info": () => "info" };

	test("needs a strict semver version", () => {
		expect(() => new Capabilities("1.2", builtIns)).toThrow();
		expect(() => new Capabilities("v1.2.0", builtIns)).toThrow();
		expect(new Capabilities("1.2.0", builtIns).contract.version).toBe("1.2.0");
	});

	test("app capabilities can't replace built-ins and must be functions", () => {
		expect(() => new Capabilities("1.0.0", builtIns, { "engine.info": () => "mine" })).toThrow(/built into AppEngine/);
		expect(() => new Capabilities("1.0.0", builtIns, { "notify": "yes" as never })).toThrow(/must be a function/);
	});

	test("the contract lists every capability, sorted and frozen", () => {
		const caps = new Capabilities("1.0.0", builtIns, { "window.setTitle": () => {}, "app.ping": () => {} });
		expect(caps.contract.capabilities).toEqual(["app.ping", "engine.info", "window.setTitle"]);
		expect(Object.isFrozen(caps.contract)).toBe(true);
		expect(Object.isFrozen(caps.contract.capabilities)).toBe(true);
		expect(caps.has("app.ping")).toBe(true);
		expect(caps.has("app.pong")).toBe(false);
	});

	test("invokes listed capabilities with their args", async () => {
		const caps = new Capabilities("1.0.0", builtIns, { "app.echo": async (args) => args });
		expect(await caps.invoke("app.echo", { a: 1 })).toEqual({ ok: true, value: { a: 1 } });
		expect(await caps.invoke("engine.info", {})).toEqual({ ok: true, value: "info" });
	});

	test("inherited and non-string names are not capabilities", async () => {
		const caps = new Capabilities("1.0.0", builtIns, { "app.echo": (args) => args });
		for (const name of ["constructor", "toString", "__proto__", "hasOwnProperty", "valueOf", 7, null, ["app.echo"]]) {
			expect(caps.has(name)).toBe(false);
			const result = await caps.invoke(name, {});
			expect(result).toMatchObject({ ok: false, code: "E_NO_CAPABILITY" });
		}
	});

	test("failures keep a handler's E_ code, anything else is E_FAILED", async () => {
		const caps = new Capabilities("1.0.0", builtIns, {
			"storage.set": () => {
				throw new StorageError("E_QUOTA", "storage is full");
			},
			"app.bad": () => {
				throw new Error("boom");
			},
			"app.odd": () => {
				throw Object.assign(new Error("odd"), { code: "ENOENT" });
			},
			"app.string": () => Promise.reject("nope"),
		});
		expect(await caps.invoke("storage.set", {})).toEqual({ ok: false, code: "E_QUOTA", message: "storage is full" });
		expect(await caps.invoke("app.bad", {})).toEqual({ ok: false, code: "E_FAILED", message: "boom" });
		expect(await caps.invoke("app.odd", {})).toMatchObject({ ok: false, code: "E_FAILED" });
		expect(await caps.invoke("app.string", {})).toEqual({ ok: false, code: "E_FAILED", message: "nope" });
	});

	test("the page prelude hands the contract to the facade", () => {
		const caps = new Capabilities("1.4.0", builtIns, { "app.ping": () => {} });
		const page: Record<string, unknown> = {};
		new Function("globalThis", caps.pagePrelude())(page);
		expect(page[ENGINE_CONTRACT_GLOBAL]).toEqual({ version: "1.4.0", capabilities: ["app.ping", "engine.info"] });
	});
});
