// fetch and its types against a local Bun server (http-fixture.ts):
// responses, headers, bodies, redirects, streaming, aborts and errors, and
// the web globals that come with them (Headers, Request, Response, Blob,
// AbortController, EventTarget).
const origin = process.env.LIVEWIRE_HTTP;
const log = (...a) => console.log(...a);
const steps = [];
const step = (fn) => steps.push(fn);
const failure = (e) => [e?.name, e?.code ?? "-", e instanceof Error].join(" ");

step(async () => {
	const res = await fetch(origin + "/text");
	log("text", res.status, res.statusText, res.ok, res.redirected, res.url === origin + "/text", res.type);
	log("text headers", res.headers.get("content-type"), res.headers.get("X-Custom"), res.headers.has("x-custom"), res.headers.get("nope"));
	log("text body", res.bodyUsed, JSON.stringify(await res.text()), res.bodyUsed);
	await res.text().then(() => log("second read resolved"), (e) => log("second read", e.name));
	const json = await (await fetch(origin + "/json")).json();
	log("json", JSON.stringify(json));
	const missing = await fetch(origin + "/missing");
	log("missing", missing.status, missing.statusText, missing.ok, await missing.text());
});

step(async () => {
	// An untyped body's content type isn't printed: Bun sends none, and macOS's
	// network stack always sends one (the JSC host picks text or bytes).
	const echo = async (init) => {
		const got = await (await fetch(origin + "/echo", init)).json();
		const typed = init.body instanceof URLSearchParams || init.body instanceof Blob || new Headers(init.headers).has("content-type");
		if (!typed) delete got.contentType;
		return JSON.stringify(got);
	};
	log("post string", await echo({ method: "POST", body: "héllo", headers: { "x-a": "1", authorization: "Bearer t" } }));
	log("post bytes", await echo({ method: "PUT", body: new Uint8Array([1, 2, 3]) }));
	log("post params", await echo({ method: "POST", body: new URLSearchParams({ a: "1", b: "x y" }) }));
	log("post blob", await echo({ method: "POST", body: new Blob(["ab", new Uint8Array([99])], { type: "text/x-test" }) }));
	log("post json typed", await echo({ method: "POST", body: JSON.stringify({ k: 1 }), headers: { "content-type": "application/json" } }));
	log("headers object", await echo({ method: "POST", body: "", headers: new Headers([["X-A", "from Headers"]]) }));
	const req = new Request(origin + "/echo", { method: "POST", body: "from a Request", headers: { "x-a": "req" } });
	log("request", req.method, req.url === origin + "/echo", req.headers.get("x-a"), req.redirect);
	const fromRequest = await (await fetch(req)).json();
	delete fromRequest.contentType;
	log("fetch(request)", JSON.stringify(fromRequest));
	log("url object", (await fetch(new URL("/text", origin))).status);
});

step(async () => {
	const follow = await fetch(origin + "/redirect");
	log("follow", follow.status, follow.redirected, follow.url === origin + "/text", await follow.text());
	const manual = await fetch(origin + "/redirect", { redirect: "manual" });
	log("manual", manual.status, manual.redirected, manual.headers.get("location"), manual.url === origin + "/redirect");
	await fetch(origin + "/redirect", { redirect: "error" }).then(() => log("error mode resolved"), (e) => log("error mode rejected", e instanceof Error));
	const see = await (await fetch(origin + "/redirect-post", { method: "POST", body: "gone" })).json();
	log("303 becomes", see.method, see.length);
});

step(async () => {
	const big = await fetch(origin + "/big");
	const bytes = new Uint8Array(await big.arrayBuffer());
	let sum = 0;
	for (let i = 0; i < bytes.length; i++) sum = (sum + bytes[i] * (i % 7 + 1)) % 1000003;
	log("big", bytes.length, sum, big.headers.get("content-type"));
	const stream = await fetch(origin + "/stream");
	const reader = stream.body.getReader();
	let text = "";
	while (true) {
		const { value, done } = await reader.read();
		if (done) break;
		text += new TextDecoder().decode(value);
	}
	log("stream", JSON.stringify(text), value(stream.body.locked));
	const it = await fetch(origin + "/stream");
	let n = 0;
	for await (const chunk of it.body) n += chunk.length;
	log("stream iterated", n);
	const b = await (await fetch(origin + "/text")).bytes();
	log("bytes", b.constructor.name, b.length);
	const blob = await (await fetch(origin + "/text")).blob();
	log("blob", blob.size, blob.type, await blob.text());
});
const value = (v) => v;

