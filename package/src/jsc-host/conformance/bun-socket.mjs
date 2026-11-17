// Bun.listen and Bun.connect over unix sockets and TCP, as Channels' daemon
// uses them: handler objects, partial writes and drain, end, stop, errors.
import { existsSync, mkdtempSync } from "node:fs";
import { join } from "node:path";

const scratch = mkdtempSync(join(process.env.LIVEWIRE_SCRATCH, "bunsock-"));
const log = (...a) => console.log(...a);
const steps = [];
const step = (fn) => steps.push(fn);
const text = (data) => new TextDecoder().decode(data);

step(async () => {
	const path = join(scratch, "daemon.sock");
	const server = [], client = [];
	let done;
	const finished = new Promise((resolve) => (done = resolve));
	const listener = Bun.listen({
		unix: path,
		data: { kind: "conn" },
		socket: {
			open(s) { server.push("open " + JSON.stringify(s.data) + " " + s.readyState); },
			data(s, data) {
				server.push("data " + JSON.stringify(text(data)) + " " + Buffer.isBuffer(data));
				server.push("write " + s.write("pong\n"));
				s.end();
			},
			end() { server.push("end"); },
			close(s, error) { server.push("close " + (error === undefined ? "-" : String(error))); },
			error(s, e) { server.push("error " + e.code); },
		},
	});
	log("listen unix", listener.unix === path, existsSync(path), typeof listener.stop);
	const socket = await Bun.connect({
		unix: path,
		socket: {
			open(s) { client.push("open " + s.readyState); },
			data(s, data) { client.push("data " + JSON.stringify(text(data))); },
			end() { client.push("end"); },
			close(s, error) { client.push("close " + (error === undefined ? "-" : String(error))); done(); },
			error(s, e) { client.push("error " + e.code); },
		},
	});
	log("connect resolved", typeof socket.write, socket.readyState);
	log("client write", socket.write(JSON.stringify({ verb: "hello" }) + "\n"));
	await finished;
	await new Promise((resolve) => setTimeout(resolve, 30));
	log("client:", client.join(", "));
	log("server:", server.join(", "));
	listener.stop(true);
	log("stopped, socket file", existsSync(path));
});

step(async () => {
	const events = [];
	try {
		await Bun.connect({
			unix: join(scratch, "missing.sock"),
			socket: { data() {}, connectError(s, e) { events.push("connectError " + e.code); } },
		});
		log("connect missing resolved");
	} catch (e) {
		log("connect missing rejected", e.code, e.syscall, typeof e.errno, events.join(","));
	}
	try {
		await Bun.connect({ hostname: "127.0.0.1", port: 1, socket: { data() {}, connectError(s, e) { events.push("tcp connectError " + e.code); } } });
		log("tcp refused resolved");
	} catch (e) {
		log("tcp refused rejected", e.code, events.join(","));
	}
});

step(async () => {
	let received = 0, serverEnd;
	const ended = new Promise((resolve) => (serverEnd = resolve));
	const listener = Bun.listen({
		hostname: "127.0.0.1",
		port: 0,
		socket: {
			data(s, data) { received += data.length; },
			close() { serverEnd(); },
		},
	});
	log("listen tcp", listener.hostname, typeof listener.port, listener.port > 0);
	const payload = new Uint8Array(4 * 1024 * 1024).fill(3);
	let offset = 0, drains = 0, sent;
	const allSent = new Promise((resolve) => (sent = resolve));
	const pump = (s) => {
		while (offset < payload.length) {
			const n = s.write(payload.subarray(offset));
			if (n <= 0) return;
			offset += n;
			if (n < payload.length - offset + n) return;
		}
		sent();
	};
	const socket = await Bun.connect({
		hostname: "127.0.0.1",
		port: listener.port,
		socket: {
			open(s) { pump(s); },
			drain(s) { drains++; pump(s); },
			data() {},
		},
	});
	await allSent;
	// Whether drain fires depends on how much the kernel took at once (on Intel
	// macOS it can take all 4 MB), so only what arrived is printed.
	void drains;
	log("tcp sent", offset, socket.remoteAddress, typeof socket.localPort);
	socket.end();
	await ended;
	log("tcp received", received);
	listener.stop();
});

step(async () => {
	const path = join(scratch, "timeout.sock");
	const listener = Bun.listen({ unix: path, socket: { data() {} } });
	let timedOut;
	const fired = new Promise((resolve) => (timedOut = resolve));
	const socket = await Bun.connect({ unix: path, socket: { data() {}, timeout(s) { timedOut(s === socket); } } });
	socket.timeout(1);
	const t0 = Date.now();
	log("timeout handler", await fired, Date.now() - t0 >= 900);
	socket.terminate();
	listener.stop(true);
});

(async () => {
	for (const fn of steps) await fn();
	log("done");
})();
