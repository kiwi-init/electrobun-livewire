// Records an app's own bun:ffi declarations from its Bun bundles, for the JSC
// host's glue (gen.ts --extra): every dlopen(path, symbols) and JSCallback
// signature the bundles contain. It reads the code instead of running it, so a
// declaration inside a function or behind a platform check counts too.
//   bun extract-app.ts [--os macos|linux] [--libs <dir>] [--sdk <sdks/main>] <bundle.js>... > app-ffi.json
// The bundles hold the SDK's own dlopen calls too, which its glue covers:
// modules under the devkit's api/ (Bun marks each module with a "// path"
// comment) are skipped, and --sdk also skips a table the SDK's proc/native.ts
// declares, for a bundle without the marks.
// A library is keyed as the host keys it at run time (libkiwi-shell.dylib is
// kiwi-shell): its path is read from the string and template literals of the
// path argument, following constants it names; ${...} after a dot is the
// platform's suffix. A path read from data (a Worker's message) is matched to
// the library in --libs (the app's MacOS or bin folder) that exports all its
// symbols. A site whose library is only another platform's is skipped; one
// neither way resolves fails the extraction.
import { FFIType } from "bun:ffi";
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

const argv = process.argv.slice(2);
const flag = (name: string): string | undefined => {
	const i = argv.indexOf(name);
	if (i < 0) return undefined;
	const value = argv[i + 1];
	argv.splice(i, 2);
	return value;
};
const os = flag("--os") ?? (process.platform === "darwin" ? "macos" : "linux");
const libsDir = flag("--libs");
const sdkDir = flag("--sdk");
const files = argv;
if (files.length === 0) {
	console.error("usage: extract-app.ts [--os macos|linux] [--libs <dir>] <bundle.js>...");
	process.exit(2);
}
const suffix = os === "macos" ? "dylib" : "so";
const libraryKey = (path: string) => path.split("/").pop()!.replace(/^lib/, "").replace(/\.(dylib|so|dll)$/, "");
const typeNames = new Map<number, string>(Object.entries(FFIType).flatMap(([name, v]) => (typeof v === "number" ? [[v, name] as [number, string]] : [])));

// Reads one JS expression or argument list starting at `at`, skipping strings,
// template literals, regexes are rare enough in declarations to ignore.
function balancedEnd(src: string, at: number, stops: string): number {
	let depth = 0;
	for (let i = at; i < src.length; i++) {
		const ch = src[i]!;
		if (ch === '"' || ch === "'") {
			for (i++; i < src.length && src[i] !== ch; i++) if (src[i] === "\\") i++;
		} else if (ch === "`") {
			for (i++; i < src.length && src[i] !== "`"; i++) {
				if (src[i] === "\\") i++;
				else if (src[i] === "$" && src[i + 1] === "{") i = balancedEnd(src, i + 2, "}");
			}
		} else if (ch === "/" && src[i + 1] === "/") {
			i = src.indexOf("\n", i);
			if (i < 0) return src.length;
		} else if (ch === "/" && src[i + 1] === "*") {
			i = src.indexOf("*/", i + 2) + 1;
		} else if ("([{".includes(ch)) depth++;
		else if (")]}".includes(ch)) {
			if (depth === 0 && stops.includes(ch)) return i;
			depth--;
		} else if (depth === 0 && stops.includes(ch)) return i;
	}
	return src.length;
}

function callArgs(src: string, open: number): string[] {
	const args: string[] = [];
	let at = open + 1;
	for (;;) {
		const end = balancedEnd(src, at, ",)");
		args.push(src.slice(at, end).trim());
		if (end >= src.length || src[end] === ")") return args;
		at = end + 1;
	}
}

function declaration(src: string, name: string): string | undefined {
	const m = new RegExp("(?:const|let|var)\\s+" + name.replace(/\$/g, "\\$") + "\\s*=\\s*").exec(src);
	if (!m) return undefined;
	const at = m.index + m[0].length;
	return src.slice(at, balancedEnd(src, at, ",;\n"));
}

