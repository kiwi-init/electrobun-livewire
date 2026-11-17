// Workers: the global Worker and node:worker_threads, messages with the
// structured clone, nested workers, errors, exit codes and terminate(). The
// worker is this same file (process.argv[1]), in a mode from workerData.
import { isMainThread, parentPort, workerData, Worker as NodeWorker } from "node:worker_threads";

const self = process.argv[1];
const show = (v, seen = new Set()) => {
	if (v && typeof v === "object") {
		if (seen.has(v)) return "[Circular]";
		seen = new Set(seen).add(v);
	}
	if (v instanceof Map) return `Map(${[...v].map(([k, x]) => show(k, seen) + "=>" + show(x, seen)).join(",")})`;
	if (v instanceof Set) return `Set(${[...v].map((x) => show(x, seen)).join(",")})`;
	if (v instanceof Date) return `Date(${v.toISOString()})`;
	if (v instanceof RegExp) return String(v);
	if (v instanceof Error) return `${v.name}(${v.message})`;
	if (ArrayBuffer.isView(v)) return `${v.constructor.name}[${Array.from(v.constructor === DataView ? new Uint8Array(v.buffer, v.byteOffset, v.byteLength) : v).join(",")}]`;
	if (v instanceof ArrayBuffer) return `ArrayBuffer(${v.byteLength})`;
	if (typeof v === "bigint") return v + "n";
	if (Object.is(v, -0)) return "-0";
	if (Array.isArray(v)) return "[" + v.map((x) => show(x, seen)).join(",") + "]";
	if (v && typeof v === "object") return "{" + Object.entries(v).map(([k, x]) => k + ":" + show(x, seen)).join(",") + "}";
	return String(v);
};

if (!isMainThread) {
	const mode = workerData.mode;
	if (mode === "echo") {
		parentPort.on("message", (m) => {
			if (m === "bye") return process.exit(0);
			parentPort.postMessage({ got: m, shared: m && m.a === m.b });
		});
	} else if (mode === "scope") {
		// Web style: onmessage and postMessage on the worker's global scope.
		globalThis.onmessage = (e) => { postMessage("scope got " + e.data); globalThis.onmessage = null; };
	} else if (mode === "throw") {
		throw new TypeError("boom in worker");
	} else if (mode === "exit") {
		process.on("exit", (code) => parentPort.postMessage("exiting " + code));
		process.exit(3);
	} else if (mode === "spin") {
		parentPort.postMessage("spinning");
		let n = 0;
		for (;;) n = (n + 1) % 1000;
	} else if (mode === "nested") {
		const inner = new Worker(self, { workerData: { mode: "echo" } });
		inner.addEventListener("message", (e) => {
			parentPort.postMessage("nested: " + show(e.data.got));
			inner.terminate();
		});
		inner.addEventListener("close", () => parentPort.postMessage("inner closed"));
		inner.postMessage("through two threads");
	} else if (mode === "sqlite") {
		// The search worker's shape: bun:sqlite in a worker, the bytes back to the parent.
		import("bun:sqlite").then(({ Database }) => {
			const db = new Database(":memory:");
			db.run("CREATE VIRTUAL TABLE hits USING fts5(body)");
			db.run("INSERT INTO hits (rowid, body) VALUES (?, ?)", [7, "found in a worker"]);
			parentPort.postMessage({ hit: db.query("SELECT rowid AS seq FROM hits WHERE hits MATCH ?").get("worker"), bytes: db.serialize().length > 0 });
			db.close();
		});
	} else if (mode === "busy") {
		let n = 0;
		for (let i = 0; i < 2e6; i++) n += i % 7;
		parentPort.postMessage(n);
	}
} else main();

// The bundle is an IIFE: no top-level await.
async function main() {
	const run = (mode, setup) => new Promise((resolve) => {
		const w = new Worker(self, { workerData: { mode } });
		const lines = [];
		w.addEventListener("message", (e) => { lines.push("message " + show(e.data)); setup.onMessage?.(w, e.data); });
		w.addEventListener("error", (e) => { lines.push("error mentions it: " + String(e.message).includes("boom in worker")); e.preventDefault(); });
		w.addEventListener("close", () => { console.log(mode + ": " + lines.join(" | ")); resolve(); });
		setup.start?.(w);
	});

	const shared = { x: 1 };
	const value = {
		n: [1, -0, NaN, Infinity, -Infinity, 2 ** 53], s: "héllo ☃", u: undefined, nul: null, big: 12345678901234567890n,
		date: new Date(Date.UTC(2026, 10, 17)), re: /a+b/gi, map: new Map([["k", 1], [2, { deep: true }]]), set: new Set([1, "two"]),
		bytes: new Uint8Array([1, 2, 255]), f64: new Float64Array([0.5, -1]), view: new DataView(new Uint8Array([9, 8]).buffer), ab: new ArrayBuffer(4),
		err: new RangeError("bad range"), a: shared, b: shared,
	};
	value.self = value;
	const cloned = structuredClone(value);
	console.log("structuredClone", show({ ...cloned, self: cloned.self === cloned }), cloned.a === cloned.b);
	try { structuredClone({ f() {} }); } catch (e) { console.log("clone function:", e.name); }

	await run("echo", {
		start: (w) => { w.postMessage(value); w.postMessage([1, [2, [3]]]); },
		onMessage: (w, d) => { if (Array.isArray(d.got)) w.postMessage("bye"); },
	});
	await run("scope", { start: (w) => w.postMessage("hi") , onMessage: (w) => w.terminate() });
	await run("throw", {});
	await run("exit", {});
	await run("spin", { onMessage: (w) => setTimeout(() => w.terminate(), 50) });
	await run("sqlite", { onMessage: (w) => w.terminate() });
	await run("nested", { onMessage: (w, d) => { if (d === "inner closed") w.terminate(); } });

	// Several at once, each on its own thread.
	const sums = await Promise.all([0, 1, 2, 3].map(() => new Promise((resolve) => {
		const w = new Worker(self, { workerData: { mode: "busy" } });
		w.onmessage = (e) => { resolve(e.data); w.terminate(); };
	})));
	console.log("parallel", sums.join(","));

	// node:worker_threads' Worker: message, exit and terminate()'s promise.
	await new Promise((resolve) => {
		const w = new NodeWorker(self, { workerData: { mode: "echo" } });
		w.on("message", (m) => { console.log("node worker message", show(m)); w.postMessage("bye"); });
		w.on("exit", (code) => { console.log("node worker exit", code); resolve(); });
		w.postMessage({ hello: "node" });
	});
	const spinner = new NodeWorker(self, { workerData: { mode: "spin" } });
	await new Promise((resolve) => spinner.once("message", resolve));
	console.log("terminate() resolves", typeof (await spinner.terminate()));
	console.log("main done");
}
