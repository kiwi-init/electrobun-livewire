// expect: os ok
// expect: fs write/read ok
// expect: stat file true dir false size 5
// expect: ENOENT ENOENT
// expect: readdir a.txt,b
// expect: rm ok
// expect: random 16 true
// expect: uuid ok
// expect: gcm nist ok
// expect: gcm iv16+aad ok
// expect: gcm roundtrip hello livewire
// expect: gcm tamper rejected
// expect: Bun.file {"a":1} application/json;charset=utf-8
// expect: buffer 3
// expect: socket data 3 bytes
// expect: socket end
// expect: socket close
// expect: done
// Node shims (fs, os, crypto, net) and Bun.file on the JSC runtime.
import { mkdirSync, writeFileSync, readFileSync, statSync, lstatSync, existsSync, readdirSync, rmSync, writeSync, closeSync } from "node:fs";
import { homedir, tmpdir } from "os";
import { randomBytes, randomUUID, createCipheriv, createDecipheriv } from "crypto";
import { Socket } from "node:net";
import { join } from "node:path";

console.log(homedir().startsWith("/") && tmpdir().startsWith("/") && !tmpdir().endsWith("/") ? "os ok" : "os bad " + homedir() + " " + tmpdir());
const dir = join(tmpdir(), "livewire-jsc-test-" + process.pid);
mkdirSync(join(dir, "b"), { recursive: true });
writeFileSync(join(dir, "a.txt"), "hello");
console.log(readFileSync(join(dir, "a.txt"), "utf8") === "hello" && readFileSync(join(dir, "a.txt")).length === 5 ? "fs write/read ok" : "fs bad");
console.log("stat file", statSync(join(dir, "a.txt")).isFile(), "dir", lstatSync(join(dir, "a.txt")).isDirectory(), "size", statSync(join(dir, "a.txt")).size);
try { readFileSync(join(dir, "missing")); } catch (e) { console.log(e.code, e.message.split(":")[0]); }
console.log("readdir", readdirSync(dir).join(","));
rmSync(dir, { recursive: true, force: true });
console.log(existsSync(dir) ? "rm failed" : "rm ok");

const r = randomBytes(16);
console.log("random", r.length, Buffer.isBuffer(r));
console.log(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(randomUUID()) ? "uuid ok" : "uuid bad");

// NIST GCM test case 15 (AES-256, 96-bit IV, no AAD).
const k15 = Buffer.from("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", "hex");
const c15 = createCipheriv("aes-256-gcm", k15, Buffer.from("cafebabefacedbaddecaf888", "hex"));
const out15 = c15.update(Buffer.from("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255", "hex"), undefined, "hex") + c15.final("hex");
console.log(out15 === "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad" && c15.getAuthTag().toString("hex") === "b094dac5d93471bdec1a502270e3cc6c" ? "gcm nist ok" : "gcm nist bad " + out15);
// 128-bit IV with AAD; expected values from Bun's crypto.
const key = Buffer.alloc(32, 1), iv = Buffer.from("000102030405060708090a0b0c0d0e0f", "hex");
const c = createCipheriv("aes-256-gcm", key, iv);
c.setAAD(Buffer.from("hdr"));
const enc = c.update("hello livewire", "utf8", "hex") + c.final("hex");
const tag = c.getAuthTag();
console.log(enc === "415ad6262ce0ff68dfdb5553d9c9" && tag.toString("hex") === "7f887058445da5c7b5a62928cc15a2b5" ? "gcm iv16+aad ok" : "gcm bad " + enc + " " + tag.toString("hex"));
const d = createDecipheriv("aes-256-gcm", key, iv);
d.setAAD(Buffer.from("hdr"));
d.setAuthTag(tag);
console.log("gcm roundtrip", d.update(enc, "hex", "utf8") + d.final("utf8"));
const bad = createDecipheriv("aes-256-gcm", key, iv);
bad.setAAD(Buffer.from("hdr"));
bad.setAuthTag(Buffer.alloc(16));
try { bad.update(enc, "hex"); bad.final(); console.log("gcm tamper accepted"); } catch { console.log("gcm tamper rejected"); }

const jsonPath = join(tmpdir(), "livewire-jsc-" + process.pid + ".json");
writeFileSync(jsonPath, JSON.stringify({ a: 1 }));
const file = Bun.file(jsonPath);
// No top-level await: the bundle is a classic script (IIFE).
file.json().then((value) => {
	console.log("Bun.file", JSON.stringify(value), file.type);
	rmSync(jsonPath);
	console.log("buffer", Buffer.byteLength("héy", "utf8") - 1);
	pipeTest();
});

// Socket over a pipe's read end: the SDK's host-message wakeup.
function pipeTest() {
const [readFd, writeFd] = __test.pipe();
const socket = new Socket({ fd: readFd, readable: true, writable: false });
socket.on("data", (chunk) => console.log("socket data", chunk.length, "bytes"));
socket.on("end", () => console.log("socket end"));
socket.on("close", () => { console.log("socket close"); console.log("done"); });
writeSync(writeFd, "abc");
setTimeout(() => closeSync(writeFd), 20); // EOF on the read end
}
