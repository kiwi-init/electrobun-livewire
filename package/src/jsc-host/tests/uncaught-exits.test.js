// expect: alive 1
// expect: Uncaught boom
// exit: 1
// An uncaught error with no 'uncaughtException' listener ends the process with 1.
let n = 0;
setInterval(() => console.log("alive", ++n), 10);
setTimeout(() => { throw "boom"; }, 15);
