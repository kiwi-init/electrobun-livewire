// The host runs a full collection once the loop goes idle, so dropped
// objects are gone before the next timer, as on Bun. JSC scans the stack
// conservatively, so a stale pointer can keep one graph alive: the test drops
// 200 separate graphs and expects nearly all of them collected.
// expect: alive at start true
// expect: collected while idle true
let refs;
function make() {
    const big = [];
    for (let i = 0; i < 1000; i++) big.push({ i });
    return new WeakRef(big);
}
function churn() {
    let total = 0;
    for (let i = 0; i < 1000; i++) total += [i, i + 1, i + 2].length;
    return total;
}
setTimeout(() => {
    refs = (() => {
        const made = [];
        for (let k = 0; k < 200; k++) made.push(make());
        return made;
    })();
    console.log("alive at start", refs[199].deref() !== undefined);
    churn();
}, 0);
setTimeout(() => {
    const alive = refs.filter((ref) => ref.deref() !== undefined).length;
    console.log("collected while idle", alive <= 20);
}, 1500);
