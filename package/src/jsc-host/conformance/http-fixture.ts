// The HTTP and WebSocket server the fetch and WebSocket conformance tests
// talk to, on both the reference runtime and the JSC host. run.ts starts it
// and passes its origin as LIVEWIRE_HTTP. Prints the origin, then serves
// until killed. With a path argument, it also serves its WebSocket on that
// unix socket (for ws+unix://), whatever the request path.
const big = new Uint8Array(3 * 1024 * 1024).map((_, i) => i % 251);
const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

function upgrade(req: Request, server: Bun.Server<{ protocol: string }>) {
	const offered = (req.headers.get("sec-websocket-protocol") ?? "").split(",").map((s) => s.trim()).filter(Boolean);
	const protocol = offered[0] ?? "";
	const headers: Record<string, string> = protocol ? { "Sec-WebSocket-Protocol": protocol } : {};
	if (server.upgrade(req, { data: { protocol }, headers })) return undefined;
	return new Response("not a websocket", { status: 400 });
}
const websocket: Bun.WebSocketHandler<{ protocol: string }> = {
	open(ws) {
		ws.send("welcome " + ws.data.protocol);
	},
	message(ws, message) {
		if (message === "close-me") return void ws.close(4001, "bye");
		if (message === "big") return void ws.send("x".repeat(200_000));
		if (message === "unicode") return void ws.send("héllo 🌍 ✓");
		ws.send(message);
	},
};

const server = Bun.serve<{ protocol: string }, never>({
	hostname: "127.0.0.1",
	port: 0,
	async fetch(req, server) {
		const url = new URL(req.url);
		switch (url.pathname) {
			case "/text":
				return new Response("hello world", { headers: { "content-type": "text/plain;charset=utf-8", "x-custom": "yes" } });
			case "/json":
				return Response.json({ a: 1, b: [true, null] });
			case "/echo": {
				const body = new Uint8Array(await req.arrayBuffer());
				return Response.json({
					method: req.method,
					contentType: req.headers.get("content-type"),
					xa: req.headers.get("x-a"),
					auth: req.headers.get("authorization"),
					length: body.length,
					text: new TextDecoder().decode(body),
				});
			}
			case "/redirect":
				return new Response(null, { status: 302, headers: { location: "/text" } });
			case "/redirect-post":
				return new Response(null, { status: 303, headers: { location: "/echo" } });
			case "/missing":
				return new Response("nope", { status: 404 });
			case "/big":
				return new Response(big, { headers: { "content-type": "application/octet-stream" } });
			case "/slow":
				await sleep(3000);
				return new Response("late");
			case "/stream":
				return new Response(
					new ReadableStream({
						async start(controller) {
							for (let i = 0; i < 4; i++) {
								controller.enqueue(new TextEncoder().encode("part " + i + "\n"));
								await sleep(30);
							}
							controller.close();
						},
					}),
					{ headers: { "content-type": "text/plain" } },
				);
			case "/ws":
				return upgrade(req, server);
		}
		return new Response("unknown " + url.pathname, { status: 500 });
	},
	websocket,
});
const unix = process.argv[2];
if (unix) Bun.serve<{ protocol: string }, never>({ unix, fetch: (req, s) => upgrade(req, s), websocket });
console.log(`http://127.0.0.1:${server.port}`);
