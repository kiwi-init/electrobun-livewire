// oracle: node
import { Socket } from "node:net";
import { openSync, writeSync, closeSync } from "node:fs";

const fifo = process.env.LIVEWIRE_FIFO;
const readFd = openSync(fifo, "r+");
const writeFd = openSync(fifo, "w");
const socket = new Socket({ fd: readFd, readable: true, writable: false });
const chunks = [];
let step = 0;
socket.on("data", (chunk) => {
	chunks.push(chunk);
	step++;
	if (step <= 2) console.log("data", Buffer.isBuffer(chunk), chunk.length);
	if (step === 1) setTimeout(() => writeSync(writeFd, "second write"), 10);
	else if (step === 2) setTimeout(() => writeSync(writeFd, Buffer.alloc(8000, 120)), 10);
	if (Buffer.concat(chunks).length === 3 + 12 + 8000) {
		console.log("total", Buffer.concat(chunks).toString("latin1", 0, 15));
		socket.destroy();
	}
});
socket.on("end", () => console.log("end"));
socket.on("error", (e) => console.log("error", e.code));
socket.on("close", (hadError) => {
	console.log("close", hadError, socket.destroyed);
	closeSync(writeFd);
	setTimeout(() => console.log("loop still ran after close"), 10);
});
console.log("readable", socket.readable, "writable", socket.writable);
writeSync(writeFd, "abc");
