import { afterAll, describe, expect, test } from "bun:test";
import { installFakeFFI } from "./fakeFFI";

const fake = installFakeFFI();
const { BrowserWindow } = await import("../core/BrowserWindow");

afterAll(() => fake.uninstall());

describe("BrowserWindow activation token", () => {
	test("passes another launch's token to native for the next show or activate", () => {
		const win = new BrowserWindow({ url: null });
		win.setActivationToken("kwin-1234_TIME5678");
		expect(fake.callsTo("setWindowActivationToken")).toEqual([
			{ winId: win.id, token: "kwin-1234_TIME5678" },
		]);
	});

	test("an empty token is ignored", () => {
		const win = new BrowserWindow({ url: null });
		const before = fake.callsTo("setWindowActivationToken").length;
		win.setActivationToken("");
		expect(fake.callsTo("setWindowActivationToken").length).toBe(before);
	});
});
