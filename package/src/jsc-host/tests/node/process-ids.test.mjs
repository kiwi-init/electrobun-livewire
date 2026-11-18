// expect: getuid ok
// expect: getgid ok
// process.getuid/getgid exist (Channels checks its test driver's socket folder
// is the user's own with them) and match os.userInfo().
import os from "node:os";
console.log(process.getuid() === os.userInfo().uid && process.geteuid() === process.getuid() ? "getuid ok" : "getuid " + process.getuid?.());
console.log(process.getgid() === os.userInfo().gid && process.getegid() === process.getgid() ? "getgid ok" : "getgid " + process.getgid?.());
