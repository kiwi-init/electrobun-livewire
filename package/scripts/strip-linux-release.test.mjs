import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, truncateSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { stripLinuxRelease } from "./strip-linux-release.mjs";

const elf = (size) => Buffer.concat([Buffer.from([0x7f, 0x45, 0x4c, 0x46]), Buffer.alloc(size - 4)]);

test("strips every ELF file in the release except CEF's own", () => {
	const dist = mkdtempSync(join(tmpdir(), "strip-linux-"));
	try {
		writeFileSync(join(dist, "bspatch"), elf(4000));
		writeFileSync(join(dist, "libNativeWrapper.so"), elf(2000));
		writeFileSync(join(dist, "main.js"), "console.log(1)");
		mkdirSync(join(dist, "lib"));
		writeFileSync(join(dist, "lib", "libasar.so"), elf(1000));
		mkdirSync(join(dist, "cef"));
		writeFileSync(join(dist, "cef", "libcef.so"), elf(8000));

		const calls = [];
		const result = stripLinuxRelease(dist, {
			strip: "llvm-strip",
			// Stands in for strip: halves the file.
			run: (command, args) => {
				calls.push([command, ...args]);
				truncateSync(args.at(-1), 1000);
			},
			log: () => {},
		});

		assert.deepEqual(result.files.sort(), ["bspatch", "lib/libasar.so", "libNativeWrapper.so"]);
		assert.ok(calls.every(([command, flag]) => command === "llvm-strip" && flag === "--strip-unneeded"));
		assert.equal(result.before, 7000);
		assert.equal(result.after, 3000);
	} finally {
		rmSync(dist, { recursive: true, force: true });
	}
});
