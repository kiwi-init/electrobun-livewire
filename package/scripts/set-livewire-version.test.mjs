import assert from "node:assert/strict";
import test from "node:test";

import { isLivewireVersion, nextLivewireVersion } from "./set-livewire-version.mjs";

test("Livewire versions are CalVer YYYY.M.PATCH", () => {
	for (const version of ["2026.10.0", "2026.10.1", "2026.1.0", "2027.12.31", "2099.9.120"]) {
		assert.equal(isLivewireVersion(version), true, version);
	}
	for (const version of [
		"2.1.0-livewire.rc.5",
		"2.1.0",
		"v2026.10.0",
		"2026.10",
		"2026.10.0-rc.1",
		"2026.10.0\n",
		"2026.0.0",
		"2026.13.0",
		"2026.01.0",
		"2026.10.01",
		"1999.10.0",
		"3026.10.0",
		undefined,
		2026.1,
	]) {
		assert.equal(isLivewireVersion(version), false, String(version));
	}
});

test("the next version bumps PATCH within a month and restarts it in a new one", () => {
	const october = new Date("2026-10-27T12:00:00Z");
	assert.equal(nextLivewireVersion("2026.10.0", october), "2026.10.1");
	assert.equal(nextLivewireVersion("2026.10.9", october), "2026.10.10");
	assert.equal(nextLivewireVersion("2026.9.4", october), "2026.10.0");
	assert.equal(nextLivewireVersion("2025.10.4", october), "2026.10.0");
	assert.equal(nextLivewireVersion("2.1.0-livewire.rc.5", october), "2026.10.0");
	assert.equal(nextLivewireVersion("2026.12.3", new Date("2027-01-01T00:00:00Z")), "2027.1.0");
});
