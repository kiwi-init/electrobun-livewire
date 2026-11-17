const log = (...a) => console.log(...a);
const t0 = setTimeout(() => log("t0 never"), 5);
log("handle", typeof t0, typeof t0.ref, typeof t0.unref, typeof t0.hasRef, t0.hasRef?.());
clearTimeout(t0);
setTimeout((a, b) => log("args", a, b), 2, "x", 42);
setTimeout(() => log("delay 1"), 1);
setTimeout(() => log("delay 0"), 0);
setTimeout(() => log("20 a"), 20);
setTimeout(() => log("20 b"), 20);
setTimeout(() => log("10"), 10);
let killer;
setTimeout(() => { log("30 first, clears the second"); clearTimeout(killer); }, 30);
killer = setTimeout(() => log("30 second never"), 30);
let n = 0;
const iv = setInterval(() => { log("interval", ++n); if (n === 3) { clearInterval(iv); after(); } }, 40);
function after() {
	setTimeout(() => {
		log("nested 0");
		setTimeout(() => log("nested 0 child"), 0);
		setImmediate(() => log("nested immediate"));
	}, 0);
	const im = setImmediate(() => log("immediate never"));
	clearImmediate(im);
	setTimeout(() => {
		const u = setTimeout(() => log("unref never"), 1000);
		u.unref?.();
		log("refresh", typeof u.refresh);
		const r = setTimeout(() => log("refreshed then cleared by its old id: never"), 30);
		const key = +r;
		r.refresh();
		log("refresh keeps the id", +r === key);
		clearTimeout(key);
		const s = setInterval(() => {}, 1000);
		s.unref?.();
		log("end of timers");
	}, 60);
}
