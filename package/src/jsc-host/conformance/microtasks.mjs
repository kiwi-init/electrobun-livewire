const log = (...a) => console.log(...a);
log("sync 1");
Promise.resolve().then(() => log("p1")).then(() => log("p1 then"));
queueMicrotask(() => log("qm1"));
Promise.resolve().then(() => { log("p2"); queueMicrotask(() => log("qm from p2")); });
(async () => { log("async start"); await null; log("after await 1"); await null; log("after await 2"); })();
const thenable = { then(r) { log("thenable then"); r("t"); } };
Promise.resolve(thenable).then((v) => log("thenable value", v));
new Promise((r) => r(Promise.resolve("nested"))).then((v) => log("adopted", v));
Promise.reject(new Error("caught")).catch((e) => log("catch", e.message)).finally(() => log("finally"));
Promise.all([1, Promise.resolve(2), new Promise((r) => setTimeout(() => r(3), 1))]).then((v) => log("all", v.join(",")));
Promise.race([new Promise((r) => setTimeout(() => r("slow"), 30)), new Promise((r) => setTimeout(() => r("fast"), 5))]).then((v) => log("race", v));
Promise.allSettled([Promise.reject(new Error("x")), 1]).then((r) => log("allSettled", r.map((s) => s.status).join(",")));
Promise.any([Promise.reject(new Error("a")), Promise.resolve("any ok")]).then((v) => log("any", v));
log("sync 2");
setTimeout(() => {
	log("timer A");
	Promise.resolve().then(() => log("micro after A"));
	queueMicrotask(() => log("qm after A"));
}, 50);
setTimeout(() => log("timer B"), 50);
setTimeout(() => {
	setImmediate(() => { log("immediate 1"); Promise.resolve().then(() => log("micro after immediate 1")); });
	setImmediate(() => { log("immediate 2"); setTimeout(interleave, 0); });
}, 80);
async function interleave() {
	const order = [];
	const a = (async () => { order.push("a1"); await undefined; order.push("a2"); await undefined; order.push("a3"); })();
	const b = Promise.resolve().then(() => order.push("b1")).then(() => order.push("b2")).then(() => order.push("b3"));
	await Promise.all([a, b]);
	log("interleave", order.join(" "));
}
