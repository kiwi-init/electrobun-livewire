// node:util: Bun's browser polyfill plus parseArgs, which it lacks.
import * as base from "util";

// The polyfill's default export is only part of it; its named exports are all of it.
const util = { ...base.default, ...base };
delete util.default;
export const { inspect, format, formatWithOptions, promisify, callbackify, inherits, deprecate, debuglog, isDeepStrictEqual, types, TextEncoder, TextDecoder } = util;

const argError = (code, message) => Object.assign(new TypeError(message), { code });
// util.parseArgs, as Node's.
export function parseArgs(config = {}) {
	const args = config.args ?? globalThis.process.argv.slice(2);
	const options = config.options ?? {};
	const strict = config.strict ?? true;
	const allowPositionals = config.allowPositionals ?? !strict;
	const values = Object.create(null);
	const positionals = [];
	const tokens = [];
	const byShort = {};
	for (const [name, o] of Object.entries(options)) if (o.short) byShort[o.short] = name;
	const store = (name, value) => {
		const o = options[name];
		if (o?.multiple) (values[name] ??= []).push(value);
		else values[name] = value;
	};
	const take = (name, inline, k, rawName = "--" + name) => {
		const o = options[name];
		const token = { kind: "option", name, rawName, index: k, value: undefined, inlineValue: undefined };
		tokens.push(token);
		if (o?.type === "string") {
			token.value = inline ?? args[k + 1];
			token.inlineValue = inline !== undefined;
		} else if (inline !== undefined) {
			token.value = inline;
			token.inlineValue = true;
		}
		if (!o) {
			if (strict) throw argError("ERR_PARSE_ARGS_UNKNOWN_OPTION", "Unknown option '" + (name.length === 1 ? "-" : "--") + name + "'");
			store(name, inline ?? true);
			return k;
		}
		if (o.type === "string") {
			if (inline !== undefined) store(name, inline);
			else {
				const next = args[k + 1];
				if (next === undefined || (strict && next.startsWith("-"))) {
					if (strict) throw argError("ERR_PARSE_ARGS_INVALID_OPTION_VALUE", "Option '--" + name + " <value>' argument missing");
				}
				store(name, next);
				return k + 1;
			}
		} else {
			if (inline !== undefined && strict) throw argError("ERR_PARSE_ARGS_INVALID_OPTION_VALUE", "Option '--" + name + "' does not take an argument");
			store(name, true);
		}
		return k;
	};
	for (let k = 0; k < args.length; k++) {
		const arg = args[k];
		if (arg === "--") {
			tokens.push({ kind: "option-terminator", index: k });
			for (let p = k + 1; p < args.length; p++) {
				if (!allowPositionals) throw argError("ERR_PARSE_ARGS_UNEXPECTED_POSITIONAL", "Unexpected argument '" + args[p] + "'. This command does not take positional arguments");
				positionals.push(args[p]);
				tokens.push({ kind: "positional", index: p, value: args[p] });
			}
			break;
		}
		if (arg.startsWith("--")) {
			const eq = arg.indexOf("=");
			const name = eq < 0 ? arg.slice(2) : arg.slice(2, eq);
			if (!options[name] && name.startsWith("no-") && options[name.slice(3)]?.type === "boolean" && config.allowNegative) {
				values[name.slice(3)] = false;
				continue;
			}
			k = take(name, eq < 0 ? undefined : arg.slice(eq + 1), k);
		} else if (arg.startsWith("-") && arg.length > 1) {
			for (let c = 1; c < arg.length; c++) {
				const name = byShort[arg[c]] ?? arg[c];
				if (options[name]?.type === "string") {
					const rest = arg.slice(c + 1);
					k = take(name, rest || undefined, k, "-" + arg[c]);
					break;
				}
				k = take(name, undefined, k, "-" + arg[c]);
			}
		} else {
			if (!allowPositionals) throw argError("ERR_PARSE_ARGS_UNEXPECTED_POSITIONAL", "Unexpected argument '" + arg + "'. This command does not take positional arguments");
			positionals.push(arg);
			tokens.push({ kind: "positional", index: k, value: arg });
		}
	}
	for (const [name, o] of Object.entries(options)) if (o.default !== undefined && values[name] === undefined) values[name] = o.default;
	return config.tokens ? { values, positionals, tokens } : { values, positionals };
}
export default { ...util, parseArgs };
