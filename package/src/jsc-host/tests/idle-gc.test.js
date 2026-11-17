// The host runs a full collection once the loop goes idle, so a dropped
// object graph is gone before the next timer, as on Bun. The graph is made in
// a timer callback, in a nested call, and the stack is churned after it, so
// JSC's conservative stack scan has no stale pointer to keep it alive.
// expect: alive at start true
// expect: collected while idle true
let ref;
function make() {
    const big = [];
    for (let i = 0; i < 1e5; i++) big.push({ i });
    return new WeakRef(big);
}
function churn() {
    let total = 0;
    for (let i = 0; i < 1000; i++) total += [i, i + 1, i + 2].length;
    return total;
}
setTimeout(() => {
    ref = (() => make())();
    console.log("alive at start", ref.deref() !== undefined);
    churn();
}, 0);
setTimeout(() => console.log("collected while idle", ref.deref() === undefined), 1500);
