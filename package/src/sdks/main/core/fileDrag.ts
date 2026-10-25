import { realpathSync } from "node:fs";
import { isAbsolute, sep } from "node:path";

// Checks a page's request to drag `paths` out of its view: every path must be
// an existing file or folder inside one of `roots`, compared after resolving
// symlinks and "..", so a link inside a root that points outside it doesn't
// get through. Returns the resolved paths, which are what gets dragged (a
// link swapped after the check can't redirect the drag), or why it's refused.
export function resolveFileDrag(
	paths: unknown,
	roots: readonly string[],
): { paths: string[] } | { refusal: string } {
	if (roots.length === 0) return { refusal: "this view has no fileDragRoots, so its pages can't drag files out" };
	const list = typeof paths === "string" ? [paths] : paths;
	if (!Array.isArray(list) || list.length === 0) return { refusal: "paths must be a path or a non-empty list of paths" };
	const realRoots = roots.flatMap((root) => {
		const real = resolve(root);
		return real ? [real] : [];
	});
	const resolved: string[] = [];
	for (const path of list) {
		if (typeof path !== "string" || !isAbsolute(path)) return { refusal: `not an absolute path: ${String(path)}` };
		const real = resolve(path);
		if (!real) return { refusal: `no such file: ${path}` };
		if (!realRoots.some((root) => real === root || real.startsWith(root.endsWith(sep) ? root : root + sep))) {
			return { refusal: `outside this view's fileDragRoots: ${path}` };
		}
		resolved.push(real);
	}
	return { paths: resolved };
}

function resolve(path: string): string | null {
	try {
		return realpathSync(path);
	} catch {
		return null;
	}
}
