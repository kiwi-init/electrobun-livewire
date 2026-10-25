import { afterAll, describe, expect, test } from "bun:test";
import { installFakeFFI } from "./fakeFFI";

let hostAvailable = false;
const fake = installFakeFFI({ isTrayHostAvailable: () => hostAvailable });
const { Tray } = await import("../core/Tray");
const { default: electrobunEventEmitter } = await import("../events/eventEmitter");

afterAll(() => fake.uninstall());

describe("Tray.hostAvailable", () => {
	test("asks the native layer each time", () => {
		hostAvailable = false;
		expect(Tray.hostAvailable()).toBe(false);
		hostAvailable = true;
		expect(Tray.hostAvailable()).toBe(true);
		expect(fake.callsTo("isTrayHostAvailable")).toHaveLength(2);
	});
});

describe("Tray.onHostChanged", () => {
	test("gets tray-host-changed until it's stopped", () => {
		const seen: boolean[] = [];
		const stop = Tray.onHostChanged((available) => seen.push(available));
		const emit = (available: boolean) =>
			electrobunEventEmitter.emitEvent(
				electrobunEventEmitter.events.tray.trayHostChanged({ available }),
			);

		emit(true);
		emit(false);
		stop();
		emit(true);

		expect(seen).toEqual([true, false]);
	});
});
