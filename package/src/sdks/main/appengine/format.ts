// AppEngine's signed UI channel format, shared by the engine (which verifies)
// and the `livewire ui` CLI (which signs). No filesystem access here.
//
// manifest.json  livewire-manifest/1: flat object of primitives, Ed25519-signed
//                over its canonical bytes (sorted keys, no `signature`).
// *.uipack       uipack/1: one JSON document {format, uiVersion, files}, files
//                base64 by relative path; the manifest's sha256 covers its exact bytes.

import {
	createHash,
	createPrivateKey,
	createPublicKey,
	generateKeyPairSync,
	sign,
	verify,
} from "node:crypto";

export const MANIFEST_FORMAT = "livewire-manifest/1";
export const UIPACK_FORMAT = "uipack/1";

export type UiManifest = {
	format: typeof MANIFEST_FORMAT;
	/** Which trusted key signed it; lets publishers rotate keys. */
	keyId: string;
	/** +1 per publish; the engine refuses anything not newer than what it has. */
	seq: number;
	uiVersion: string;
	/** Absolute, or relative to the manifest's own URL. */
	bundleUrl: string;
	/** Lowercase hex SHA-256 of the bundle's bytes. */
	sha256: string;
	/** Semver range of the engine contract the UI needs. */
	requiresContract: string;
	/** Data schema the UI writes; the engine never goes back below its high-water mark. */
	dataSchema: number;
	/** Milliseconds since the epoch. */
	publishedAt: number;
	/** Optional, milliseconds since the epoch: after this the manifest is refused. */
	expiresAt?: number;
	/** Base64 Ed25519 signature over canonicalManifestBytes(). */
	signature: string;
};

export type UnsignedUiManifest = Omit<UiManifest, "signature">;

/** ui.json in a UI source directory. */
export type UiConfig = {
	uiVersion: string;
	requiresContract: string;
	dataSchema: number;
};

export type Uipack = {
	format: typeof UIPACK_FORMAT;
	uiVersion: string;
	/** Relative path → base64 file contents. */
	files: Record<string, string>;
};

/** Raw 32-byte Ed25519 public keys, base64url, by keyId. */
export type TrustedKeys = Record<string, string>;

export type Result<T> = { ok: true; value: T } | { ok: false; reason: string };

const MANIFEST_KEYS = new Set([
	"format",
	"keyId",
	"seq",
	"uiVersion",
	"bundleUrl",
	"sha256",
	"requiresContract",
	"dataSchema",
	"publishedAt",
	"expiresAt",
	"signature",
]);
const UI_VERSION = /^[A-Za-z0-9._-]{1,64}$/;
const SHA256_HEX = /^[0-9a-f]{64}$/;
const KEY_ID = /^[A-Za-z0-9._-]{1,64}$/;
const PATH_CHARS = /^[A-Za-z0-9._\-/]+$/;
// Names Windows reserves in any directory, with or without an extension.
const WINDOWS_RESERVED = /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\..*)?$/i;

const isNonNegativeInteger = (value: unknown): value is number =>
	typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

/** Problems with a manifest's fields (not its signature), or null if it's well formed. */
export function manifestShapeError(value: unknown): string | null {
	if (!value || typeof value !== "object" || Array.isArray(value)) return "not an object";
	const m = value as Record<string, unknown>;
	for (const key of Object.keys(m)) {
		if (!MANIFEST_KEYS.has(key)) return `unknown field "${key}"`;
	}
	if (m["format"] !== MANIFEST_FORMAT) return `format must be "${MANIFEST_FORMAT}"`;
	if (typeof m["keyId"] !== "string" || !KEY_ID.test(m["keyId"])) return "bad keyId";
	if (!isNonNegativeInteger(m["seq"]) || m["seq"] < 1) return "seq must be an integer >= 1";
	if (typeof m["uiVersion"] !== "string" || !UI_VERSION.test(m["uiVersion"])) return "bad uiVersion";
	if (typeof m["bundleUrl"] !== "string" || m["bundleUrl"] === "") return "bad bundleUrl";
	if (typeof m["sha256"] !== "string" || !SHA256_HEX.test(m["sha256"])) return "sha256 must be 64 lowercase hex chars";
	if (typeof m["requiresContract"] !== "string" || m["requiresContract"] === "") return "bad requiresContract";
	if (!isNonNegativeInteger(m["dataSchema"])) return "dataSchema must be a non-negative integer";
	if (!isNonNegativeInteger(m["publishedAt"])) return "bad publishedAt";
	if (m["expiresAt"] !== undefined && !isNonNegativeInteger(m["expiresAt"])) return "bad expiresAt";
	if (m["signature"] !== undefined && typeof m["signature"] !== "string") return "bad signature";
	return null;
}

