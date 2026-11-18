// Generates the JSC host's bun:ffi glue from the SDK's own declarations: one
// C host function per dlopen symbol (with Bun's argument and return
// conversions) and a set of trampolines per JSCallback signature. Runs with
// Bun at build time:
//   bun package/src/jsc-host/ffi/gen.ts <out dir> [--extra decls.json ...]
// --extra adds libraries and callback signatures the app declares beyond the
// SDK's: {"libraries":[{"path","symbols"}],"callbacks":[{"args","returns"}]}.
// Writes <out>/ffi_glue.c and <out>/ffi-decls.json. Fails when a declaration
// uses a type the glue doesn't know, so drift breaks the build, not the app.
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { basename, join } from "node:path";

const here = import.meta.dir;
const outDir = process.argv[2];
const flagValues = (name: string): string[] =>
	process.argv.flatMap((arg, i) => {
		const value = process.argv[i + 1];
		return arg === name && value ? [value] : [];
	});
const extras = flagValues("--extra");
// --sdk <dir>: the SDK's sdks/main to read (default: this checkout's).
const sdkDir = flagValues("--sdk")[0] ?? join(here, "../../sdks/main");
// --check-lib <dylib>: fail when the SDK declares a symbol that library (same
// file name) doesn't export.
const checkLibs = flagValues("--check-lib");
if (!outDir) {
	console.error("usage: gen.ts <out dir>");
	process.exit(2);
}
mkdirSync(outDir, { recursive: true });

// 1. dlopen tables: recorded by running proc/native.ts against the recorder.
const extracted = Bun.spawnSync([process.execPath, join(here, "extract.ts"), sdkDir], { stderr: "inherit" });
if (extracted.exitCode !== 0) process.exit(1);
type Decl = { args?: string[]; returns?: string };
const record = JSON.parse(extracted.stdout.toString()) as { libraries: { path: string; symbols: Record<string, Decl> }[] };

for (const extraPath of extras) {
	const extra = JSON.parse(readFileSync(extraPath, "utf8"));
	record.libraries.push(...(extra.libraries ?? []));
}

// One entry per library: the SDK and an app (--extra) may both declare one
// (Channels' message box calls NativeWrapper's showMessageBox). A symbol
// declared two ways fails, since the glue has one signature per symbol.
{
	const keyOf = (path: string) => basename(path).replace(/^lib/, "").replace(/\.(dylib|so|dll)$/, "");
	const merged = new Map<string, { path: string; symbols: Record<string, Decl> }>();
	for (const lib of record.libraries) {
		const into = merged.get(keyOf(lib.path));
		if (!into) {
			merged.set(keyOf(lib.path), { path: lib.path, symbols: { ...lib.symbols } });
			continue;
		}
		for (const [name, d] of Object.entries(lib.symbols)) {
			const had = into.symbols[name];
			const same = (a?: Decl) => JSON.stringify({ args: (a?.args ?? []).map(canon), returns: canon(a?.returns ?? "void") });
			if (had && same(had) !== same(d)) {
				console.error("gen: " + keyOf(lib.path) + "." + name + " is declared two ways: " + same(had) + " and " + same(d));
				process.exit(1);
			}
			into.symbols[name] = d;
		}
	}
	record.libraries = [...merged.values()];
}

for (const libPath of checkLibs) {
	const lib = record.libraries.find((l) => basename(l.path) === basename(libPath));
	if (!lib) continue;
	// Exports: Mach-O's global defined symbols; an ELF .so's dynamic table (its
	// .symtab may be stripped).
	const nm = Bun.spawnSync(process.platform === "darwin" ? ["nm", "-gU", libPath] : ["nm", "-D", "--defined-only", libPath]);
	const exported = new Set(nm.stdout.toString().split("\n").map((line: string) => line.trim().split(/\s+/).pop()?.replace(/^_/, "")));
	const missing = Object.keys(lib.symbols).filter((name) => !exported.has(name));
	if (missing.length) {
		console.error("gen: " + basename(libPath) + " doesn't export " + missing.join(", "));
		process.exit(1);
	}
}

