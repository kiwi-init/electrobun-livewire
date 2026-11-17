// Hello-world Livewire main process for the system-JSC host spike (T109).
// Bundled at build time into one classic script (Resources/app.js); the host
// provides globalThis.__host. A real build would generate this binding layer
// from the SDK's FFI declarations instead of hand-writing it.

type Host = {
	print(message: string): void;
	readFile(path: string): string | null;
	lastError(): string;
	randomBytes(count: number): string;
	resourcesDir: string;
	configureWebviewRuntime(rpcPort: number, fullPreload: string, sandboxedPreload: string): boolean;
	getWindowStyle(...flags: boolean[]): number;
	createWindow(
		x: number, y: number, width: number, height: number, styleMask: number, titleBarStyle: string,
		transparent: boolean, title: string, hidden: boolean, activate: boolean, centered: boolean,
		trafficLightX: number, trafficLightY: number,
	): number;
	setNextWebviewAllowedProtocols(views: boolean, appData: boolean): void;
	createWebview(
		windowId: number, hostWebviewId: number, renderer: string, url: string,
		x: number, y: number, width: number, height: number, autoResize: boolean, partition: string,
		secretKey: string, preload: string, viewsRoot: string,
		sandbox: boolean, startTransparent: boolean, startPassthrough: boolean, spellCheck: boolean,
	): number;
};

const host = (globalThis as unknown as { __host: Host }).__host;
const started = Date.now();

function must<T>(value: T, what: string): T {
	if (!value) throw new Error(`${what} failed: ${host.lastError()}`);
	return value;
}

const res = host.resourcesDir;
must(
	host.configureWebviewRuntime(0, host.readFile(`${res}/preload-full.js`) ?? "", host.readFile(`${res}/preload-sandboxed.js`) ?? ""),
	"configureWebviewRuntime",
);

const style = host.getWindowStyle(false, true, true, true, true, false, false, false, false, false, false, false);
const width = 720;
const height = 480;
const activate = (globalThis as { __activate?: boolean }).__activate ?? false;
const windowId = must(
	host.createWindow(0, 0, width, height, style, "default", false, "Livewire on system JavaScriptCore", false, activate, true, 0, 0),
	"createWindow",
);

host.setNextWebviewAllowedProtocols(true, false);
const webviewId = must(
	host.createWebview(windowId, 0, "native", "views://main/index.html", 0, 0, width, height, true, "persist:default",
		host.randomBytes(32), "", "", true, false, false, false),
	"createWebview",
);

host.print(`[app] window ${windowId}, webview ${webviewId}, main ran in ${Date.now() - started} ms`);
