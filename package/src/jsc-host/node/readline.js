// node:readline isn't available on the JSC runtime yet.
export function createInterface() {
	throw new Error("readline.createInterface isn't available on the JSC runtime yet");
}
export default { createInterface };
