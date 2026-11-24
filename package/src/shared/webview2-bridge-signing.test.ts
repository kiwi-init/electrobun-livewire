import { expect, test } from "bun:test";
import { join } from "node:path";

const core = await Bun.file(join(import.meta.dir, "../core/main.zig")).text();

function signer(): string {
	const lines = core.split("\n").map((line) => line.trim());
	const from = lines.indexOf("\\\\(function (token) {{");
	const to = lines.indexOf('\\\\}})("{s}");', from);
	if (from < 0 || to < 0) throw new Error("the preload's bridge signer isn't where this test reads it in main.zig");
	return lines
		.slice(from, to + 1)
		.map((line) => line.slice(2))
		.join("\n")
		.replaceAll("{{", "{")
		.replaceAll("}}", "}")
		.replace('"{s}"', '"token:"');
}

interface Bridge {
	postMessage(message: string): unknown;
}

function sign(window: Record<string, Bridge | undefined>): void {
	new Function("window", signer())(window);
}

function hostObject(sent: string[], probed: string[]): Bridge {
	return new Proxy(function () {} as unknown as Bridge, {
		get: (_target, name) => {
			if (name === "postMessage") {
				return (message: string) => {
					sent.push(message);
					return Promise.resolve(null);
				};
			}
			probed.push(String(name));
			return Promise.resolve(undefined);
		},
	});
}

test("a WebView2 host object, whose every property reads as a promise, is signed without being probed", () => {
	const sent: string[] = [];
	const probed: string[] = [];
	const window: Record<string, Bridge | undefined> = {
		__electrobunEventBridge: hostObject(sent, probed),
		__electrobunBunBridge: hostObject(sent, probed),
	};
	sign(window);
	expect(typeof window["__electrobunEventBridge"]).toBe("object");
	window["__electrobunEventBridge"]!.postMessage("event");
	window["__electrobunBunBridge"]!.postMessage("call");
	expect(sent).toEqual(["token:event", "token:call"]);
	expect(probed).toEqual([]);
});

test("a plain bridge object is signed once, however often the preload runs", () => {
	const sent: string[] = [];
	const window: Record<string, Bridge | undefined> = {
		__electrobunInternalBridge: { postMessage: (message) => sent.push(message) },
	};
	sign(window);
	sign(window);
	window["__electrobunInternalBridge"]!.postMessage("batch");
	expect(sent).toEqual(["token:batch"]);
});

test("a bridge the platform doesn't have stays missing", () => {
	const window: Record<string, Bridge | undefined> = {};
	sign(window);
	expect(window["__electrobunHostBridge"]).toBeUndefined();
});
