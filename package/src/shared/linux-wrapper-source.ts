import { readFileSync } from "node:fs";
import { join } from "node:path";

const linuxDir = join(import.meta.dirname, "../native/linux");

// The Linux wrapper's source as the compiler sees it: nativeWrapper.cpp with
// its wrapper/*.inc parts expanded in place. For tests that read the source.
export function readLinuxWrapperSource(): string {
	const stitched = readFileSync(join(linuxDir, "nativeWrapper.cpp"), "utf8");
	return stitched.replace(/^#include "(wrapper\/[^"]+)"\r?\n/gm, (_, part: string) =>
		readFileSync(join(linuxDir, part), "utf8"),
	);
}
