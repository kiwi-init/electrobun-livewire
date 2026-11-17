const native = chrome.webview.hostObjects.sync.native;
const host = { echo: (n) => native.Echo(n), now: () => performance.now(), print: (s) => chrome.webview.postMessage(String(s)) };
host.print("BOOT");
const result = IDLE ? {} : workload(host);
const pings = 300;
const pong = () => new Promise((resolve) => {
	const onMessage = () => { chrome.webview.removeEventListener("message", onMessage); resolve(); };
	chrome.webview.addEventListener("message", onMessage);
	chrome.webview.postMessage("ping");
});
(async () => {
	const t = performance.now();
	if (!IDLE) {
		for (let i = 0; i < pings; i++) await pong();
		result.async_message_us = ((performance.now() - t) * 1000) / pings;
	}
	host.print("READY " + JSON.stringify(result));
})();