step(async () => {
	const controller = new AbortController();
	setTimeout(() => controller.abort(), 50);
	await fetch(origin + "/slow", { signal: controller.signal }).then(() => log("abort resolved"), (e) => log("abort", failure(e), controller.signal.aborted));
	await fetch(origin + "/slow", { signal: AbortSignal.timeout(50) }).then(() => log("timeout resolved"), (e) => log("timeout", failure(e)));
	const reason = new Error("mine");
	await fetch(origin + "/text", { signal: AbortSignal.abort(reason) }).then(() => log("pre-aborted resolved"), (e) => log("pre-aborted", e === reason));
	await fetch("http://127.0.0.1:1/").then(() => log("refused resolved"), (e) => log("refused", failure(e)));
	await fetch("not a url").then(() => log("bad url resolved"), (e) => log("bad url", e.name));
});

step(async () => {
	const h = new Headers({ "Content-Type": "text/plain" });
	h.append("x-b", "1");
	h.append("X-B", "2");
	h.append("set-cookie", "a=1");
	h.append("set-cookie", "b=2");
	log("headers", h.get("x-b"), h.get("content-type"), JSON.stringify(h.getSetCookie()), h.has("X-B"));
	h.set("x-b", "3");
	h.delete("content-type");
	log("headers entries", JSON.stringify([...h]));
	log("headers keys", [...h.keys()].join(","));
	const seen = [];
	h.forEach((v, k) => seen.push(k + "=" + v));
	log("headers forEach", seen.join(" "));
	try {
		new Headers({ "bad name": "x" });
		log("bad header accepted");
	} catch (e) {
		log("bad header", e.name);
	}
	const r = new Response("made", { status: 201, statusText: "Made", headers: { "x-r": "1" } });
	log("response", r.status, r.statusText, r.ok, r.headers.get("x-r"), r.headers.get("content-type"), await r.clone().text(), await r.text());
	const j = Response.json({ ok: true }, { status: 202 });
	log("Response.json", j.status, j.headers.get("content-type"), await j.text());
	const red = Response.redirect("http://example.com/next", 307);
	log("Response.redirect", red.status, red.headers.get("location"));
	const e = Response.error();
	log("Response.error", e.status, e.type);
	const empty = new Response(null, { status: 204 });
	log("empty", empty.body, await empty.text());
	const rs = new Response(new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode("streamed")); c.close(); } }));
	log("stream body", await rs.text());
});

step(async () => {
	const blob = new Blob(["héllo", " ", new Uint8Array([119, 111])], { type: "Text/Plain" });
	log("blob", blob.size, blob.type, await blob.text(), (await blob.arrayBuffer()).byteLength);
	const part = blob.slice(1, 4, "x/y");
	log("slice", part.size, part.type, JSON.stringify(await part.text()));
	log("blob bytes", (await blob.bytes()).length, blob instanceof Blob);
	let streamed = 0;
	for await (const chunk of blob.stream()) streamed += chunk.length;
	log("blob stream", streamed);
});

step(async () => {
	const c = new AbortController();
	const seen = [];
	c.signal.addEventListener("abort", (e) => seen.push("listener " + e.type), { once: true });
	c.signal.onabort = () => seen.push("onabort");
	c.abort();
	c.abort();
	log("abort", c.signal.aborted, c.signal.reason.name, c.signal.reason.message, seen.join(", "));
	try {
		c.signal.throwIfAborted();
	} catch (e) {
		log("throwIfAborted", e.name);
	}
	const a = new AbortController(), b = new AbortController();
	const any = AbortSignal.any([a.signal, b.signal]);
	b.abort("why");
	log("any", any.aborted, any.reason);
	// AbortSignal.timeout's timer doesn't keep the process alive (as in Bun).
	const t = AbortSignal.timeout(20);
	const keep = setTimeout(() => {}, 1000);
	await new Promise((resolve) => t.addEventListener("abort", resolve));
	clearTimeout(keep);
	log("timeout signal", t.reason.name, t.reason.message, t.reason instanceof DOMException);
	const target = new EventTarget();
	const got = [];
	const fn = (e) => got.push(e.type + ":" + (e.target === target));
	target.addEventListener("ping", fn);
	target.addEventListener("ping", fn);
	target.dispatchEvent(new Event("ping"));
	target.removeEventListener("ping", fn);
	target.dispatchEvent(new Event("ping"));
	log("event target", got.join(" "));
	const d = new DOMException("gone", "NotFoundError");
	log("DOMException", d.name, d.message, d.code, d instanceof Error);
});

(async () => {
	for (const fn of steps) await fn();
	log("done");
})();
