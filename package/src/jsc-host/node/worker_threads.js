// node:worker_threads on the Livewire JSC runtime, over node/worker.js.
import { EventEmitter } from "events";
import { Worker as WebWorker, isMainThread, parentPort, threadId, workerData } from "./worker.js";

// Node's Worker: an EventEmitter with message/error/exit, over the web one.
export class Worker extends EventEmitter {
	constructor(filename, options = {}) {
		super();
		this._worker = new WebWorker(filename, options);
		this._worker._quiet = true; // an unheard 'error' throws here instead
		this.threadId = this._worker.threadId;
		this._exit = new Promise((resolve) => {
			this._worker.addEventListener("close", (e) => { this.emit("exit", e.code); resolve(e.code); });
		});
		this._worker.addEventListener("message", (e) => this.emit("message", e.data));
		this._worker.addEventListener("error", (e) => this.emit("error", e.error));
		queueMicrotask(() => this.emit("online"));
	}
	postMessage(data, transfer) { this._worker.postMessage(data, transfer); }
	terminate() { this._worker.terminate(); return this._exit; }
	ref() { this._worker.ref(); return this; }
	unref() { this._worker.unref(); return this; }
}

export { isMainThread, parentPort, threadId, workerData };
export const resourceLimits = {};
export const SHARE_ENV = Symbol.for("nodejs.worker_threads.SHARE_ENV");
const environmentData = new Map();
export const setEnvironmentData = (key, value) => environmentData.set(key, value);
export const getEnvironmentData = (key) => environmentData.get(key);
export const markAsUntransferable = () => {};
export default { Worker, isMainThread, parentPort, threadId, workerData, resourceLimits, SHARE_ENV, setEnvironmentData, getEnvironmentData, markAsUntransferable };
