// expect: Bun.gc is a function
// expect: a dropped object is collected
// Bun.gc(true) runs a full collection (Channels calls it when a window closes;
// it was missing, so the app's window failed to start).
console.log(typeof Bun.gc === "function" ? "Bun.gc is a function" : "Bun.gc is " + typeof Bun.gc);
let collected = false;
const registry = new FinalizationRegistry(() => (collected = true));
(() => registry.register({ big: new Array(1000).fill(0) }, "x"))();
for (let i = 0; i < 5; i++) Bun.gc(true);
setTimeout(() => {
	for (let i = 0; i < 5 && !collected; i++) Bun.gc(true);
	setTimeout(() => console.log(collected ? "a dropped object is collected" : "nothing was collected"), 50);
}, 10);
