// expect: Uncaught (in promise) nope
// exit: 1
// An unhandled rejection with no listener ends the process with 1.
setInterval(() => {}, 1000);
Promise.reject("nope");
