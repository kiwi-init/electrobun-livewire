// FinalizationRegistry callbacks run once their targets are collected: the
// host's idle collection frees them, and JSC's deferred work, which calls
// the callbacks, runs in the host's loop (on Linux it once never ran).
// expect: registered 100
// expect: callbacks ran true
let calls = 0;
const registry = new FinalizationRegistry(() => calls++);
function register() {
    for (let i = 0; i < 100; i++) registry.register({ i, pad: new Array(100).fill(i) }, i);
}
setTimeout(() => {
    register();
    console.log("registered", 100);
}, 0);
setTimeout(() => {
    // JSC scans the stack conservatively: a few targets may survive.
    console.log("callbacks ran", calls >= 80);
}, 2000);
