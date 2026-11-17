// expect: argv ok
// expect: env ok
// expect: caught boom
// expect: exit 3
// exit: 3
if (process.argv.length >= 2 && process.argv[1].endsWith("process.test.js")) console.log("argv ok");
if (typeof process.env.PATH === "string" && process.platform === "darwin") console.log("env ok");
process.on("uncaughtException", (e) => console.log("caught", e.message));
process.on("exit", (code) => console.log("exit", code));
setTimeout(() => { throw new Error("boom"); }, 1);
setTimeout(() => { process.exitCode = 3; }, 5);
