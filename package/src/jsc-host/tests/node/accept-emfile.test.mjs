// expect: child out of fds EMFILE
// expect: accepts while out of fds bounded true
// expect: accepted after freeing true
// A listener out of fds (EMFILE) neither spins on the connection waiting
// for it nor goes deaf: it retries on a timer, and once fds free up it
// accepts again. The child runs with ulimit -n 64 (Bun.listen and net.Server).
import { spawn } from "node:child_process";
import { openSync, closeSync, mkdtempSync } from "node:fs";
import { createServer } from "node:net";

const [self, script, role, dir] = process.argv;

if (role === "child") {
	let calls = 0, opened = 0;
	const real = globalThis.__node.sockAccept;
	globalThis.__node.sockAccept = (fd) => (calls++, real(fd));
	const listener = Bun.listen({ unix: dir + "/b.sock", socket: { open() { opened++; }, data() {} } });
	const server = createServer(() => opened++).on("error", () => {});
	server.listen(dir + "/n.sock", () => {
		const held = [];
		try {
			for (;;) held.push(openSync("/dev/null", "r"));
		} catch (e) {
			console.log("full", e.code);
		}
		setTimeout(() => {
			console.log("calls", calls);
			for (const fd of held) closeSync(fd);
			opened = 0;
			console.log("freed");
			setTimeout(() => {
				console.log("opened", opened);
				listener.stop(true);
				server.close();
				process.exit(0);
			}, 800);
		}, 1000);
	});
} else {
	const dir = mkdtempSync("/tmp/lw-emfile-");
	const child = spawn("sh", ["-c", 'ulimit -n 64; exec "$0" "$@"', self, script, "child", dir], { stdio: ["ignore", "pipe", "inherit"] });
	const clients = [];
	const connect = () => {
		for (const name of ["b.sock", "n.sock"]) Bun.connect({ unix: dir + "/" + name, socket: { data() {} } }).then((s) => clients.push(s), () => {});
	};
	let buffered = "";
	child.stdout.on("data", (chunk) => {
		buffered += chunk;
		const lines = buffered.split("\n");
		buffered = lines.pop();
		for (const line of lines) {
			const [word, value] = line.split(" ");
			if (word === "full") {
				console.log("child out of fds", value);
				connect();
			} else if (word === "calls") {
				// A spinning loop makes thousands of calls a second; a 100 ms retry, about 20.
				console.log("accepts while out of fds bounded", Number(value) < 100);
			} else if (word === "freed") {
				connect();
			} else if (word === "opened") {
				console.log("accepted after freeing", Number(value) >= 2);
				for (const s of clients) s.terminate();
			}
		}
	});
}
