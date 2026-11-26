// Bun.semver.satisfies on the Livewire JSC runtime, with node-semver's range
// grammar and prerelease rule, as Bun's: the SDK's AppEngine checks a UI's
// requiresContract with it.
const NUM = "(0|[1-9]\\d*)";
const PART = "(0|[1-9]\\d*|[xX*])";
const IDENT = "[0-9A-Za-z-]+";
const PRE = "(?:-(" + IDENT + "(?:\\." + IDENT + ")*))?";
const BUILD = "(?:\\+" + IDENT + "(?:\\." + IDENT + ")*)?";
const VERSION = new RegExp("^[v=]?\\s*" + NUM + "\\." + NUM + "\\." + NUM + PRE + BUILD + "$");
const PARTIAL = new RegExp("^(<=|>=|<|>|=|\\^|~>?)?\\s*[v=]?\\s*(?:" + PART + "(?:\\." + PART + "(?:\\." + PART + PRE + BUILD + ")?)?)?$");
const HYPHEN = /^(\S+)\s+-\s+(\S+)$/;

const parse = (text) => {
	const m = VERSION.exec(String(text).trim());
	return m ? { major: +m[1], minor: +m[2], patch: +m[3], pre: m[4] ? m[4].split(".") : [] } : null;
};

const compareIds = (a, b) => {
	const an = /^\d+$/.test(a), bn = /^\d+$/.test(b);
	if (an && bn) return Math.sign(+a - +b);
	if (an) return -1;
	if (bn) return 1;
	return a < b ? -1 : a > b ? 1 : 0;
};
const compare = (a, b) => {
	for (const k of ["major", "minor", "patch"]) if (a[k] !== b[k]) return a[k] < b[k] ? -1 : 1;
	if (!a.pre.length || !b.pre.length) return a.pre.length ? -1 : b.pre.length ? 1 : 0;
	for (let i = 0; i < Math.max(a.pre.length, b.pre.length); i++) {
		if (i >= a.pre.length) return -1;
		if (i >= b.pre.length) return 1;
		const c = compareIds(a.pre[i], b.pre[i]);
		if (c) return c;
	}
	return 0;
};

const isX = (p) => p === undefined || p === "x" || p === "X" || p === "*";
const v = (major, minor, patch, pre = []) => ({ major, minor, patch, pre });
const floor = (major, minor, patch, pre) => v(major, minor, patch, pre);
const ZERO = ["0"];

// One comparator ("^1.2", ">=1.2.3-rc.1", "1.x") as [op, version] pairs.
function desugar(text) {
	const m = PARTIAL.exec(text);
	if (!m) return null;
	const op = m[1] === "~>" ? "~" : m[1] ?? "";
	const [M, m2, p] = [m[2], m[3], m[4]];
	const pre = m[5] ? m[5].split(".") : [];
	if (isX(M)) return op === "<" || op === ">" ? [["<", v(0, 0, 0, ZERO)]] : [];
	const major = +M;
	if (isX(m2)) {
		if (op === ">") return [[">=", v(major + 1, 0, 0)]];
		if (op === "<") return [["<", v(major, 0, 0, ZERO)]];
		if (op === "<=") return [["<", v(major + 1, 0, 0, ZERO)]];
		if (op === ">=") return [[">=", v(major, 0, 0)]];
		return [[">=", v(major, 0, 0)], ["<", v(major + 1, 0, 0, ZERO)]];
	}
	const minor = +m2;
	if (isX(p)) {
		if (op === ">") return [[">=", v(major, minor + 1, 0)]];
		if (op === "<") return [["<", v(major, minor, 0, ZERO)]];
		if (op === "<=") return [["<", v(major, minor + 1, 0, ZERO)]];
		if (op === ">=") return [[">=", v(major, minor, 0)]];
		if (op === "^" && major === 0) return [[">=", v(0, minor, 0)], ["<", v(0, minor + 1, 0, ZERO)]];
		if (op === "^") return [[">=", v(major, minor, 0)], ["<", v(major + 1, 0, 0, ZERO)]];
		return [[">=", v(major, minor, 0)], ["<", v(major, minor + 1, 0, ZERO)]];
	}
	const patch = +p;
	const at = floor(major, minor, patch, pre);
	if (op === "^") {
		const top = major > 0 ? v(major + 1, 0, 0, ZERO) : minor > 0 ? v(0, minor + 1, 0, ZERO) : v(0, 0, patch + 1, ZERO);
		return [[">=", at], ["<", top]];
	}
	if (op === "~") return [[">=", at], ["<", v(major, minor + 1, 0, ZERO)]];
	return [[op === "" ? "=" : op, at]];
}

function hyphen(from, to) {
	const bottom = PARTIAL.exec(from);
	const top = PARTIAL.exec(to);
	if (!bottom || !top) return null;
	// Bun reads an x major before numbers ("*.3") at the bottom of a hyphen
	// range as 0 ("0.3"), and as * everywhere else.
	const low = isX(bottom[2]) && !isX(bottom[3]) ? desugar(">=0." + from.split(".").slice(1).join(".")) : desugar(">=" + from);
	if (!low) return null;
	const [, , M, m2, p] = top;
	if (isX(M)) return low;
	if (isX(m2)) return [...low, ["<", v(+M + 1, 0, 0, ZERO)]];
	if (isX(p)) return [...low, ["<", v(+M, +m2 + 1, 0, ZERO)]];
	return [...low, ...(desugar("<=" + to) ?? [])];
}

// A range as comparator sets, or null when it isn't one.
function parseRange(range) {
	const sets = [];
	for (const raw of String(range).split("||")) {
		const set = raw.trim().replace(/(<=|>=|<|>|=|\^|~>?)\s+/g, "$1").replace(/\s+/g, " ");
		const h = HYPHEN.exec(set);
		const comparators = h ? hyphen(h[1], h[2]) : set === "" ? [] : set.split(" ").map(desugar).reduce((all, c) => (all && c ? [...all, ...c] : null), []);
		if (!comparators) return null;
		sets.push(comparators);
	}
	return sets;
}

const holds = (version, [op, at]) => {
	const c = compare(version, at);
	return op === "=" ? c === 0 : op === ">" ? c > 0 : op === ">=" ? c >= 0 : op === "<" ? c < 0 : c <= 0;
};

// node-semver's rule: a prerelease only satisfies a set that names a
// prerelease of the same major.minor.patch.
const prereleaseAllowed = (version, set) =>
	!version.pre.length ||
	set.some(([, at]) => at.pre.length && at.pre !== ZERO && at.major === version.major && at.minor === version.minor && at.patch === version.patch);

export function satisfies(version, range) {
	const parsed = parse(version);
	if (!parsed) return false;
	const sets = parseRange(range);
	if (!sets) return true;
	return sets.some((set) => set.every((c) => holds(parsed, c)) && prereleaseAllowed(parsed, set));
}

