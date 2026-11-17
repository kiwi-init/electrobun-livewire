// node:zlib on the system libz (__node.zlib, one shot per call). Streams
// collect their input and run once at the end, which is what pipeline() over a
// file or a download needs; zstd and brotli aren't in the OS's libz.
import { Buffer } from "buffer";
import { Transform } from "stream";

const n = globalThis.__node;

export const constants = {
	Z_NO_FLUSH: 0, Z_PARTIAL_FLUSH: 1, Z_SYNC_FLUSH: 2, Z_FULL_FLUSH: 3, Z_FINISH: 4, Z_BLOCK: 5,
	Z_OK: 0, Z_STREAM_END: 1, Z_NEED_DICT: 2, Z_ERRNO: -1, Z_STREAM_ERROR: -2, Z_DATA_ERROR: -3, Z_MEM_ERROR: -4, Z_BUF_ERROR: -5, Z_VERSION_ERROR: -6,
	Z_NO_COMPRESSION: 0, Z_BEST_SPEED: 1, Z_BEST_COMPRESSION: 9, Z_DEFAULT_COMPRESSION: -1,
	Z_FILTERED: 1, Z_HUFFMAN_ONLY: 2, Z_RLE: 3, Z_FIXED: 4, Z_DEFAULT_STRATEGY: 0,
	DEFLATE: 1, INFLATE: 2, GZIP: 3, GUNZIP: 4, DEFLATERAW: 5, INFLATERAW: 6, UNZIP: 7,
	Z_MIN_WINDOWBITS: 8, Z_MAX_WINDOWBITS: 15, Z_DEFAULT_WINDOWBITS: 15,
	Z_MIN_CHUNK: 64, Z_MAX_CHUNK: Infinity, Z_DEFAULT_CHUNK: 16384,
	Z_MIN_MEMLEVEL: 1, Z_MAX_MEMLEVEL: 9, Z_DEFAULT_MEMLEVEL: 8,
	Z_MIN_LEVEL: -1, Z_MAX_LEVEL: 9, Z_DEFAULT_LEVEL: -1,
};

function received(value) {
	if (value == null) return `Received ${value}`;
	if (typeof value === "function") return `Received function ${value.name}`;
	if (typeof value === "object") return value.constructor?.name ? `Received an instance of ${value.constructor.name}` : `Received ${JSON.stringify(value)}`;
	const shown = typeof value === "string" ? `'${value.length > 25 ? value.slice(0, 25) + "..." : value}'` : String(value);
	return `Received type ${typeof value} (${shown})`;
}

function bytes(input) {
	if (typeof input === "string") return Buffer.from(input, "utf8");
	if (input instanceof ArrayBuffer) return new Uint8Array(input);
	if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
	const err = new TypeError(`The "buffer" argument must be of type string, Buffer, TypedArray, DataView, or ArrayBuffer. ${received(input)}`);
	err.code = "ERR_INVALID_ARG_TYPE";
	throw err;
}

const toBuffer = (u8) => Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength);

// [compress, windowBits for the format]; options.windowBits replaces 15.
const formats = {
	deflate: [true, 0], inflate: [false, 0],
	gzip: [true, 16], gunzip: [false, 16],
	deflateRaw: [true, -1], inflateRaw: [false, -1],
	unzip: [false, 32],
};

function run(format, input, options) {
	const [compress, kind] = formats[format];
	const bits = options?.windowBits ?? 15;
	const wbits = kind === -1 ? -bits : bits + kind;
	return n.zlib(compress, wbits, bytes(input), compress ? (options?.level ?? -1) : undefined);
}

const sync = (format) => (input, options) => toBuffer(run(format, input, options));
const async = (format) => (input, options, callback) => {
	if (typeof options === "function") [options, callback] = [undefined, options];
	let result, error = null;
	try { result = toBuffer(run(format, input, options)); } catch (e) { error = e; }
	setImmediate(() => callback(error, result));
};

class Zlib extends Transform {
	constructor(format, options) {
		super(options);
		this._format = format;
		this._options = options;
		this._chunks = [];
	}
	_transform(chunk, encoding, callback) {
		this._chunks.push(typeof chunk === "string" ? Buffer.from(chunk, encoding) : chunk);
		callback();
	}
	_flush(callback) {
		let out;
		try { out = toBuffer(run(this._format, Buffer.concat(this._chunks), this._options)); } catch (e) { return callback(e); }
		this._chunks = [];
		callback(null, out);
	}
}
const stream = (format) => (options) => new Zlib(format, options);

export const deflateSync = sync("deflate"), inflateSync = sync("inflate");
export const gzipSync = sync("gzip"), gunzipSync = sync("gunzip");
export const deflateRawSync = sync("deflateRaw"), inflateRawSync = sync("inflateRaw");
export const unzipSync = sync("unzip");
export const deflate = async("deflate"), inflate = async("inflate");
export const gzip = async("gzip"), gunzip = async("gunzip");
export const deflateRaw = async("deflateRaw"), inflateRaw = async("inflateRaw");
export const unzip = async("unzip");
export const createDeflate = stream("deflate"), createInflate = stream("inflate");
export const createGzip = stream("gzip"), createGunzip = stream("gunzip");
export const createDeflateRaw = stream("deflateRaw"), createInflateRaw = stream("inflateRaw");
export const createUnzip = stream("unzip");
export const crc32 = (data, value = 0) => n.crc32(bytes(data), value);

const missing = (name) => () => {
	throw new Error(`zlib.${name} isn't available on the JSC runtime: the OS has no zstd or brotli library`);
};
export const createZstdCompress = missing("createZstdCompress"), createZstdDecompress = missing("createZstdDecompress");
export const zstdCompressSync = missing("zstdCompressSync"), zstdDecompressSync = missing("zstdDecompressSync");
export const createBrotliCompress = missing("createBrotliCompress"), createBrotliDecompress = missing("createBrotliDecompress");
export const brotliCompressSync = missing("brotliCompressSync"), brotliDecompressSync = missing("brotliDecompressSync");

export default {
	constants, crc32,
	deflateSync, inflateSync, gzipSync, gunzipSync, deflateRawSync, inflateRawSync, unzipSync,
	deflate, inflate, gzip, gunzip, deflateRaw, inflateRaw, unzip,
	createDeflate, createInflate, createGzip, createGunzip, createDeflateRaw, createInflateRaw, createUnzip,
	createZstdCompress, createZstdDecompress, zstdCompressSync, zstdDecompressSync,
	createBrotliCompress, createBrotliDecompress, brotliCompressSync, brotliDecompressSync,
};
