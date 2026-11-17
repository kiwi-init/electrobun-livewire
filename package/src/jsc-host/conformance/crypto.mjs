import { createCipheriv, createDecipheriv, randomBytes, randomUUID, randomInt, randomFillSync, getRandomValues } from "node:crypto";

const hex = (b) => Buffer.from(b).toString("hex");
let seed = 0x2545f491;
const det = (n) => { const b = Buffer.alloc(n); for (let i = 0; i < n; i++) { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; b[i] = seed >>> 24; } return b; };

const nist = [
	["00000000000000000000000000000000", "000000000000000000000000", "", "", "", "58e2fccefa7e3061367f1d57a4e7455a"],
	["00000000000000000000000000000000", "000000000000000000000000", "00000000000000000000000000000000", "", "0388dace60b6a392f328c2b971b2fe78", "ab6e47d42cec13bdf53a67b21257bddf"],
	["feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888", "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", "feedfacedeadbeeffeedfacedeadbeefabaddad2", "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091", "5bc94fbc3221a5db94fae95ae7121a47"],
	["feffe9928665731c6d6a8f9467308308", "cafebabefacedbad", "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", "feedfacedeadbeeffeedfacedeadbeefabaddad2", "61353b4c2806934a777ff51fa22a4755699b2a714fcdc6f83766e5f97b6c742373806900e49f24b22b097544d4896b424989b5e1ebac0f07c23f4598", "3612d2e79e3b0785561be14aaca2fccb"],
	["feffe9928665731c6d6a8f9467308308feffe9928665731c", "cafebabefacedbaddecaf888", "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", "feedfacedeadbeeffeedfacedeadbeefabaddad2", "3980ca0b3c00e841eb06fac4872a2757859e1ceaa6efd984628593b40ca1e19c7d773d00c144c525ac619d18c84a3f4718e2448b2fe324d9ccda2710", "2519498e80f1478f37ba55bd6d27618c"],
	["feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", "9313225df88406e555909c5aff5269aa6a7a9538534f7da1e4c303d2a318a728c3c0c95156809539fcf0e2429a6b525416aedbf5a0de6a57a637b39b", "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", "feedfacedeadbeeffeedfacedeadbeefabaddad2", "5a8def2f0c9e53f1f75d7853659e2a20eeb2b22aafde6419a058ab4f6f746bf40fc0c3b780f244452da3ebf1c5d82cdea2418997200ef82e44ae7e3f", "a44a8266ee1c8eb0c8b5d4cf5ae9f19a"],
];
for (const [i, [k, iv, pt, aad, ct, tag]] of nist.entries()) {
	const key = Buffer.from(k, "hex");
	const c = createCipheriv("aes-" + key.length * 8 + "-gcm", key, Buffer.from(iv, "hex"));
	c.setAAD(Buffer.from(aad, "hex"));
	const out = Buffer.concat([c.update(Buffer.from(pt, "hex")), c.final()]);
	console.log("nist", i + 1, hex(out) === ct, hex(c.getAuthTag()) === tag);
}

for (const [kl, ivl, len, aadl] of [[16, 12, 0, 0], [16, 12, 1, 0], [32, 12, 15, 7], [32, 12, 16, 16], [24, 16, 33, 0], [32, 8, 100, 5], [32, 1, 64, 0], [16, 60, 4097, 13], [32, 12, 262144, 32]]) {
	const key = det(kl), iv = det(ivl), data = det(len), aad = det(aadl);
	const c = createCipheriv("aes-" + kl * 8 + "-gcm", key, iv);
	if (aadl) c.setAAD(aad);
	const parts = [];
	for (let off = 0; off < len; off += 1000) parts.push(c.update(data.subarray(off, off + 1000)));
	parts.push(c.final());
	const ct = Buffer.concat(parts);
	const tag = c.getAuthTag();
	let sum = 0;
	for (const x of ct) sum = (Math.imul(sum, 31) + x) >>> 0;
	const d = createDecipheriv("aes-" + kl * 8 + "-gcm", key, iv);
	if (aadl) d.setAAD(aad);
	d.setAuthTag(tag);
	const pt = Buffer.concat([d.update(ct), d.final()]);
	console.log("gcm", kl, ivl, len, aadl, hex(tag), sum, pt.equals(data));
}

const key = det(32), iv = det(12);
const c = createCipheriv("aes-256-gcm", key, iv);
const ct = Buffer.concat([c.update("tamper me", "utf8"), c.final()]);
const tag = c.getAuthTag();
const open = (t, data = ct, aad) => {
	try {
		const d = createDecipheriv("aes-256-gcm", key, iv);
		if (aad) d.setAAD(aad);
		d.setAuthTag(t);
		return "ok " + Buffer.concat([d.update(data), d.final()]).toString("utf8");
	} catch (e) { return "rejected"; }
};
console.log("tag ok", open(tag));
console.log("tag flipped", open(Buffer.from(tag.map((b, i) => (i === 15 ? b ^ 1 : b)))));
console.log("data flipped", open(tag, Buffer.from(ct.map((b, i) => (i === 0 ? b ^ 1 : b)))));
console.log("aad added", open(tag, ct, Buffer.from("x")));
console.log("short tag 12", open(tag.subarray(0, 12)));
console.log("encodings", (() => { const e = createCipheriv("aes-128-gcm", det(16), det(12)); return e.update("hi", "utf8", "base64") + e.final("base64"); })());
try { createCipheriv("aes-256-gcm", det(31), det(12)); console.log("bad key accepted"); } catch { console.log("bad key rejected"); }

const r = randomBytes(32);
console.log("randomBytes", Buffer.isBuffer(r), r.length, new Set(r).size > 8);
randomBytes(8, (e, b) => console.log("randomBytes cb", e, b.length));
console.log("uuid", /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(randomUUID()), randomUUID() !== randomUUID());
let inRange = true;
for (let i = 0; i < 2000; i++) { const v = randomInt(5, 9); if (v < 5 || v >= 9 || !Number.isInteger(v)) inRange = false; }
console.log("randomInt", inRange);
const u32 = new Uint32Array(4);
console.log("getRandomValues", getRandomValues(u32) === u32, u32.some((x) => x !== 0));
console.log("crypto.getRandomValues", typeof globalThis.crypto?.getRandomValues, typeof globalThis.crypto?.randomUUID);
const fill = Buffer.alloc(16);
randomFillSync(fill, 8);
console.log("randomFillSync offset", fill.subarray(0, 8).every((x) => x === 0), fill.subarray(8).some((x) => x !== 0));
