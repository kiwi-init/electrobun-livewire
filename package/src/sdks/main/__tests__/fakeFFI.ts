// A stand-in for the native libraries, so BrowserView/BrowserWindow logic runs
// under `bun test`. Install it before importing the SDK:
//
//   const fake = installFakeFFI();
//   const { BrowserView } = await import("../core/BrowserView");
//
// Every ffi.request call is recorded in `calls`. Requests without a handler
// return undefined, except the creators, which hand out ids, and the pointer
// lookups, which return a non-null pointer for any id.

type Params = Record<string, unknown>;
type Handler = (params: Params) => unknown;

export type FakeFFICall = { method: string; params: Params };

const FAKE_FFI_KEY = Symbol.for("electrobun.fakeFFI");

export function installFakeFFI(handlers: Record<string, Handler> = {}) {
	const calls: FakeFFICall[] = [];
	let nextId = 1;
	const defaults: Record<string, Handler> = {
		createWindow: () => nextId++,
		createWebview: () => nextId++,
		getWebviewPointer: ({ id }) => id,
		getWindowPointer: ({ winId }) => winId,
	};
	const fake = new Proxy(
		{},
		{
			get: (_target, method) =>
				typeof method === "string"
					? (params: Params = {}) => {
							calls.push({ method, params });
							return (handlers[method] ?? defaults[method])?.(params);
						}
					: undefined,
		},
	);
	(globalThis as Record<PropertyKey, unknown>)[FAKE_FFI_KEY] = fake;

	return {
		calls,
		callsTo(method: string) {
			return calls.filter((call) => call.method === method).map((call) => call.params);
		},
		uninstall() {
			delete (globalThis as Record<PropertyKey, unknown>)[FAKE_FFI_KEY];
		},
	};
}
