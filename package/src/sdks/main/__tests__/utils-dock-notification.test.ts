import { afterAll, describe, expect, test } from "bun:test";
import { installFakeFFI } from "./fakeFFI";

const fake = installFakeFFI({ getDockBadge: () => "4", getDockProgress: () => 0.25 });
const Utils = await import("../core/Utils");

afterAll(() => fake.uninstall());

function onPlatform<T>(platform: NodeJS.Platform, run: () => T): T {
	const original = Object.getOwnPropertyDescriptor(process, "platform")!;
	Object.defineProperty(process, "platform", { value: platform });
	try {
		return run();
	} finally {
		Object.defineProperty(process, "platform", original);
	}
}

describe("dock badge", () => {
	test("counts become labels; 0, empty and null remove the badge", () => {
		for (const [badge, label] of [
			[3, "3"],
			[2.7, "2"],
			["new", "new"],
			[0, null],
			[-1, null],
			["", null],
			[null, null],
		] as const) {
			Utils.setDockBadge(badge);
			expect(fake.callsTo("setDockBadge").at(-1)).toEqual({ label });
		}
		expect(Utils.getDockBadge()).toBe("4");
	});
});

describe("dock progress", () => {
	test("is clamped to 0..1, and null or NaN removes the bar", () => {
		for (const [progress, sent] of [
			[0.5, 0.5],
			[-0.2, 0],
			[1.7, 1],
			[Number.NaN, null],
			[null, null],
		] as const) {
			Utils.setDockProgress(progress);
			expect(fake.callsTo("setDockProgress").at(-1)).toEqual({ progress: sent });
		}
		expect(Utils.getDockProgress()).toBe(0.25);
	});
});

describe("showNotification", () => {
	test("on macOS it sends actions, reply and an id, and returns that id", () => {
		const id = onPlatform("darwin", () =>
			Utils.showNotification({
				title: "Ana",
				body: "Lunch?",
				actions: [{ id: "mute", title: "Mute", destructive: true }],
				reply: { placeholder: "Reply to Ana" },
			}),
		);
		expect(id).toMatch(/^[0-9a-f-]{36}$/);
		expect(fake.callsTo("showNotificationWithOptions").at(-1)).toEqual({
			id,
			title: "Ana",
			body: "Lunch?",
			subtitle: undefined,
			silent: undefined,
			actions: [{ id: "mute", title: "Mute", destructive: true }],
			reply: { placeholder: "Reply to Ana" },
		});
	});

	test("a given id is kept, so a later notification can replace it", () => {
		const id = onPlatform("darwin", () => Utils.showNotification({ id: "chat-42", title: "2 new messages" }));
		expect(id).toBe("chat-42");
		expect(fake.callsTo("showNotificationWithOptions").at(-1)?.["id"]).toBe("chat-42");
	});

	test("elsewhere it uses the plain notification and still returns an id", () => {
		const before = fake.callsTo("showNotificationWithOptions").length;
		const id = onPlatform("linux", () => Utils.showNotification({ title: "Hi", silent: true, actions: [{ id: "a", title: "A" }] }));
		expect(typeof id).toBe("string");
		expect(fake.callsTo("showNotification").at(-1)).toEqual({ title: "Hi", body: undefined, subtitle: undefined, silent: true });
		expect(fake.callsTo("showNotificationWithOptions").length).toBe(before);
	});
});
