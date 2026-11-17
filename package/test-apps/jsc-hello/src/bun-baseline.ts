// The same hello-world on Bun, through the regular Livewire SDK: the baseline
// the system-JSC host is measured against (T110).
import { BrowserWindow } from "electrobun/bun";

const started = Date.now();
const win = new BrowserWindow({
	title: "Livewire on Bun",
	frame: { x: 0, y: 0, width: 720, height: 480 },
	url: "views://main/index.html",
	activate: false,
});
console.log(`[app] window ${win.id}, main ran in ${Date.now() - started} ms`);
