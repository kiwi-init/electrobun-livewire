// expect: ENOENT -2 chdir true
// expect: arch ok
try { process.chdir("/no/such/dir"); } catch (e) { console.log(e.code, e.errno, e.syscall, e instanceof Error); }
console.log(process.arch === "arm64" || process.arch === "x64" ? "arch ok" : "arch " + process.arch);
