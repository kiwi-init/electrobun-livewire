// crypto.subtle and node:crypto beyond AES-GCM: known vectors, and the
// shapes Channels' main process uses (X25519 + HKDF -> AES-GCM, Ed25519 with
// JWK and minisign-style verify, PBKDF2-SHA1 -> AES-CBC, SHA-256, BLAKE2b).
import * as nc from "node:crypto";
const { subtle } = globalThis.crypto;
const hex = (b) => Buffer.from(b instanceof ArrayBuffer ? new Uint8Array(b) : b).toString("hex");
const unhex = (h) => Buffer.from(h, "hex");
const enc = (s) => new TextEncoder().encode(s);
const tryit = async (label, fn) => { try { console.log(label, await fn()); } catch (e) { console.log(label, "throws", e?.name); } };

// The bundle is an IIFE: no top-level await.
async function main() {
	// Digests and HMAC (FIPS 180, RFC 4231).
	for (const h of ["SHA-1", "SHA-256", "SHA-384", "SHA-512"]) console.log("digest", h, hex(await subtle.digest(h, enc("abc"))));
	console.log("digest empty", hex(await subtle.digest({ name: "sha-256" }, new Uint8Array(0))));
	const million = new Uint8Array(1_000_000).fill(0x61);
	console.log("digest 1M a", hex(await subtle.digest("SHA-256", million)));
	for (const h of ["sha1", "sha256", "sha384", "sha512", "blake2b512"]) {
		const x = nc.createHash(h).update("ab").update(Buffer.from("c"));
		const y = x.copy();
		console.log("createHash", h, x.digest("hex"), y.digest("base64"));
	}
	console.log("hash()", nc.createHash("sha256").update("héllo", "utf8").digest("base64url"));
	const rfcKey = unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
	for (const h of ["sha256", "sha384", "sha512"]) console.log("hmac", h, nc.createHmac(h, rfcKey).update("Hi There").digest("hex"));
	console.log("hmac empty key", nc.createHmac("sha256", "").update("x").digest("hex"));
	const hk = await subtle.importKey("raw", enc("key"), { name: "HMAC", hash: "SHA-256" }, true, ["sign", "verify"]);
	const mac = await subtle.sign("HMAC", hk, enc("The quick brown fox jumps over the lazy dog"));
	console.log("subtle hmac", hex(mac), await subtle.verify("HMAC", hk, mac, enc("The quick brown fox jumps over the lazy dog")), await subtle.verify("HMAC", hk, mac, enc("x")));
	console.log("hmac key", hk.type, hk.extractable, JSON.stringify(hk.algorithm), hk.usages.join(","), JSON.stringify(await subtle.exportKey("jwk", hk)));
	const gh = await subtle.generateKey({ name: "HMAC", hash: "SHA-512" }, true, ["sign"]);
	console.log("generated hmac", gh.algorithm.length, (await subtle.exportKey("raw", gh)).byteLength);

	// PBKDF2 (RFC 6070) and the legacy store's PBKDF2-SHA1 -> AES-128-CBC.
	console.log("pbkdf2Sync", nc.pbkdf2Sync("password", "salt", 4096, 20, "sha1").toString("hex"));
	console.log("pbkdf2Sync 256", nc.pbkdf2Sync("passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 40, "sha256").toString("hex"));
	const pk = await subtle.importKey("raw", enc("peanuts"), "PBKDF2", false, ["deriveKey", "deriveBits"]);
	const aesCbc = await subtle.deriveKey({ name: "PBKDF2", salt: enc("saltysalt"), iterations: 1003, hash: "SHA-1" }, pk, { name: "AES-CBC", length: 128 }, true, ["encrypt", "decrypt"]);
	console.log("pbkdf2 -> aes-cbc", hex(await subtle.exportKey("raw", aesCbc)), JSON.stringify(aesCbc.algorithm));
	const iv = new Uint8Array(16).fill(0x20);
	for (const len of [0, 1, 15, 16, 17, 100]) {
		const pt = new Uint8Array(len).map((_, i) => i * 7);
		const ct = await subtle.encrypt({ name: "AES-CBC", iv }, aesCbc, pt);
		const back = await subtle.decrypt({ name: "AES-CBC", iv }, aesCbc, ct);
		const c = nc.createCipheriv("aes-128-cbc", Buffer.from(await subtle.exportKey("raw", aesCbc)), iv);
		const nct = Buffer.concat([c.update(pt), c.final()]);
		const d = nc.createDecipheriv("aes-128-cbc", Buffer.from(await subtle.exportKey("raw", aesCbc)), iv);
		console.log("aes-cbc", len, hex(ct), hex(back) === hex(pt), hex(nct) === hex(ct), Buffer.concat([d.update(nct), d.final()]).equals(Buffer.from(pt)));
	}
	await tryit("aes-cbc bad data", () => subtle.decrypt({ name: "AES-CBC", iv }, aesCbc, new Uint8Array(15)));
	await tryit("pbkdf2 bits", async () => hex(await subtle.deriveBits({ name: "PBKDF2", salt: enc("salt"), iterations: 2, hash: "SHA-256" }, pk, 256)));

	// HKDF (RFC 5869 case 1) and X25519 (RFC 7748) -> HKDF -> AES-GCM.
	const ikm = await subtle.importKey("raw", unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"), "HKDF", false, ["deriveBits", "deriveKey"]);
	console.log("hkdf", hex(await subtle.deriveBits({ name: "HKDF", hash: "SHA-256", salt: unhex("000102030405060708090a0b0c"), info: unhex("f0f1f2f3f4f5f6f7f8f9") }, ikm, 42 * 8)));
	console.log("hkdfSync", hex(nc.hkdfSync("sha256", unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"), unhex("000102030405060708090a0b0c"), unhex("f0f1f2f3f4f5f6f7f8f9"), 42)));
	const pkcs8 = (k) => Buffer.concat([unhex("302e020100300506032b656e04220420"), unhex(k)]);
	const alice = await subtle.importKey("pkcs8", pkcs8("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"), { name: "X25519" }, true, ["deriveBits", "deriveKey"]);
	const bobPub = await subtle.importKey("raw", unhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"), { name: "X25519" }, true, []);
	console.log("x25519", hex(await subtle.deriveBits({ name: "X25519", public: bobPub }, alice, 256)));
	console.log("x25519 jwk", JSON.stringify(await subtle.exportKey("jwk", alice)));
	console.log("x25519 spki", hex(await subtle.exportKey("spki", bobPub)), bobPub.type, bobPub.extractable, bobPub.usages.length);
	const shared = await subtle.importKey("raw", await subtle.deriveBits({ name: "X25519", public: bobPub }, alice, 256), "HKDF", false, ["deriveKey"]);
	const gcm = await subtle.deriveKey({ name: "HKDF", hash: "SHA-256", salt: new Uint8Array(32), info: enc("channels") }, shared, { name: "AES-GCM", length: 256 }, true, ["encrypt", "decrypt"]);
	const nonce = new Uint8Array(12).fill(9);
	const sealed = await subtle.encrypt({ name: "AES-GCM", iv: nonce, additionalData: enc("ad") }, gcm, enc("secret message"));
	console.log("hkdf -> gcm", hex(await subtle.exportKey("raw", gcm)), hex(sealed));
	console.log("gcm open", new TextDecoder().decode(await subtle.decrypt({ name: "AES-GCM", iv: nonce, additionalData: enc("ad") }, gcm, sealed)));
	await tryit("gcm tampered", () => subtle.decrypt({ name: "AES-GCM", iv: nonce }, gcm, sealed));
	const pair = await subtle.generateKey({ name: "X25519" }, true, ["deriveBits"]);
	const other = await subtle.generateKey({ name: "X25519" }, true, ["deriveBits"]);
	const s1 = hex(await subtle.deriveBits({ name: "X25519", public: other.publicKey }, pair.privateKey, 256));
	const s2 = hex(await subtle.deriveBits({ name: "X25519", public: pair.publicKey }, other.privateKey, 256));
	console.log("x25519 agree", s1 === s2, pair.privateKey.extractable, pair.publicKey.usages.length, pair.privateKey.usages.join(","));
	const jwkBack = await subtle.importKey("jwk", await subtle.exportKey("jwk", pair.privateKey), { name: "X25519" }, true, ["deriveBits"]);
	console.log("x25519 jwk round trip", hex(await subtle.exportKey("pkcs8", jwkBack)) === hex(await subtle.exportKey("pkcs8", pair.privateKey)));
	const zeroPub = await subtle.importKey("raw", new Uint8Array(32), { name: "X25519" }, true, []);
	await tryit("x25519 small order", () => subtle.deriveBits({ name: "X25519", public: zeroPub }, alice, 256));

	// Ed25519 (RFC 8032 test 2), subtle and node:crypto, JWK and minisign-style verify.
	const edSeed = "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
	const edPub = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
	const edPriv = await subtle.importKey("pkcs8", Buffer.concat([unhex("302e020100300506032b657004220420"), unhex(edSeed)]), "Ed25519", true, ["sign"]);
	const msg = unhex("72");
	const sig = await subtle.sign("Ed25519", edPriv, msg);
	console.log("ed25519 sign", hex(sig));
	const edJwk = await subtle.exportKey("jwk", edPriv);
	console.log("ed25519 jwk", JSON.stringify(edJwk));
	const edPubKey = await subtle.importKey("jwk", { kty: "OKP", crv: "Ed25519", x: edJwk.x }, "Ed25519", true, ["verify"]);
	console.log("ed25519 verify", await subtle.verify("Ed25519", edPubKey, sig, msg), await subtle.verify({ name: "Ed25519" }, edPubKey, sig, unhex("73")));
	console.log("ed25519 raw", hex(await subtle.exportKey("raw", edPubKey)) === edPub);
	const ko = nc.createPublicKey({ key: { kty: "OKP", crv: "Ed25519", x: edJwk.x }, format: "jwk" });
	console.log("createPublicKey", ko.type, ko.asymmetricKeyType, nc.verify(null, msg, ko, Buffer.from(sig)), nc.verify(null, unhex("73"), ko, Buffer.from(sig)));
	console.log("spki pem", ko.export({ format: "pem", type: "spki" }).trim().split("\n").join("|"));
	console.log("spki der", ko.export({ format: "der", type: "spki" }).toString("hex"));
	const kpriv = nc.createPrivateKey({ key: Buffer.concat([unhex("302e020100300506032b657004220420"), unhex(edSeed)]), format: "der", type: "pkcs8" });
	console.log("node sign", nc.sign(null, msg, kpriv).toString("hex") === hex(sig), JSON.stringify(kpriv.export({ format: "jwk" })));
	const pemPriv = kpriv.export({ format: "pem", type: "pkcs8" });
	console.log("pem private", nc.createPrivateKey(pemPriv).equals(kpriv), nc.createPublicKey(pemPriv).equals(ko), nc.createPublicKey(kpriv).export({ format: "jwk" }).x === edJwk.x);
	const gen = nc.generateKeyPairSync("ed25519");
	const gsig = nc.sign(null, enc("hi"), gen.privateKey);
	console.log("generateKeyPairSync", gen.publicKey.asymmetricKeyType, gsig.length, nc.verify(null, enc("hi"), gen.publicKey, gsig));
	const ka = nc.generateKeyPairSync("x25519"), kb = nc.generateKeyPairSync("x25519");
	console.log("diffieHellman", nc.diffieHellman({ privateKey: ka.privateKey, publicKey: kb.publicKey }).equals(nc.diffieHellman({ privateKey: kb.privateKey, publicKey: ka.publicKey })));
	const fromCk = nc.KeyObject.from(edPubKey);
	console.log("KeyObject.from", fromCk.type, fromCk.asymmetricKeyType, fromCk.equals(ko));
	console.log("timingSafeEqual", nc.timingSafeEqual(Buffer.from("ab"), Buffer.from("ab")), nc.timingSafeEqual(Buffer.from("ab"), Buffer.from("ac")));
	const gen2 = await subtle.generateKey("Ed25519", false, ["sign", "verify"]);
	const s3 = await subtle.sign("Ed25519", gen2.privateKey, enc("x"));
	console.log("ed25519 generated", gen2.privateKey.extractable, gen2.privateKey.usages.join(","), gen2.publicKey.usages.join(","), await subtle.verify("Ed25519", gen2.publicKey, s3, enc("x")));

	// Errors callers branch on.
	await tryit("wrong usage", () => subtle.sign("Ed25519", edPubKey, msg));
	await tryit("not extractable", () => subtle.exportKey("raw", gen2.privateKey));
	await tryit("unknown alg", () => subtle.digest("MD5", msg));
	await tryit("bad aes length", () => subtle.importKey("raw", new Uint8Array(20), "AES-GCM", true, ["encrypt"]));
	console.log("instanceof", edPriv instanceof CryptoKey, Object.prototype.toString.call(edPriv), typeof SubtleCrypto);
}
main().catch((e) => { console.log("failed", e); process.exitCode = 1; });
