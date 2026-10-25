import { describe, expect, it } from "bun:test";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const nativeWrapper = readFileSync(
	join(import.meta.dirname, "../native/linux/nativeWrapper.cpp"),
	"utf8",
);

describe("Linux tray interaction contract", () => {
	it("matches the current menu-only Ayatana AppIndicator implementation", () => {
		expect(nativeWrapper).toContain("app_indicator_new");
		expect(nativeWrapper).toContain("app_indicator_set_menu");
		expect(nativeWrapper).toContain(
			"g_signal_connect(menuItem, \"activate\", G_CALLBACK(onMenuItemActivate)",
		);
		expect(
			/g_signal_connect(?:_data)?\s*\(\s*indicator\s*,\s*"activate"/.test(
				nativeWrapper,
			),
		).toBe(false);
	});
});
