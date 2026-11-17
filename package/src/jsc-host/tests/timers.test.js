// expect: a 0
// expect: b 5
// expect: tick 1
// expect: tick 2
// expect: tick 3
// expect: c 30 x y
const t0 = performance.now();
setTimeout((p, q) => console.log("c", 30, p, q), 30, "x", "y");
setTimeout(() => console.log("b", 5), 5);
const never = setTimeout(() => console.log("cleared"), 10);
clearTimeout(never);
setTimeout(() => console.log("a", 0), 0);
let n = 0;
const iv = setInterval(() => { n++; console.log("tick", n); if (n === 3) clearInterval(iv); }, 7);
