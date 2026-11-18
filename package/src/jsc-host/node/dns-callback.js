// node:dns: lookup with a callback.
import { lookup as lookupAsync } from "./dns.js";

export const lookup = (hostname, options, callback) => {
	if (typeof options === "function") [callback, options] = [options, {}];
	lookupAsync(hostname, options).then(
		(r) => (Array.isArray(r) ? callback(null, r) : callback(null, r.address, r.family)),
		(e) => callback(e),
	);
};
export const promises = { lookup: lookupAsync };
export default { lookup, promises };
