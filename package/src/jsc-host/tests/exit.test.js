// expect: before
setTimeout(() => console.log("never"), 50);
console.log("before");
process.exit(0);
