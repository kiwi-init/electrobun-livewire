// node:tty on the host's isatty and TIOCGWINSZ.
import { Stdin, decorateOutput } from "./stdio.js";

export const isatty = (fd) => globalThis.__node.isatty(Number(fd));
export class ReadStream extends Stdin {}
export class WriteStream {
	constructor(fd) {
		decorateOutput(this, fd);
	}
	write(s) {
		globalThis.process.stdout.write.call({}, s);
		return true;
	}
}
export default { isatty, ReadStream, WriteStream };
