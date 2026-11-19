host.print("BOOT");
const result = IDLE ? {} : workload(host);
host.print("READY " + JSON.stringify(result));
