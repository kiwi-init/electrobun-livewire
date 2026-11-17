// The host runs a full collection once the loop goes idle, so a dropped
// object graph is gone before the next timer, as on Bun.
// expect: alive at start true
// expect: collected while idle true
function make() {
    const big = [];
    for (let i = 0; i < 1e5; i++) big.push({ i });
    return new WeakRef(big);
}
const ref = make();
console.log("alive at start", ref.deref() !== undefined);
setTimeout(() => console.log("collected while idle", ref.deref() === undefined), 1500);
