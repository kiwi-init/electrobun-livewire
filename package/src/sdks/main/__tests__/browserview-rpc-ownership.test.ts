import { afterAll, describe, expect, test } from "bun:test";
import { installFakeFFI } from "./fakeFFI";

const fake = installFakeFFI({
	sendHostMessageToWebviewViaTransport: () => true,
});
const { BrowserView } = await import("../core/BrowserView");

afterAll(() => fake.uninstall());

function messagesSentTo(id: number) {
	return fake
		.callsTo("sendHostMessageToWebviewViaTransport")
		.filter((params) => params["id"] === id)
		.map((params) => JSON.parse(String(params["messageJson"])) as { id: string });
}

function sharedRpc() {
	return BrowserView.defineRPC<{
		bun: { requests: {}; messages: {} };
		webview: { requests: {}; messages: { ping: number } };
	}>({ handlers: { requests: {}, messages: {} } });
}

describe("BrowserView rpc transport ownership", () => {
	test("removing an older view keeps a shared rpc talking to the newer one", async () => {
		const rpc = sharedRpc();
		const older = new BrowserView({ url: "views://older/index.html", rpc });
		const newer = new BrowserView({ url: "views://newer/index.html", rpc });

		older.remove();
		await rpc.send("ping", 1);

		expect(messagesSentTo(newer.id).map((message) => message.id)).toEqual(["ping"]);
		expect(messagesSentTo(older.id)).toEqual([]);
		newer.remove();
	});

	test("removing the view that owns the rpc stops its sends", async () => {
		const rpc = sharedRpc();
		const view = new BrowserView({ url: "views://only/index.html", rpc });

		view.remove();
		await rpc.send("ping", 2);

		expect(messagesSentTo(view.id)).toEqual([]);
		expect(fake.callsTo("webviewRemove")).toContainEqual({ id: view.id });
	});
});
