// expect: worker callbacks ran true
// A Worker's FinalizationRegistry callbacks run too: each Worker's runtime
// has its own loop and VM, made the same way as the main one.
import { isMainThread, parentPort, Worker } from "node:worker_threads";

if (isMainThread) {
	const worker = new Worker(process.argv[1]);
	worker.on("message", (ran) => {
		console.log("worker callbacks ran", ran);
		worker.terminate();
	});
} else {
	let calls = 0;
	const registry = new FinalizationRegistry(() => calls++);
	setTimeout(() => {
		for (let i = 0; i < 100; i++) registry.register({ i, pad: new Array(100).fill(i) }, i);
	}, 0);
	// JSC scans the stack conservatively: a few targets may survive.
	setTimeout(() => parentPort.postMessage(calls >= 80), 2000);
}
