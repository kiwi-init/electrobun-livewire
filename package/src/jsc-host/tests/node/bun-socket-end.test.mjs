// expect: end returned part true
// expect: received 8388608 intact true
// expect: closed
// Bun.Socket.end(payload) with more than the kernel takes at once: the rest
// is sent as the socket drains, then it closes. (Bun 1.4.4 sends the first
// part and then neither sends the rest nor closes, so this can't be a
// conformance test against it.)
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const path = join(mkdtempSync(join(tmpdir(), "lw-end-")), "end.sock");
const payload = new Uint8Array(8 * 1024 * 1024);
for (let i = 0; i < payload.length; i++) payload[i] = i % 251;
const listener = Bun.listen({
	unix: path,
	socket: {
		open(s) {
			const r = s.end(payload);
			console.log("end returned part", r > 0 && r < payload.length);
		},
		data() {},
	},
});
let received = 0, intact = true;
Bun.connect({
	unix: path,
	socket: {
		data(s, data) {
			for (let i = 0; i < data.length; i++) if (data[i] !== (received + i) % 251) intact = false;
			received += data.length;
		},
		close() {
			console.log("received", received, "intact", intact);
			console.log("closed");
			listener.stop(true);
		},
	},
});
