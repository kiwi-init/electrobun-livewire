// The typed RPC between the test app's main process and its page.
import type { RPCSchema } from "electrobun/bun";

export type TestRPC = {
	bun: RPCSchema<{
		requests: { hello: { params: { name: string }; response: string } };
		messages: { ready: {} };
	}>;
	webview: RPCSchema<{
		requests: { add: { params: { a: number; b: number }; response: number } };
		messages: {};
	}>;
};
