// expect: unhandledRejection nope
// expect: still running
// A listener handles it, and the process keeps going.
process.on("unhandledRejection", (reason) => console.log("unhandledRejection", reason));
Promise.reject("nope");
setTimeout(() => console.log("still running"), 20);
