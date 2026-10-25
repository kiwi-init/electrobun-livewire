import { afterAll, describe, expect, test } from "bun:test";
import { installFakeFFI } from "./fakeFFI";

const fake = installFakeFFI();
const { BrowserView } = await import("../core/BrowserView");
const { BrowserWindow } = await import("../core/BrowserWindow");

afterAll(() => fake.uninstall());

const policy = "default-src views:; img-src 'none'";

describe("contentSecurityPolicy", () => {
	test("reaches createWebview from a BrowserView and from a BrowserWindow", () => {
		new BrowserView({ url: "views://a/index.html", contentSecurityPolicy: policy });
		expect(fake.callsTo("createWebview").at(-1)?.["contentSecurityPolicy"]).toBe(policy);

		new BrowserWindow({ url: "views://b/index.html", contentSecurityPolicy: policy });
		expect(fake.callsTo("createWebview").at(-1)?.["contentSecurityPolicy"]).toBe(policy);
	});

	test("defaults to none, and setContentSecurityPolicy updates and clears it", () => {
		const view = new BrowserView({ url: "views://c/index.html" });
		expect(fake.callsTo("createWebview").at(-1)?.["contentSecurityPolicy"]).toBeNull();

		view.setContentSecurityPolicy(policy);
		view.setContentSecurityPolicy(null);
		expect(fake.callsTo("webviewSetContentSecurityPolicy").slice(-2)).toEqual([
			{ id: view.id, policy },
			{ id: view.id, policy: null },
		]);
	});
});

describe("contentSecurityPolicy header safety", () => {
	const unsafe = ["default-src 'self'\r\nSet-Cookie: x=1", "default-src 'self'\nX: y", "default-src 'self'\0"];

	test("the constructors refuse CR, LF and NUL", () => {
		for (const policy of unsafe) {
			expect(() => new BrowserView({ url: "views://d/index.html", contentSecurityPolicy: policy })).toThrow(/CR, LF or NUL/);
			expect(() => new BrowserWindow({ url: "views://e/index.html", contentSecurityPolicy: policy })).toThrow(/CR, LF or NUL/);
		}
	});

	test("setContentSecurityPolicy refuses them and keeps the old policy", () => {
		const view = new BrowserView({ url: "views://f/index.html", contentSecurityPolicy: policy });
		const calls = fake.callsTo("webviewSetContentSecurityPolicy").length;
		for (const bad of unsafe) {
			expect(() => view.setContentSecurityPolicy(bad)).toThrow(/CR, LF or NUL/);
		}
		expect(view.contentSecurityPolicy).toBe(policy);
		expect(fake.callsTo("webviewSetContentSecurityPolicy").length).toBe(calls);
	});
});
