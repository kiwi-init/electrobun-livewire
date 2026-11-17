// expect: done
const iv = setInterval(() => console.log("should not keep the loop alive"), 1000);
iv.unref();
setTimeout(() => console.log("done"), 5);
