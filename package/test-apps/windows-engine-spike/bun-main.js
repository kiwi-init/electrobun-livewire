import { dlopen } from "bun:ffi";
const lib = dlopen(process.argv[2], { echo: { args: ["i32"], returns: "i32" } });
const host = { echo: lib.symbols.echo, now: () => performance.now(), print: (s) => console.log(s) };
host.print("BOOT");
const result = IDLE ? {} : workload(host);
host.print("READY " + JSON.stringify(result));
setTimeout(() => {}, 6000);
