// expect: on js thread: true
// expect: continuation after thread callback
// expect: timer after
let resolve;
const p = new Promise((r) => (resolve = r));
p.then(() => console.log("continuation after thread callback"));
__test.postFromThread((onJsThread) => {
	console.log("on js thread:", onJsThread);
	resolve();
	setTimeout(() => console.log("timer after"), 1);
}, 20);
