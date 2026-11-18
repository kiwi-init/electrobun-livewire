// expect: sleepSync blocks
// expect: sleepSync 0 ok
// expect: no timer ran during it
// expect: end listener alone exits
// Bun.sleepSync blocks the thread for the time asked (it was a no-op), and an
// "end" listener on process.stdin alone doesn't open fd 0 or keep the process
// alive (Node starts reading only for "data"/"readable").
let timerRan = false;
setTimeout(() => (timerRan = true), 0);
const t0 = performance.now();
Bun.sleepSync(60);
const waited = performance.now() - t0;
console.log(waited >= 55 ? "sleepSync blocks" : "sleepSync returned after " + waited.toFixed(1) + " ms");
const t1 = performance.now();
Bun.sleepSync(0);
console.log(performance.now() - t1 < 20 ? "sleepSync 0 ok" : "sleepSync 0 slow");
console.log(timerRan ? "a timer ran during it" : "no timer ran during it");
process.stdin.on("end", () => console.log("stdin ended"));
console.log("end listener alone exits");
