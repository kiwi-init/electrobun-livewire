import { describe, expect, it } from "bun:test";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { readLinuxWrapperSource } from "./linux-wrapper-source";

const nativeWrapper = readLinuxWrapperSource();
const sniTray = readFileSync(
	join(import.meta.dirname, "../native/linux/sni_tray.cpp"),
	"utf8",
);

describe("Linux tray interaction contract", () => {
	it("is a StatusNotifierItem whose left click is tray-clicked", () => {
		expect(nativeWrapper).toContain("sni::StatusNotifierItem");
		// Activate reports a click with an empty action, as on macOS and Windows.
		expect(nativeWrapper).toContain('if (handler) handler(id, "");');
		// Hosts send Activate on left click and show the Menu on right click.
		expect(sniTray).toContain(
			'if (g_strcmp0(property, "ItemIsMenu") == 0) return g_variant_new_boolean(FALSE);',
		);
	});

	it("sends a -symbolic image file by name from its folder, with pixels as the fallback", () => {
		expect(nativeWrapper).toContain("sni::symbolicIconFile(imagePath)");
		expect(nativeWrapper).toContain("sniItem->setThemedIcon(*symbolic, std::move(pixmap));");
		expect(sniTray).toContain(
			'if (g_strcmp0(property, "IconThemePath") == 0) return g_variant_new_string(state->iconThemePath.c_str());',
		);
	});

	it("falls back to menu-only AppIndicator when no watcher is on the bus", () => {
		expect(nativeWrapper).toContain("sni::watcherAvailable(bus) || !appIndicatorApi()");
		expect(nativeWrapper).toContain("app_indicator_new");
		expect(nativeWrapper).toContain("app_indicator_set_menu");
	});
});
