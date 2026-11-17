import { createInterface } from "node:readline";
import { Readable, PassThrough } from "node:stream";

async function main() {
	const run = (chunks) => new Promise((resolve) => {
		const events = [];
		const rl = createInterface({ input: Readable.from(chunks) });
		rl.on("line", (line) => events.push(JSON.stringify(line)));
		rl.on("close", () => { events.push("close"); resolve(events.join(" ")); });
	});

	console.log("lf", await run(["a\nb\n", "c"]));
	console.log("crlf", await run(["one\r\ntwo\r\n"]));
	console.log("crlf split", await run(["one\r", "\ntwo\r", "\n"]));
	console.log("lone cr", await run(["x\ry\rz"]));
	console.log("blank lines", await run(["\n\n", "a\n\n"]));
	console.log("no trailing newline", await run(["tail"]));
	console.log("empty", await run([]));
	const euro = Buffer.from("price: €5\nnext\n");
	console.log("split utf-8", await run([euro.subarray(0, 9), euro.subarray(9)]));

	const iterated = [];
	for await (const line of createInterface({ input: Readable.from(["it 1\nit 2\n", "it 3"]) })) iterated.push(line);
	console.log("async iterator", iterated.join(","));

	const early = [];
	for await (const line of createInterface({ input: Readable.from(["stop\nnever\n"]) })) { early.push(line); break; }
	console.log("break", early.join(","));

	const input = new PassThrough();
	const output = new PassThrough();
	let shown = "";
	output.on("data", (d) => (shown += d));
	const rl = createInterface({ input, output });
	const answer = await new Promise((resolve) => { rl.question("name? ", resolve); input.write("kiwi\n"); });
	console.log("question", JSON.stringify(shown), answer);
	const after = new Promise((resolve) => rl.once("line", resolve));
	input.write("then a line\n");
	console.log("line after question", await after);
	let closed = 0;
	rl.on("close", () => closed++);
	rl.close();
	rl.close();
	console.log("close once", closed, rl.closed);
}
main();
