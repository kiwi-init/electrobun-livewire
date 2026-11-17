// expect: sync
// expect: promise
// expect: nextTick
// expect: microtask
// expect: immediate
// expect: timeout
// nextTick is an ordinary microtask here (FIFO with promise jobs); Node runs
// it ahead of promise jobs already queued. Nothing in the SDK depends on that.
setTimeout(() => console.log("timeout"), 5);
setImmediate(() => console.log("immediate"));
Promise.resolve().then(() => console.log("promise"));
process.nextTick(() => console.log("nextTick"));
queueMicrotask(() => console.log("microtask"));
console.log("sync");
