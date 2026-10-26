import { afterAll, describe, expect, test } from "bun:test";
import { installFakeFFI } from "./fakeFFI";

let accentColor: string | null | undefined = "#007aff";
const fake = installFakeFFI({ getAccentColor: () => accentColor });
const Utils = await import("../core/Utils");
const { default: electrobunEventEmitter } = await import("../events/eventEmitter");

afterAll(() => fake.uninstall());

describe("getAccentColor", () => {
	test("returns the native hex, and null when there's none", () => {
		expect(Utils.getAccentColor()).toBe("#007aff");
		accentColor = null;
		expect(Utils.getAccentColor()).toBe(null);
		accentColor = undefined;
		expect(Utils.getAccentColor()).toBe(null);
	});
});

describe("accent-color-changed", () => {
	test("reaches listeners with the new color", () => {
		const seen: (string | null)[] = [];
		const listener = (event: { data: { color: string | null } }) => seen.push(event.data.color);
		electrobunEventEmitter.on("accent-color-changed", listener);
		try {
			for (const color of ["#ff0000", null]) {
				electrobunEventEmitter.emitEvent(electrobunEventEmitter.events.app.accentColorChanged({ color }));
			}
		} finally {
			electrobunEventEmitter.off("accent-color-changed", listener);
		}
		expect(seen).toEqual(["#ff0000", null]);
	});
});
