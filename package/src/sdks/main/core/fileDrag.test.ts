import { describe, expect, test } from "bun:test";
import { mkdirSync, mkdtempSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { realpathSync } from "node:fs";
import { resolveFileDrag } from "./fileDrag";

const fileDragRefusal = (paths: unknown, roots: string[]) => {
	const checked = resolveFileDrag(paths, roots);
	return "refusal" in checked ? checked.refusal : null;
};

function fixture() {
	const dir = mkdtempSync(join(tmpdir(), "file-drag-"));
	const root = join(dir, "exports");
	const outside = join(dir, "private");
	mkdirSync(root);
	mkdirSync(outside);
	writeFileSync(join(root, "report.pdf"), "pdf");
	writeFileSync(join(outside, "secret.txt"), "secret");
	mkdirSync(join(root, "album"));
	symlinkSync(join(outside, "secret.txt"), join(root, "link.txt"));
	// A sibling whose name starts with the root's.
	mkdirSync(join(dir, "exports-old"));
	writeFileSync(join(dir, "exports-old", "a.txt"), "a");
	return { dir, root, outside };
}

describe("resolveFileDrag", () => {
	test("drags the resolved paths, not the ones the page sent", () => {
		const { root } = fixture();
		symlinkSync(join(root, "report.pdf"), join(root, "shortcut.pdf"));
		expect(resolveFileDrag([join(root, "shortcut.pdf"), `${root}/album/../report.pdf`], [root])).toEqual({
			paths: [realpathSync(join(root, "report.pdf")), realpathSync(join(root, "report.pdf"))],
		});
	});


	test("allows files and folders inside a root", () => {
		const { root } = fixture();
		expect(fileDragRefusal(join(root, "report.pdf"), [root])).toBe(null);
		expect(fileDragRefusal([join(root, "report.pdf"), join(root, "album")], [root])).toBe(null);
		expect(fileDragRefusal(root, [root])).toBe(null);
	});

	test("refuses everything when the view has no roots", () => {
		const { root } = fixture();
		expect(fileDragRefusal(join(root, "report.pdf"), [])).toContain("no fileDragRoots");
	});

	test("refuses paths outside the roots, however they're spelled", () => {
		const { dir, root, outside } = fixture();
		expect(fileDragRefusal(join(outside, "secret.txt"), [root])).toContain("outside");
		expect(fileDragRefusal(`${root}/../private/secret.txt`, [root])).toContain("outside");
		expect(fileDragRefusal(join(root, "link.txt"), [root])).toContain("outside");
		expect(fileDragRefusal(join(dir, "exports-old", "a.txt"), [root])).toContain("outside");
		// One bad path refuses the whole drag.
		expect(fileDragRefusal([join(root, "report.pdf"), join(outside, "secret.txt")], [root])).toContain("outside");
	});

	test("refuses malformed input and missing files", () => {
		const { root } = fixture();
		expect(fileDragRefusal([], [root])).toContain("non-empty");
		expect(fileDragRefusal(42, [root])).toContain("non-empty");
		expect(fileDragRefusal(["report.pdf"], [root])).toContain("not an absolute path");
		expect(fileDragRefusal([7], [root])).toContain("not an absolute path");
		expect(fileDragRefusal(join(root, "missing.pdf"), [root])).toContain("no such file");
	});
});