/** The exact bytes that get signed: sorted keys, no whitespace, no `signature`. */
export function canonicalManifestBytes(manifest: UnsignedUiManifest | UiManifest): Uint8Array {
	const fields = manifest as Record<string, unknown>;
	const sorted: Record<string, unknown> = {};
	for (const key of Object.keys(fields).sort()) {
		if (key === "signature" || fields[key] === undefined) continue;
		sorted[key] = fields[key];
	}
	return new TextEncoder().encode(JSON.stringify(sorted));
}

export function signManifest(manifest: UnsignedUiManifest, privateKeyPem: string): UiManifest {
	const shapeError = manifestShapeError(manifest);
	if (shapeError) throw new Error(`manifest: ${shapeError}`);
	const signature = sign(null, canonicalManifestBytes(manifest), createPrivateKey(privateKeyPem));
	return { ...manifest, signature: signature.toString("base64") };
}

/**
 * Checks shape, signer, signature and expiry. It does not check seq against
 * what's installed, the bundle hash or the contract: the engine does that.
 */
export function verifyManifest(
	value: unknown,
	trustedKeys: TrustedKeys,
	now = Date.now(),
): Result<UiManifest> {
	const shapeError = manifestShapeError(value);
	if (shapeError) return { ok: false, reason: `manifest: ${shapeError}` };
	const manifest = value as UiManifest;
	if (typeof manifest.signature !== "string" || manifest.signature === "") {
		return { ok: false, reason: "manifest is unsigned" };
	}
	const publicKey = Object.hasOwn(trustedKeys, manifest.keyId) ? trustedKeys[manifest.keyId] : undefined;
	if (!publicKey) return { ok: false, reason: `key "${manifest.keyId}" isn't trusted` };
	let valid = false;
	try {
		const key = createPublicKey({ key: { kty: "OKP", crv: "Ed25519", x: publicKey }, format: "jwk" });
		valid = verify(null, canonicalManifestBytes(manifest), key, new Uint8Array(Buffer.from(manifest.signature, "base64")));
	} catch {
		valid = false;
	}
	if (!valid) return { ok: false, reason: "bad signature" };
	if (manifest.expiresAt !== undefined && now >= manifest.expiresAt) {
		return { ok: false, reason: "manifest has expired" };
	}
	return { ok: true, value: manifest };
}

/** Problems with a bundle-relative path, or null if it's allowed. */
export function uipackPathError(path: string): string | null {
	if (path.length === 0 || path.length >= 200) return "must be 1–199 characters";
	if (!PATH_CHARS.test(path)) return "may only use A–Z a–z 0–9 . _ - /";
	for (const segment of path.split("/")) {
		if (segment === "" || segment === "." || segment === "..") return "has an empty, . or .. segment";
		// Windows strips a trailing dot, so "a." and "a" would be the same file.
		if (segment.endsWith(".")) return "has a segment ending in .";
		if (WINDOWS_RESERVED.test(segment)) return `uses "${segment}", a name Windows reserves`;
	}
	return null;
}

// Paths that differ only in case land on the same file on macOS and Windows.
function caseCollision(paths: Iterable<string>): string | null {
	const seen = new Map<string, string>();
	for (const path of paths) {
		const folded = path.toLowerCase();
		const other = seen.get(folded);
		if (other !== undefined) return `"${other}" and "${path}" differ only in case`;
		seen.set(folded, path);
	}
	return null;
}

