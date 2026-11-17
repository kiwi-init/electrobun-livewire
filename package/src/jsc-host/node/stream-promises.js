// node:stream/promises over the stream polyfill's callback forms.
import { pipeline as pipelineCallback, finished as finishedCallback } from "stream";

export const pipeline = (...streams) =>
	new Promise((resolve, reject) => pipelineCallback(...streams, (err, value) => (err ? reject(err) : resolve(value))));
export const finished = (stream, options) =>
	new Promise((resolve, reject) => finishedCallback(stream, options ?? {}, (err) => (err ? reject(err) : resolve())));
export default { pipeline, finished };
