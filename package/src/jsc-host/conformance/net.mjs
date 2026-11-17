// node:net over unix sockets and TCP, in the shapes Channels' desktop main
// process uses (single-instance handoff, the daemon client): listen, connect,
// line protocols, half-close, backpressure, idle timeouts, errors, close.
import net from "node:net";
import { existsSync, writeFileSync, rmSync, mkdtempSync } from "node:fs";
import { join } from "node:path";

// Each run gets its own folder: the reference's unref'd server leaves its socket file.
const scratch = mkdtempSync(join(process.env.LIVEWIRE_SCRATCH, "net-"));
const log = (...a) => console.log(...a);
const steps = [];
const step = (fn) => steps.push(fn);
const errLine = (e) => [e.code, e.syscall, typeof e.errno, e.address === undefined ? "-" : "address", e.port === undefined ? "-" : "port"].join(" ");
const listen = (server, ...args) => new Promise((resolve, reject) => { server.once("error", reject); server.listen(...args, () => { server.off("error", reject); resolve(); }); });
const closed = (s) => new Promise((resolve) => s.once("close", resolve));

step(async () => {
	const path = join(scratch, "app.sock");
	const seen = [];
	const server = net.createServer((conn) => {
		seen.push("server: connection");
		let buf = "";
		conn.on("data", (chunk) => {
			buf += chunk.toString("utf8");
			const end = buf.indexOf("\n");
			if (end < 0) return;
			seen.push("server: line " + buf.slice(0, end));
			conn.end("ok\n");
		});
		conn.on("end", () => seen.push("server: end"));
		conn.on("close", (hadError) => seen.push("server: close " + hadError));
	});
	server.on("listening", () => seen.push("server: listening"));
	await listen(server, path);
	log("unix listening", server.address() === path, existsSync(path), server.listening);
	const client = net.connect(path);
	let answer = "";
	client.on("connect", () => {
		seen.push("client: connect");
		client.write(JSON.stringify({ kind: "show" }) + "\n");
	});
	client.on("data", (chunk) => (answer += chunk.toString("utf8")));
	client.on("end", () => seen.push("client: end"));
	const hadError = await new Promise((resolve) => client.on("close", resolve));
	await new Promise((resolve) => setTimeout(resolve, 20));
	log("client", JSON.stringify(answer), hadError, client.destroyed);
	log(seen.filter((s) => s.startsWith("client")).join(", "));
	log(seen.filter((s) => s.startsWith("server")).join(", "));
	await new Promise((resolve) => server.close(resolve));
	log("unix closed", server.listening, existsSync(path));
});

step(async () => {
	const missing = net.connect(join(scratch, "missing.sock"));
	missing.on("error", (e) => log("connect missing", errLine(e)));
	log("connect missing close", await closed(missing));
	const stale = join(scratch, "stale.sock");
	const s = net.createServer();
	await listen(s, stale);
	await new Promise((resolve) => s.close(resolve));
	writeFileSync(stale, "");
	rmSync(stale);
	const s2 = net.createServer();
	await listen(s2, stale);
	s2.close();
	const busy = join(scratch, "busy.sock");
	const a = net.createServer();
	await listen(a, busy);
	const b = net.createServer();
	try {
		await listen(b, busy);
		log("listen busy: no error");
	} catch (e) {
		log("listen busy", errLine(e));
	}
	a.close();
});

step(async () => {
	const server = net.createServer({ allowHalfOpen: false }, (conn) => {
		const chunks = [];
		conn.on("data", (c) => chunks.push(c));
		conn.on("end", () => {
			const all = Buffer.concat(chunks);
			log("tcp server got", all.length, all[0], all[all.length - 1]);
			let ok = true;
			for (let i = 0; i < 4 && ok; i++) ok = conn.write(all);
			log("tcp server write returned", ok);
			conn.end();
		});
	});
	await listen(server, 0, "127.0.0.1");
	const addr = server.address();
	log("tcp address", addr.address, addr.family, typeof addr.port, addr.port > 0);
	const client = net.createConnection({ port: addr.port, host: "127.0.0.1" });
	await new Promise((resolve) => client.on("connect", resolve));
	log("tcp client", client.remoteAddress, client.remotePort === addr.port, client.remoteFamily, client.localAddress, typeof client.localPort);
	client.setNoDelay(true);
	client.setKeepAlive(true, 1000);
	const payload = Buffer.alloc(2 * 1024 * 1024, 7);
	payload[0] = 1;
	payload[payload.length - 1] = 9;
	const wrote = client.write(payload);
	log("tcp client write returned", wrote);
	await new Promise((resolve) => (wrote ? resolve() : client.once("drain", resolve)));
	client.end();
	let received = 0;
	client.on("data", (c) => (received += c.length));
	await new Promise((resolve) => client.on("end", resolve));
	log("tcp client received", received, "bytesWritten", client.bytesWritten, "bytesRead", client.bytesRead);
	log("tcp client close", await closed(client));
	server.close();
	const refused = net.connect(addr.port, "127.0.0.1");
	refused.on("error", (e) => log("tcp refused", errLine(e), e.address, e.port === addr.port));
	await closed(refused);
});

step(async () => {
	const server = net.createServer((conn) => conn.on("data", () => {}));
	await listen(server, join(scratch, "idle.sock"));
	const client = net.connect({ path: join(scratch, "idle.sock") });
	await new Promise((resolve) => client.on("connect", resolve));
	const t0 = Date.now();
	await new Promise((resolve) => client.setTimeout(100, resolve));
	log("timeout fired", Date.now() - t0 >= 90, client.destroyed, client.timeout);
	client.setTimeout(0);
	client.destroy(new Error("boom"));
	client.on("error", (e) => log("destroy error", e.message));
	log("destroy close", await closed(client), client.destroyed);
	server.close();
});

step(async () => {
	const server = net.createServer((conn) => {
		conn.on("end", () => log("half-open server saw end, writable", conn.writable));
		conn.on("data", (c) => {
			setTimeout(() => {
				conn.write("late " + c);
				conn.end();
			}, 20);
		});
	});
	server.listen(join(scratch, "half.sock"));
	await new Promise((resolve) => server.on("listening", resolve));
	const client = net.connect(join(scratch, "half.sock"), () => client.end("hello"));
	client.setEncoding("utf8");
	let text = "";
	client.on("data", (d) => (text += d));
	await closed(client);
	log("half-open client", JSON.stringify(text));
	const s = net.createServer({ allowHalfOpen: true }, (conn) => {
		conn.on("end", () => {
			log("allowHalfOpen end, writable", conn.writable);
			conn.end("bye");
		});
	});
	s.listen(join(scratch, "half2.sock"));
	await new Promise((resolve) => s.on("listening", resolve));
	const c2 = net.connect(join(scratch, "half2.sock"), () => c2.end());
	let t2 = "";
	c2.on("data", (d) => (t2 += d));
	await closed(c2);
	log("allowHalfOpen client", JSON.stringify(t2));
	server.close();
	s.close();
});

step(async () => {
	log("isIP", net.isIP("127.0.0.1"), net.isIP("::1"), net.isIP("nope"), net.isIPv4("10.0.0.1"), net.isIPv6("::"));
	const server = net.createServer();
	await listen(server, join(scratch, "unref.sock"));
	server.unref();
	log("unref'd server: the process can exit");
});

(async () => {
	for (const fn of steps) await fn();
})();
