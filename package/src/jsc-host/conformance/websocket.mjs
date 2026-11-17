// WebSocket against a local Bun server (http-fixture.ts), the way
// @kiwi/core's relay client uses it: subprotocols carrying a token,
// readyState, on* handlers and listeners, text and binary frames, closes
// from either side, and failures.
const origin = process.env.LIVEWIRE_HTTP;
const wsOrigin = origin.replace(/^http/, "ws");
const log = (...a) => console.log(...a);
const steps = [];
const step = (fn) => steps.push(fn);
const nextMessage = (ws) => new Promise((resolve) => ws.addEventListener("message", (e) => resolve(e.data), { once: true }));
const closed = (ws) => new Promise((resolve) => ws.addEventListener("close", resolve, { once: true }));

step(async () => {
	log("constants", WebSocket.CONNECTING, WebSocket.OPEN, WebSocket.CLOSING, WebSocket.CLOSED);
	const ws = new WebSocket(wsOrigin + "/ws", ["kiwi.v1", "s1.token"]);
	log("new", ws.readyState, ws.url === wsOrigin + "/ws", ws.binaryType, ws.bufferedAmount, JSON.stringify(ws.protocol), ws.OPEN);
	const welcome = nextMessage(ws);
	await new Promise((resolve) => (ws.onopen = resolve));
	log("open", ws.readyState, ws.protocol, typeof ws.extensions);
	log("welcome", await welcome);
	ws.send("hello");
	log("echo text", await nextMessage(ws));
	ws.send("unicode");
	log("unicode", await nextMessage(ws));
	ws.send(new Uint8Array([1, 2, 255]));
	const buf = await nextMessage(ws);
	log("echo binary", Buffer.isBuffer(buf), buf instanceof Uint8Array, [...buf].join(","));
	ws.binaryType = "arraybuffer";
	ws.send(new Uint8Array([7, 8]).buffer);
	const ab = await nextMessage(ws);
	log("arraybuffer", ab instanceof ArrayBuffer, ab.byteLength);
	ws.binaryType = "blob";
	ws.send(new Uint8Array([9]));
	const blob = await nextMessage(ws);
	log("blob", blob instanceof Blob, blob.size);
	ws.binaryType = "bogus";
	log("binaryType kept", ws.binaryType);
	ws.send("big");
	log("big", (await nextMessage(ws)).length);
	let handlers = 0;
	ws.onmessage = () => handlers++;
	ws.addEventListener("message", () => handlers++);
	ws.send("twice");
	await nextMessage(ws);
	log("handler and listener", handlers);
	ws.onmessage = null;
	const done = closed(ws);
	ws.close(1000, "done");
	log("closing", ws.readyState);
	const e = await done;
	log("client close", e.code, JSON.stringify(e.reason), e.wasClean, ws.readyState, e instanceof CloseEvent);
	ws.send("after close");
	log("send after close: no throw");
});

step(async () => {
	const ws = new WebSocket(wsOrigin + "/ws");
	const events = [];
	ws.onerror = () => events.push("error");
	await new Promise((resolve) => ws.addEventListener("open", resolve));
	log("no protocol", JSON.stringify(ws.protocol), await nextMessage(ws));
	ws.send("close-me");
	const e = await closed(ws);
	log("server close", e.code, JSON.stringify(e.reason), e.wasClean, ws.readyState, events.join(","));
});

step(async () => {
	const ws = new WebSocket("ws://127.0.0.1:1/");
	const events = [];
	ws.onopen = () => events.push("open");
	ws.onerror = (e) => events.push("error " + e.type);
	const e = await closed(ws);
	log("refused", events.join(","), e.code, e.wasClean, ws.readyState);
	try {
		new WebSocket("ftp://example.com/");
	} catch (error) {
		log("bad scheme", error.name);
	}
	for (const code of [999, 1001, 1005, 1015, 2999, 5000]) {
		try {
			new WebSocket(wsOrigin + "/ws").close(code);
			log("close code", code, "accepted");
		} catch (error) {
			log("close code", code, error.name);
		}
	}
	const early = new WebSocket(wsOrigin + "/ws");
	try {
		early.send("too soon");
	} catch (error) {
		log("send while connecting", error.name);
	}
	const earlyEvents = [];
	early.onerror = () => earlyEvents.push("error");
	early.close();
	const how = await closed(early);
	log("closed while connecting", early.readyState, how.code, how.wasClean, earlyEvents.join(","));
	const plain = new WebSocket(wsOrigin + "/ws");
	await new Promise((resolve) => (plain.onopen = resolve));
	plain.close();
	const none = await closed(plain);
	log("close() without a code", none.code, none.wasClean, JSON.stringify(none.reason));
	const coded = new WebSocket(wsOrigin + "/ws");
	await new Promise((resolve) => (coded.onopen = resolve));
	coded.close(4000);
	const only = await closed(coded);
	log("close(4000)", only.code, only.wasClean, JSON.stringify(only.reason));
});

(async () => {
	for (const fn of steps) await fn();
	log("done");
	process.exit(0);
})();
