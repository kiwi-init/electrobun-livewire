import { realpathSync } from "node:fs";
import { isAbsolute, sep } from "node:path";

// Why a page's request to drag `paths` out of its view is refused, or null
// when every path is an existing file or folder inside one of `roots`.
// Paths are compared after resolving symlinks and "..", so a link inside a
// root that points outside it doesn't get through.
export function fileDragRefusal(paths: unknown, roots: readonly string[]): string | null {
	if (roots.length === 0) return "this view has no fileDragRoots, so its pages can't drag files out";
	const list = typeof paths === "string" ? [paths] : paths;
	if (!Array.isArray(list) || list.length === 0) return "paths must be a path or a non-empty list of paths";
	const realRoots = roots.flatMap((root) => {
		const real = resolve(root);
		return real ? [real] : [];
	});
	for (const path of list) {
		if (typeof path !== "string" || !isAbsolute(path)) return `not an absolute path: ${String(path)}`;
		const real = resolve(path);
		if (!real) return `no such file: ${path}`;
		if (!realRoots.some((root) => real === root || real.startsWith(root.endsWith(sep) ? root : root + sep))) {
			return `outside this view's fileDragRoots: ${path}`;
		}
	}
	return null;
}

function resolve(path: string): string | null {
	try {
		return realpathSync(path);
	} catch {
		return null;
	}
}
