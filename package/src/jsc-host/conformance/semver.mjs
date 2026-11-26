// Bun.semver.satisfies: node-semver's ranges and prerelease rule, as AppEngine
// checks a UI's requiresContract.
const versions = ["0.0.0","0.0.1","0.0.2","0.1.0","0.1.5","0.2.0","0.9.9","1.0.0-0","1.0.0-alpha","1.0.0-alpha.1","1.0.0-beta","1.0.0-rc.1","1.0.0","1.0.1","1.2.0","1.2.3-beta.2","1.2.3","1.2.4","1.3.0","1.9.9","2.0.0-0","2.0.0-rc.1","2.0.0","2.3.4","2.4.0","3.0.0","3.8.9","3.9.0","3.9.1","3.10.0","4.0.0","v1.2.3","1.2","banana",""];
const ranges = ["^1","^1.2","^1.2.3","^0.2.3","^0.0.3","^0.x","^0.0","^1.x","^1.2.3-beta.2","^3.0.0","^3.9.0","^3.10.0","~1.2.3","~1.2","~1","~>1.2.3","~0.2","1","1.x","1.2.x","*","","x",">1",">1.2",">1.2.3",">=1.2",">=1.2.3","<1","<1.2","<1.2.3","<=1","<=1.2","<=1.2.3","=1.2.3","1.2.3",">=1.0.0 <2.0.0",">= 1.2.3 < 2",">1.0.0-alpha <1.0.0","1.2 - 2.3.4","1.2.3 - 2.3","1.2.3 - 2","^1 || ^3",">=3.0.0 || <1.0.0","^1.0.0-0","1.0.0-alpha - 1.0.0",">=1.0.0-rc.1","<2.0.0-rc.1",">*","<*","^0.0.0","~0.0.1"];
for (const range of ranges) console.log(JSON.stringify(range).padEnd(24), versions.map((v) => (Bun.semver.satisfies(v, range) ? 1 : 0)).join(""));

let seed = 7;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) & 0x7fffffff) / 0x7fffffff);
const pick = (a) => a[Math.floor(rnd() * a.length)];
const num = () => String(pick([0, 0, 1, 1, 2, 3, 10]));
const pre = () => (rnd() < 0.3 ? "-" + pick(["0", "alpha", "alpha.1", "beta.2", "rc.1", "rc.10", "1", "x.y"]) : "");
const version = () => pick(["", "", "", "v", "="]) + num() + "." + num() + "." + num() + pre() + (rnd() < 0.1 ? "+001" : "");
const part = () => (rnd() < 0.15 ? pick(["x", "X", "*"]) : num());
const partial = () => {
	const k = rnd();
	return k < 0.15 ? part() : k < 0.35 ? part() + "." + part() : num() + "." + num() + "." + num() + pre();
};
const comparator = () => pick(["", "", "^", "~", "~>", ">", ">=", "<", "<=", "="]) + pick(["", "", " "]) + partial();
const set = () => (rnd() < 0.15 ? partial() + " - " + partial() : Array.from({ length: 1 + Math.floor(rnd() * 3) }, comparator).join(" "));
const range = () => Array.from({ length: 1 + Math.floor(rnd() * 2) }, set).join(" || ");
for (let line = 0; line < 100; line++) console.log("random", line, Array.from({ length: 50 }, () => (Bun.semver.satisfies(version(), range()) ? 1 : 0)).join(""));