// 2. JSCallback signatures: read from the source, since some are created
// lazily (tray, wgpu input) and never run during extraction. Every SDK file
// that creates one is read (proc/native.ts, webgpuAdapter.ts, ...).
type CallbackSig = { args: string[]; returns: string };
const callbackSigs = new Map<string, CallbackSig>();
for (const file of new Bun.Glob("**/*.ts").scanSync(sdkDir)) {
	if (/\.test\.ts$|(^|\/)__tests__\//.test(file)) continue;
	const source = readFileSync(join(sdkDir, file), "utf8");
	if (!source.includes("new JSCallback(")) continue;
	const lines = source.split("\n");
	const sites = lines.flatMap((line, i) => (line.includes("new JSCallback(") && !line.trim().startsWith("//") ? [i] : []));
	for (const [n, start] of sites.entries()) {
		const end = sites[n + 1] ?? lines.length;
		const chunk = lines.slice(start, end).join("\n");
		const m = chunk.match(/args:\s*\[([^\]]*)\]\s*,\s*returns:\s*([^,\n}]+)/s);
		if (!m) throw new Error(`gen: can't read the JSCallback signature at ${file}:${start + 1}`);
		const norm = (t: string) => t.replace(/\/\/.*$/gm, "").trim().replace(/^FFIType\./, "").replace(/["']/g, "");
		const sig = { args: (m[1] ?? "").split(",").map(norm).filter(Boolean), returns: norm(m[2] ?? "void") };
		callbackSigs.set(sigKey(sig.args, sig.returns), sig);
	}
}
for (const extraPath of extras) {
	for (const sig of JSON.parse(readFileSync(extraPath, "utf8")).callbacks ?? []) {
		callbackSigs.set(sigKey(sig.args ?? [], sig.returns ?? "void"), { args: sig.args ?? [], returns: sig.returns ?? "void" });
	}
}

function sigKey(args: string[], returns: string) {
	return args.map(canon).join(",") + "->" + canon(returns);
}
function canon(t: string): string {
	const map: Record<string, string> = { int: "i32", int32_t: "i32", uint32_t: "u32", pointer: "ptr", callback: "function", double: "f64", uint64_t: "u64" };
	return map[t] ?? t;
}
const C_TYPES: Record<string, string> = {
	void: "void", bool: "bool", u32: "uint32_t", i32: "int32_t", u64: "uint64_t", f64: "double",
	cstring: "const char *", ptr: "void *", function: "void *",
};
function ctype(t: string) {
	const c = C_TYPES[canon(t)];
	if (!c) throw new Error("gen: unsupported FFI type " + t);
	return c;
}

// --- library host functions ------------------------------------------------
let c = "// Generated by package/src/jsc-host/ffi/gen.ts from the SDK's bun:ffi\n// declarations. Do not edit.\n#include \"support.h\"\n\n";
c += "typedef struct { const char *name; const char *sig; JSObjectCallAsFunctionCallback fn; void **target; } ffi_symbol;\n";
c += "typedef struct { const char *key; const ffi_symbol *symbols; int count; } ffi_library;\n\n";

const libraries: { key: string; table: string; count: number }[] = [];
for (const lib of record.libraries) {
	const key = basename(lib.path).replace(/^lib/, "").replace(/\.(dylib|so|dll)$/, "");
	const id = key.replace(/[^A-Za-z0-9]/g, "_");
	const names = Object.keys(lib.symbols);
	c += "// " + key + ": " + names.length + " symbols\n";
	names.forEach((_, i) => {
		c += "static void *" + id + "_p" + i + ";\n";
	});
	names.forEach((name, i) => {
		const decl = lib.symbols[name] ?? {};
		const args = (decl.args ?? []).map(canon);
		const ret = canon(decl.returns ?? "void");
		const fnType = ctype(ret) + " (*)(" + (args.length ? args.map(ctype).join(", ") : "void") + ")";
		let body = "";
		const conv = args.map((t, j) => {
			const a = "FFI_ARG(" + j + ")";
			switch (t) {
				case "bool": return "ffi_arg_bool(ctx, " + a + ")";
				case "u32": return "(uint32_t)ffi_arg_num(ctx, " + a + ")";
				case "i32": return "(int32_t)ffi_arg_num(ctx, " + a + ")";
				case "f64": return "ffi_arg_num(ctx, " + a + ")";
				case "u64": return "ffi_arg_u64(ctx, " + a + ")";
				case "cstring": body += "    char *own" + j + " = NULL;\n"; return "ffi_arg_cstring(ctx, " + a + ", &own" + j + ")";
				case "ptr": case "function": return "ffi_arg_ptr(ctx, " + a + ")";
				default: throw new Error("gen: unsupported argument type " + t + " in " + name);
			}
		});
		// Arguments are converted before the call, so the "in a native call"
		// window (see support.h) covers only the C function itself.
		args.forEach((t, j) => { body += "    " + ctype(t) + " x" + j + " = " + conv[j] + ";\n"; });
		const call = "((" + fnType + ")" + id + "_p" + i + ")(" + conv.map((_, j) => "x" + j).join(", ") + ")";
		const frees = args.flatMap((t, j) => (t === "cstring" ? ["    free(own" + j + ");\n"] : [])).join("");
		let ret_ = "";
		switch (ret) {
			case "void": body += "    ffi_enter_native();\n    " + call + ";\n    ffi_leave_native();\n" + frees; ret_ = "JSValueMakeUndefined(ctx)"; break;
			case "bool": body += "    ffi_enter_native();\n    bool r = " + call + ";\n    ffi_leave_native();\n" + frees; ret_ = "JSValueMakeBoolean(ctx, r)"; break;
			case "u32": case "i32": case "f64": body += "    ffi_enter_native();\n    double r = (double)" + call + ";\n    ffi_leave_native();\n" + frees; ret_ = "JSValueMakeNumber(ctx, r)"; break;
			case "u64": body += "    ffi_enter_native();\n    uint64_t r = " + call + ";\n    ffi_leave_native();\n" + frees; ret_ = "ffi_ret_u64(ctx, r)"; break;
			case "ptr": case "function": body += "    ffi_enter_native();\n    void *r = " + call + ";\n    ffi_leave_native();\n" + frees; ret_ = "ffi_ret_ptr(ctx, r)"; break;
			case "cstring": body += "    ffi_enter_native();\n    const char *r = " + call + ";\n    ffi_leave_native();\n" + frees; ret_ = "ffi_ret_cstring(ctx, r)"; break;
			default: throw new Error("gen: unsupported return type " + ret + " in " + name);
		}
		c += "static JSValueRef " + id + "_f" + i + "(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {\n";
		c += "    (void)f; (void)self; (void)exc; (void)argc; (void)argv;\n";
		c += "    if (!" + id + "_p" + i + ") { *exc = jsr_string(ctx, \"" + name + " is not loaded\"); return NULL; }\n";
		c += "    FFI_TRACE(\"call " + name + "\");\n";
		c += body + "    return " + ret_ + ";\n}\n";
	});
	c += "static const ffi_symbol " + id + "_table[] = {\n";
	names.forEach((name, i) => {
		const decl = lib.symbols[name] ?? {};
		c += "    {\"" + name + "\", \"" + sigKey(decl.args ?? [], decl.returns ?? "void") + "\", " + id + "_f" + i + ", &" + id + "_p" + i + "},\n";
	});
	c += "};\n\n";
	libraries.push({ key, table: id + "_table", count: names.length });
}
c += "static const ffi_library ffi_libraries[] = {\n" + libraries.map((l) => "    {\"" + l.key + "\", " + l.table + ", " + l.count + "},\n").join("") + "};\n\n";

// --- callback trampolines ----------------------------------------------------
const SLOTS = 16;
const cbs = [...callbackSigs.entries()];
cbs.forEach(([key, sig], s) => {
	const args = sig.args.map(canon);
	const ret = canon(sig.returns);
	const p = "cb" + s;
	const params = args.length ? args.map((t, j) => ctype(t) + " a" + j).join(", ") : "void";
	c += "// " + key + "\nstatic ffi_slot " + p + "_slots[" + SLOTS + "];\n";
	c += "static JSValueRef " + p + "_args(JSContextRef ctx, ffi_cb_call *call, JSValueRef *out) {\n    (void)ctx; (void)call; (void)out;\n";
	args.forEach((t, j) => {
		const v = "call->a[" + j + "]";
		const make: Record<string, string> = {
			bool: "JSValueMakeBoolean(ctx, " + v + ".b)", u32: "JSValueMakeNumber(ctx, " + v + ".u32)", i32: "JSValueMakeNumber(ctx, " + v + ".i32)",
			f64: "JSValueMakeNumber(ctx, " + v + ".f64)", u64: "ffi_ret_u64(ctx, " + v + ".u64)", ptr: "ffi_ret_ptr(ctx, " + v + ".p)",
			function: "ffi_ret_ptr(ctx, " + v + ".p)", cstring: v + ".s ? jsr_string(ctx, " + v + ".s) : JSValueMakeNull(ctx)",
		};
		c += "    out[" + j + "] = " + make[t] + ";\n";
	});
	c += "    return NULL;\n}\n";
	const wants = ret !== "void";
	c += "static " + ctype(ret) + " " + p + "_invoke(int slot" + (args.length ? ", " + params : "") + ") {\n";
	c += "    FFI_TRACE(\"callback " + p + "\");\n";
	c += "    ffi_cb_call *call = calloc(1, sizeof *call);\n    call->slot = &" + p + "_slots[slot];\n    call->argc = " + args.length + ";\n    call->make_args = " + p + "_args;\n";
	args.forEach((t, j) => {
		const field: Record<string, string> = { bool: "b", u32: "u32", i32: "i32", f64: "f64", u64: "u64", ptr: "p", function: "p" };
		if (t === "cstring") c += "    call->a[" + j + "].s = a" + j + " ? strdup(a" + j + ") : NULL;\n    call->owns_string[" + j + "] = true;\n";
		else c += "    call->a[" + j + "]." + field[t] + " = a" + j + ";\n";
	});
	c += "    bool direct = ffi_runs_js_here() && !call->slot->threadsafe;\n";
	c += "    bool waits = !ffi_runs_js_here() && " + (wants ? "!call->slot->threadsafe" : "false") + ";\n";
	c += "    JSValueRef r = ffi_dispatch(call, " + (wants ? "true" : "false") + ");\n";
	if (ret === "void") c += "    (void)r;\n";
	else if (ret === "cstring") c += "    const char *out = ffi_result_cstring(call->slot, r);\n";
	else if (ret === "ptr" || ret === "function") c += "    void *out = ffi_arg_ptr(jsr_context(g_ffi_rt), r);\n";
	else if (ret === "bool") c += "    bool out = r ? JSValueToBoolean(jsr_context(g_ffi_rt), r) : false;\n";
	else if (ret === "u64") c += "    uint64_t out = r ? ffi_arg_u64(jsr_context(g_ffi_rt), r) : 0;\n";
	else c += "    " + ctype(ret) + " out = (" + ctype(ret) + ")ffi_result_num(r);\n";
	c += "    if (direct) ffi_free_call(call);\n    else if (waits) { if (!call->timed_out) ffi_release_result(r, true); ffi_call_unref(call); }\n";
	c += ret === "void" ? "}\n" : "    return out;\n}\n";
	for (let n = 0; n < SLOTS; n++) {
		c += "static " + ctype(ret) + " " + p + "_t" + n + "(" + params + ") { " + (ret === "void" ? "" : "return ") + p + "_invoke(" + n + (args.length ? ", " + args.map((_, j) => "a" + j).join(", ") : "") + "); }\n";
	}
	c += "static void *const " + p + "_tramps[" + SLOTS + "] = {" + Array.from({ length: SLOTS }, (_, n) => p + "_t" + n).join(", ") + "};\n\n";
});
c += "typedef struct { const char *key; ffi_slot *slots; void *const *tramps; } ffi_callback_sig;\n";
c += "static const ffi_callback_sig ffi_callback_sigs[] = {\n" + cbs.map(([key], s) => "    {\"" + key + "\", cb" + s + "_slots, cb" + s + "_tramps},\n").join("") + "};\n";
c += "#define FFI_CALLBACK_SLOTS " + SLOTS + "\n";
c += "#include \"natives.inc\"\n";

writeFileSync(join(outDir, "ffi_glue.c"), c);
writeFileSync(join(outDir, "ffi-decls.json"), JSON.stringify({ libraries: record.libraries, callbacks: Object.fromEntries(callbackSigs) }, null, 1));
console.error("gen: " + libraries.map((l) => l.key + " " + l.count).join(", ") + " symbols; " + cbs.length + " callback signatures x " + SLOTS + " slots");