/** Builds a uipack from files and returns its exact bytes and their hash. */
export function buildUipack(
	uiVersion: string,
	files: Map<string, Uint8Array>,
): { bytes: Uint8Array; sha256: string; fileCount: number } {
	if (!UI_VERSION.test(uiVersion)) throw new Error(`bad uiVersion "${uiVersion}"`);
	const collision = caseCollision(files.keys());
	if (collision) throw new Error(`files ${collision}`);
	const encoded: Record<string, string> = {};
	for (const path of [...files.keys()].sort()) {
		const error = uipackPathError(path);
		if (error) throw new Error(`file "${path}" ${error}`);
		encoded[path] = Buffer.from(files.get(path)!).toString("base64");
	}
	const pack: Uipack = { format: UIPACK_FORMAT, uiVersion, files: encoded };
	const bytes = new TextEncoder().encode(JSON.stringify(pack));
	return { bytes, sha256: sha256Hex(bytes), fileCount: files.size };
}

/** Verifies a downloaded uipack against the manifest and decodes its files. */
export function readUipack(
	bytes: Uint8Array,
	expected: { sha256: string; uiVersion: string },
): Result<Map<string, Uint8Array>> {
	if (sha256Hex(bytes) !== expected.sha256) return { ok: false, reason: "bundle hash doesn't match the manifest" };
	let pack: Uipack;
	try {
		pack = JSON.parse(new TextDecoder().decode(bytes));
	} catch {
		return { ok: false, reason: "bundle isn't valid JSON" };
	}
	if (pack?.format !== UIPACK_FORMAT) return { ok: false, reason: `bundle format must be "${UIPACK_FORMAT}"` };
	if (pack.uiVersion !== expected.uiVersion) return { ok: false, reason: "bundle uiVersion doesn't match the manifest" };
	if (!pack.files || typeof pack.files !== "object" || Array.isArray(pack.files)) {
		return { ok: false, reason: "bundle has no files" };
	}
	const collision = caseCollision(Object.keys(pack.files));
	if (collision) return { ok: false, reason: `bundle files ${collision}` };
	const files = new Map<string, Uint8Array>();
	for (const [path, data] of Object.entries(pack.files)) {
		const error = uipackPathError(path);
		if (error) return { ok: false, reason: `bundle file "${path}" ${error}` };
		if (typeof data !== "string") return { ok: false, reason: `bundle file "${path}" isn't base64` };
		files.set(path, new Uint8Array(Buffer.from(data, "base64")));
	}
	return { ok: true, value: files };
}

export function uipackFileName(uiVersion: string, sha256: string): string {
	return `${uiVersion}-${sha256.slice(0, 12)}.uipack`;
}

export function sha256Hex(bytes: Uint8Array): string {
	return createHash("sha256").update(bytes).digest("hex");
}

/** keyId for a raw base64url public key: the first 16 hex chars of its SHA-256. */
export function keyIdFor(publicKey: string): string {
	return sha256Hex(new Uint8Array(Buffer.from(publicKey, "base64url"))).slice(0, 16);
}

/** The raw base64url public key that goes with a PKCS#8 PEM private key. */
export function publicKeyFromPrivate(privateKeyPem: string): string {
	const jwk = createPublicKey(createPrivateKey(privateKeyPem)).export({ format: "jwk" });
	if (typeof jwk.x !== "string") throw new Error("not an Ed25519 key");
	return jwk.x;
}

export function generateSigningKey(): { keyId: string; publicKey: string; privateKeyPem: string } {
	const { privateKey } = generateKeyPairSync("ed25519");
	const privateKeyPem = privateKey.export({ type: "pkcs8", format: "pem" }).toString();
	const publicKey = publicKeyFromPrivate(privateKeyPem);
	return { keyId: keyIdFor(publicKey), publicKey, privateKeyPem };
}
