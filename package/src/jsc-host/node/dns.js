// node:dns/promises and node:dns's lookup, on getaddrinfo off the JS thread.
const resolveName = (hostname, options = {}) =>
	new Promise((resolve, reject) => {
		if (typeof options === "number") options = { family: options };
		globalThis.__node.lookup(String(hostname), (code, list) => {
			if (code) {
				const error = new Error("getaddrinfo " + code + " " + hostname);
				error.code = code;
				error.syscall = "getaddrinfo";
				error.hostname = hostname;
				return reject(error);
			}
			let found = options.family ? list.filter((a) => a.family === options.family) : list;
			if (options.order !== "verbatim" && !options.verbatim) found = found.filter((a) => a.family === 4).concat(found.filter((a) => a.family === 6));
			if (!found.length) {
				const error = new Error("getaddrinfo ENOTFOUND " + hostname);
				error.code = "ENOTFOUND";
				error.syscall = "getaddrinfo";
				error.hostname = hostname;
				return reject(error);
			}
			resolve(options.all ? found.map((a) => ({ address: a.address, family: a.family })) : { address: found[0].address, family: found[0].family });
		});
	});
export const lookup = resolveName;
export const promises = { lookup };
export default { lookup, promises };