// An object literal of FFI types: any name it reads (FFIType, an alias, a
// namespace) stands for a value whose members are their own names.
const typeOf = new Proxy({}, { get: (_, member) => String(member) });
const scope = new Proxy({}, { has: (_, key) => key !== "JSON", get: (_, key) => (key === Symbol.unscopables ? undefined : typeOf) });
function literal(src: string, file: string, expression: string): unknown {
	const named = /^[\w$]+$/.test(expression) ? declaration(src, expression) : undefined;
	const text = named ?? expression;
	try {
		return new Function("scope", "with (scope) { return (" + text + "); }")(scope);
	} catch (error) {
		throw new Error("extract-app: can't read the declaration " + text.slice(0, 200) + " in " + file + ": " + error);
	}
}

const typeName = (t: unknown) => (typeof t === "number" ? (typeNames.get(t) ?? String(t)) : String(t));
type Decl = { args: string[]; returns: string };
const decl = (d: { args?: unknown[]; returns?: unknown } | undefined): Decl => ({ args: (d?.args ?? []).map(typeName), returns: typeName(d?.returns ?? "void") });

// The library names a path expression can take: on this platform, and on any.
function libraryPaths(src: string, expression: string, depth = 0): { here: string[]; any: string[] } {
	const pieces: string[] = [];
	for (const m of expression.matchAll(/"((?:[^"\\]|\\.)*)"|'((?:[^'\\]|\\.)*)'|`((?:[^`\\]|\\.)*)`/g)) {
		pieces.push((m[1] ?? m[2] ?? m[3] ?? "").replace(/\.\$\{[^}]*\}/g, "." + suffix));
	}
	if (depth < 3) {
		for (const m of expression.matchAll(/(?<![\w$."'`])([A-Za-z_$][\w$]*)(?![\w$]*\s*[:(])/g)) {
			const init = declaration(src, m[1]!);
			if (init) pieces.push(...libraryPaths(src, init, depth + 1).any);
		}
	}
	const other = os === "macos" ? /\.so(\.\d+)*$/ : /\.dylib$|^\/System\//;
	const any = pieces.filter((p) => /(^|\/)lib[^/\s]*$|\.(so|dylib)(\.\d+)*$|^\/[^\s]+$/.test(p));
	return { here: any.filter((p) => !other.test(p)), any };
}

function exporting(symbols: string[]): string | undefined {
	if (!libsDir) return undefined;
	for (const name of readdirSync(libsDir)) {
		if (!name.endsWith("." + suffix)) continue;
		const nm = Bun.spawnSync(os === "macos" ? ["nm", "-gU", join(libsDir, name)] : ["nm", "-D", "--defined-only", join(libsDir, name)]);
		const exported = new Set(nm.stdout.toString().split("\n").map((l) => l.trim().split(/\s+/).pop()?.replace(/^_/, "")));
		if (symbols.every((s) => exported.has(s))) return name;
	}
	return undefined;
}

const sdkSymbols = new Set<string>();
if (sdkDir) {
	const sdk = Bun.spawnSync([process.execPath, join(import.meta.dir, "extract.ts"), sdkDir], { stderr: "inherit" });
	if (sdk.exitCode !== 0) process.exit(1);
	for (const lib of (JSON.parse(sdk.stdout.toString()) as { libraries: { symbols: Record<string, unknown> }[] }).libraries) for (const name of Object.keys(lib.symbols)) sdkSymbols.add(name);
}

// A symbol table the SDK builds itself (not a literal): every key it names is
// the SDK's.
function sdkCovers(src: string, expression: string): boolean {
	const text = /^[\w$]+$/.test(expression) ? (declaration(src, expression) ?? expression) : expression;
	const keys = [...text.matchAll(/([A-Za-z_$][\w$]*)\s*:\s*\{/g)].map((m) => m[1]!);
	return keys.length > 0 && keys.every((k) => sdkSymbols.has(k));
}

const libraries = new Map<string, { path: string; symbols: Record<string, Decl> }>();
const callbacks = new Map<string, Decl>();
const unresolved: string[] = [];
const add = (path: string, symbols: Record<string, Decl>, where: string) => {
	const key = libraryKey(path);
	const lib = libraries.get(key) ?? { path, symbols: {} };
	for (const [name, d] of Object.entries(symbols)) {
		const had = lib.symbols[name];
		if (had && JSON.stringify(had) !== JSON.stringify(d)) throw new Error("extract-app: " + key + "." + name + " is declared two ways (" + where + ")");
		lib.symbols[name] = d;
	}
	libraries.set(key, lib);
};

// The devkit's modules blanked out (same length, so line numbers hold).
function appOnly(src: string): string {
	const marks = [...src.matchAll(/^\/\/ (\S+\.(?:ts|tsx|js|mjs))$/gm)];
	return marks.reduceRight((out, m, i) => {
		if (!/(^|\/)\.hutch\/devkit\/api\/|(^|\/)api\/(sdks|shared)\//.test(m[1]!)) return out;
		const end = marks[i + 1]?.index ?? out.length;
		return out.slice(0, m.index) + out.slice(m.index!, end).replace(/[^\n]/g, " ") + out.slice(end);
	}, src);
}

for (const file of files) {
	const src = appOnly(readFileSync(file, "utf8"));
	const locals = { dlopen: new Set<string>(), JSCallback: new Set<string>() };
	for (const m of src.matchAll(/import\s*\{([^}]*)\}\s*from\s*["']bun:ffi["']/g)) {
		for (const part of m[1]!.split(",")) {
			const [imported, local] = part.trim().split(/\s+as\s+/);
			if (imported === "dlopen" || imported === "JSCallback") locals[imported].add((local ?? imported).trim());
		}
	}
	for (const m of src.matchAll(/import\s*\*\s*as\s*([\w$]+)\s*from\s*["']bun:ffi["']/g)) {
		locals.dlopen.add(m[1] + ".dlopen");
		locals.JSCallback.add(m[1] + ".JSCallback");
	}
	for (const [kind, names] of Object.entries(locals)) {
		for (const name of names) {
			const call = new RegExp("(?<![\\w$.])" + name.replace(/[.$]/g, "\\$&") + "\\s*\\(", "g");
			for (const m of src.matchAll(call)) {
				const args = callArgs(src, m.index! + m[0].length - 1);
				const where = file + " at " + (src.slice(0, m.index).split("\n").length);
				if (kind === "JSCallback") {
					if (!args[1]) continue;
					const sig = decl(literal(src, file, args[1]) as Decl);
					callbacks.set(sig.args.join(",") + "->" + sig.returns, sig);
					continue;
				}
				if (!args[1]) continue;
				if (sdkSymbols.size && sdkCovers(src, args[1])) continue;
				const table = literal(src, file, args[1]) as Record<string, { args?: unknown[]; returns?: unknown }>;
				const symbols = Object.fromEntries(Object.entries(table).map(([s, d]) => [s, decl(d)]));
				if (sdkSymbols.size && Object.keys(symbols).every((s) => sdkSymbols.has(s))) continue;
				const { here, any } = libraryPaths(src, args[0] ?? "");
				if (here.length === 0 && any.length > 0) continue;
				const paths = here;
				if (paths.length === 0) {
					const found = exporting(Object.keys(symbols));
					if (found) paths.push(found);
				}
				if (paths.length === 0) unresolved.push(where + ": dlopen(" + (args[0] ?? "").slice(0, 80) + ", { " + Object.keys(symbols).join(", ") + " })");
				for (const path of new Set(paths)) add(path, symbols, where);
			}
		}
	}
}
if (unresolved.length) {
	console.error("extract-app: no library found for:\n  " + unresolved.join("\n  ") + (libsDir ? "" : "\n(--libs <dir> matches them to the app's libraries by their symbols)"));
	process.exit(1);
}
process.stdout.write(JSON.stringify({ libraries: [...libraries.values()], callbacks: [...callbacks.values()] }, null, 1) + "\n");
