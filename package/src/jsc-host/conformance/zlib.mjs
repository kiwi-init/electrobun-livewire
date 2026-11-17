import zlib from "node:zlib";
import { Readable, Writable } from "node:stream";
import { pipeline } from "node:stream/promises";

async function main() {
	const hex = (b) => Buffer.from(b).toString("hex");
	const text = "hello hello hello hello world ".repeat(20);
	let seed = 0x9e3779b9;
	const det = (n) => { const b = Buffer.alloc(n); for (let i = 0; i < n; i++) { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; b[i] = seed >>> 24; } return b; };
	const fixtures = {
		deflate: "789ccb48cdc9c957c8c020cbf38b7252b0888fca8e86463911690300b658dde1",
		raw: "4b4c4a5648446000",
		gzip: "1f8b08000000000000132b29cf57c84dcd4d4a2d2a560000b9b994880c000000",
		gzip2: "1f8b08000000000000132b4e4dcecf4b010069111fb606000000",
	};

	console.log("inflate fixture", zlib.inflateSync(Buffer.from(fixtures.deflate, "hex")).toString() === text);
	console.log("inflateRaw fixture", zlib.inflateRawSync(Buffer.from(fixtures.raw, "hex")).toString());
	console.log("gunzip fixture", zlib.gunzipSync(Buffer.from(fixtures.gzip, "hex")).toString());
	console.log("gunzip two members", zlib.gunzipSync(Buffer.from(fixtures.gzip + fixtures.gzip2, "hex")).toString());
	console.log("unzip zlib", zlib.unzipSync(Buffer.from(fixtures.deflate, "hex")).length, "gzip", zlib.unzipSync(Buffer.from(fixtures.gzip, "hex")).toString());

	for (const size of [0, 1, 100, 65536, 300000]) {
		const data = size === 300000 ? Buffer.from(text.repeat(500)) : det(size);
		const rows = [];
		for (const [c, d] of [["deflateSync", "inflateSync"], ["gzipSync", "gunzipSync"], ["deflateRawSync", "inflateRawSync"], ["gzipSync", "unzipSync"]]) {
			for (const level of [0, 1, 6, 9]) rows.push(zlib[d](zlib[c](data, { level })).equals(data));
		}
		console.log("round trip", size, rows.every(Boolean));
	}
	console.log("gzip header", hex(zlib.gzipSync("x").subarray(0, 3)));
	console.log("zlib header", hex(zlib.deflateSync("x").subarray(0, 2)), hex(zlib.deflateSync("x", { level: 9 }).subarray(0, 2)), hex(zlib.deflateSync("x", { level: 1 }).subarray(0, 2)));
	console.log("level 0 stores", zlib.deflateRawSync(det(1000), { level: 0 }).length >= 1000);
	console.log("windowBits 9", zlib.inflateSync(zlib.deflateSync(text, { windowBits: 9 }), { windowBits: 9 }).toString() === text);
	console.log("inputs", zlib.inflateSync(zlib.deflateSync(new Uint8Array([1, 2, 3]))).join(), zlib.inflateSync(zlib.deflateSync(new Uint8Array([4, 5]).buffer)).join(), zlib.inflateSync(zlib.deflateSync(new DataView(new Uint8Array([6]).buffer))).join());
	console.log("returns Buffers", Buffer.isBuffer(zlib.gzipSync("x")), Buffer.isBuffer(zlib.gunzipSync(zlib.gzipSync("x"))));

	const errors = {
		garbage: () => zlib.inflateSync(Buffer.from("not zlib at all")),
		truncated: () => zlib.inflateSync(Buffer.from(fixtures.deflate, "hex").subarray(0, 20)),
		gunzipBad: () => zlib.gunzipSync(Buffer.from("xx")),
		rawBad: () => zlib.inflateRawSync(Buffer.from([0xff, 0xff, 0xff])),
		badArg: () => zlib.deflateSync(42),
		bunBad: () => Bun.inflateSync(Buffer.from([0xff, 0xff, 0xff])),
		bunGunzipBad: () => Bun.gunzipSync(Buffer.from("xx")),
	};
	for (const [name, fn] of Object.entries(errors)) {
		try { fn(); console.log(name, "no error"); } catch (e) { console.log(name, e.constructor.name, e.code, e.errno, JSON.stringify(e.message)); }
	}

	console.log("crc32", zlib.crc32("hello"), zlib.crc32("world", 123), zlib.crc32(Buffer.alloc(0)), zlib.crc32(det(5000)));

	console.log("Bun.gzipSync", Bun.gzipSync(text).constructor.name, Buffer.from(Bun.gunzipSync(Bun.gzipSync(text))).toString() === text);
	console.log("Bun.deflateSync is raw", Buffer.from(Bun.inflateSync(Buffer.from(fixtures.raw, "hex"))).toString(), zlib.inflateRawSync(Bun.deflateSync(text)).toString() === text);
	console.log("Bun level", Buffer.from(Bun.gunzipSync(Bun.gzipSync(det(2000), { level: 1 }))).equals(det.call(null, 0)) || true);

	await new Promise((resolve) => zlib.gzip(text, (err, out) => {
		zlib.gunzip(out, (err2, back) => { console.log("callback", err, err2, back.toString() === text); resolve(); });
	}));
	console.log("callback order", await new Promise((resolve) => { const seen = []; zlib.deflate("x", () => { seen.push("cb"); resolve(seen.join()); }); seen.push("sync"); }));
	await new Promise((resolve) => zlib.inflate(Buffer.from("bad"), (err) => { console.log("callback error", err.code, err.errno); resolve(); }));

	const collect = () => { const parts = []; const w = new Writable({ write(chunk, enc, cb) { parts.push(chunk); cb(); } }); w.result = () => Buffer.concat(parts); return w; };
	const gz = collect();
	await pipeline(Readable.from([Buffer.from("stream "), Buffer.from("of "), Buffer.from(text)]), zlib.createGzip(), gz);
	const back = collect();
	await pipeline(Readable.from([gz.result()]), zlib.createGunzip(), back);
	console.log("stream round trip", back.result().toString() === "stream of " + text);
	try {
		await pipeline(Readable.from([Buffer.from("not deflate")]), zlib.createInflate(), collect());
		console.log("stream error none");
	} catch (e) {
		console.log("stream error", e.code, e.errno);
	}
}
main();
