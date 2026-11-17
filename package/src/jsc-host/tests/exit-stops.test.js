// expect: before
// expect: exit listener 2
// exit: 2
// Nothing after process.exit() runs, as in Node and Bun.
process.on("exit", (code) => console.log("exit listener", code));
console.log("before");
process.exit(2);
console.log("ran after exit");
