#!/usr/bin/env bun
// livewire ui: sign and publish AppEngine UI bundles to any static host.
//
//   bun cli.ts keygen [--out DIR]
//       New Ed25519 key: DIR/<keyId>.private.pem (0600, keep it secret) and
//       DIR/<keyId>.public.txt. Prints the keyId and public key for the engine.
//
//   bun cli.ts publish SRC --key PRIVATE.pem --out SITE [--previous PATH|URL]
//                      [--base-url URL] [--expires-in 30d]
//       Packs SRC (its files plus ui.json {uiVersion, requiresContract,
//       dataSchema}) into SITE/bundles/<uiVersion>-<sha>.uipack and writes the
//       signed SITE/manifest.json. Upload SITE as is. seq is the later of the
//       previous seq + 1 and the publish time in ms, so a fresh checkout still
//       publishes past what engines have. --previous reads the live manifest
//       (default: SITE/manifest.json) for that and for the dataSchema guard.
//
//   bun cli.ts verify SITE --public-key KEY [--key-id ID]
//       Checks the manifest's signature and the bundle it points to.

import {
	chmodSync,
	existsSync,
	lstatSync,
	mkdirSync,
	readFileSync,
	readdirSync,
	renameSync,
	writeFileSync,
} from "node:fs";
import { join, relative, sep } from "node:path";
import {
	buildUipack,
	generateSigningKey,
	keyIdFor,
	manifestShapeError,
	publicKeyFromPrivate,
	readUipack,
	signManifest,
	uipackFileName,
	verifyManifest,
	MANIFEST_FORMAT,
	type UiConfig,
	type UiManifest,
} from "./format";

export function keygen(outDir: string) {
	const key = generateSigningKey();
	mkdirSync(outDir, { recursive: true });
	const privatePath = join(outDir, `${key.keyId}.private.pem`);
	const publicPath = join(outDir, `${key.keyId}.public.txt`);
	if (existsSync(privatePath)) throw new Error(`${privatePath} already exists`);
	writeFileSync(privatePath, key.privateKeyPem, { mode: 0o600, flag: "wx" });
	chmodSync(privatePath, 0o600);
	writeFileSync(publicPath, `${key.publicKey}\n`);
	return { keyId: key.keyId, publicKey: key.publicKey, privatePath, publicPath };
}

function readUiConfig(srcDir: string): UiConfig {
	const path = join(srcDir, "ui.json");
	if (!existsSync(path)) throw new Error(`${path} is missing: it needs {uiVersion, requiresContract, dataSchema}`);
	const config = JSON.parse(readFileSync(path, "utf8")) as Partial<UiConfig>;
	if (typeof config.uiVersion !== "string") throw new Error("ui.json: uiVersion must be a string");
	if (typeof config.requiresContract !== "string" || !config.requiresContract) {
		throw new Error("ui.json: requiresContract must be a semver range, e.g. \"^1.0.0\"");
	}
	if (!Number.isSafeInteger(config.dataSchema) || (config.dataSchema as number) < 0) {
		throw new Error("ui.json: dataSchema must be a non-negative integer");
	}
	return config as UiConfig;
}

// Every file under srcDir except ui.json and anything inside a dot-named
// segment. Symlinks are refused rather than followed.
function collectFiles(srcDir: string): Map<string, Uint8Array> {
	const files = new Map<string, Uint8Array>();
	const walk = (dir: string) => {
		for (const entry of readdirSync(dir).sort()) {
			if (entry.startsWith(".")) continue;
			const path = join(dir, entry);
			const rel = relative(srcDir, path).split(sep).join("/");
			if (rel === "ui.json") continue;
			const stat = lstatSync(path);
			if (stat.isSymbolicLink()) throw new Error(`${rel} is a symlink; copy the real file in instead`);
			if (stat.isDirectory()) walk(path);
			else if (stat.isFile()) files.set(rel, new Uint8Array(readFileSync(path)));
		}
	};
	walk(srcDir);
	if (files.size === 0) throw new Error(`${srcDir} has no files to publish`);
	return files;
}

export function parseDuration(text: string): number {
	const match = /^(\d+)(m|h|d)$/.exec(text);
	if (!match) throw new Error(`bad duration "${text}": use e.g. 90m, 12h or 30d`);
	const unit = { m: 60_000, h: 3_600_000, d: 86_400_000 }[match[2] as "m" | "h" | "d"];
	return Number(match[1]) * unit;
}

function writeAtomically(path: string, data: string | Uint8Array) {
	const temporary = `${path}.tmp-${process.pid}`;
	writeFileSync(temporary, data);
	renameSync(temporary, path);
}

// The last published manifest: from --previous (a path or an http(s) URL), else
// SITE/manifest.json, else none.
export async function readPreviousManifest(siteDir: string, previous?: string): Promise<UiManifest | null> {
	let text: string | null = null;
	let source = previous ?? join(siteDir, "manifest.json");
	if (previous && /^https?:\/\//i.test(previous)) {
		const response = await fetch(previous);
		if (response.status === 404) return null;
		if (!response.ok) throw new Error(`--previous ${previous}: HTTP ${response.status}`);
		text = await response.text();
	} else if (existsSync(source)) {
		text = readFileSync(source, "utf8");
	} else if (previous) {
		throw new Error(`--previous ${previous} doesn't exist`);
	}
	if (text === null) return null;
	const parsed = JSON.parse(text);
	const shapeError = manifestShapeError(parsed);
	if (shapeError) throw new Error(`previous manifest ${source}: ${shapeError}`);
	return parsed as UiManifest;
}

export async function publish(options: {
	srcDir: string;
	privateKeyPem: string;
	siteDir: string;
	previous?: string;
	baseUrl?: string;
	expiresInMs?: number;
	now?: number;
}) {
	const now = options.now ?? Date.now();
	const config = readUiConfig(options.srcDir);
	const pack = buildUipack(config.uiVersion, collectFiles(options.srcDir));
	const bundleName = uipackFileName(config.uiVersion, pack.sha256);

	const manifestPath = join(options.siteDir, "manifest.json");
	const previous = await readPreviousManifest(options.siteDir, options.previous);
	// Installed engines never move below a data schema they've seen.
	if (previous && config.dataSchema < previous.dataSchema) {
		throw new Error(`dataSchema ${config.dataSchema} is below the published ${previous.dataSchema}; engines would refuse it`);
	}

	const bundlesDir = join(options.siteDir, "bundles");
	mkdirSync(bundlesDir, { recursive: true });
	const bundlePath = join(bundlesDir, bundleName);
	// Content-addressed: an existing file with this name already has these bytes.
	if (!existsSync(bundlePath)) writeAtomically(bundlePath, pack.bytes);

	const publicKey = publicKeyFromPrivate(options.privateKeyPem);
	const bundleUrl = options.baseUrl
		? `${options.baseUrl.replace(/\/+$/, "")}/bundles/${bundleName}`
		: `bundles/${bundleName}`;
	const manifest = signManifest(
		{
			format: MANIFEST_FORMAT,
			keyId: keyIdFor(publicKey),
			// Engines refuse a seq they've already passed. The publish time keeps it
			// rising even from a checkout with no record of the last publish.
			seq: Math.max((previous?.seq ?? 0) + 1, now),
			uiVersion: config.uiVersion,
			bundleUrl,
			sha256: pack.sha256,
			requiresContract: config.requiresContract,
			dataSchema: config.dataSchema,
			publishedAt: now,
			...(options.expiresInMs ? { expiresAt: now + options.expiresInMs } : {}),
		},
		options.privateKeyPem,
	);
	writeAtomically(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
	return { manifest, manifestPath, bundlePath, fileCount: pack.fileCount, bytes: pack.bytes.length };
}

export function verifySite(options: { siteDir: string; publicKey: string; keyId?: string; now?: number }) {
	const manifestPath = join(options.siteDir, "manifest.json");
	const keyId = options.keyId ?? keyIdFor(options.publicKey);
	const result = verifyManifest(JSON.parse(readFileSync(manifestPath, "utf8")), { [keyId]: options.publicKey }, options.now);
	if (!result.ok) throw new Error(result.reason);
	const manifest = result.value;
	if (/^[a-z]+:/i.test(manifest.bundleUrl)) {
		return { manifest, bundle: "remote (not checked)" as const };
	}
	const files = readUipack(new Uint8Array(readFileSync(join(options.siteDir, manifest.bundleUrl))), manifest);
	if (!files.ok) throw new Error(files.reason);
	return { manifest, bundle: { files: files.value.size } };
}

function flag(args: string[], name: string): string | undefined {
	const index = args.indexOf(name);
	if (index === -1) return undefined;
	const value = args[index + 1];
	if (value === undefined || value.startsWith("--")) throw new Error(`${name} needs a value`);
	args.splice(index, 2);
	return value;
}

async function main(argv: string[]) {
	const args = [...argv];
	const command = args.shift();
	switch (command) {
		case "keygen": {
			const key = keygen(flag(args, "--out") ?? ".");
			console.log(`keyId      ${key.keyId}\npublicKey  ${key.publicKey}\nprivate    ${key.privatePath} (0600, keep it out of git)`);
			return;
		}
		case "publish": {
			const keyPath = flag(args, "--key");
			const siteDir = flag(args, "--out");
			const previous = flag(args, "--previous");
			const baseUrl = flag(args, "--base-url");
			const expiresIn = flag(args, "--expires-in");
			const [srcDir] = args;
			if (!srcDir || !keyPath || !siteDir) throw new Error("usage: publish SRC --key PRIVATE.pem --out SITE [--previous PATH|URL] [--base-url URL] [--expires-in 30d]");
			const result = await publish({
				srcDir,
				privateKeyPem: readFileSync(keyPath, "utf8"),
				siteDir,
				previous,
				baseUrl,
				expiresInMs: expiresIn ? parseDuration(expiresIn) : undefined,
			});
			console.log(`published ${result.manifest.uiVersion} as seq ${result.manifest.seq}: ${result.fileCount} files, ${result.bytes} bytes\n  ${result.bundlePath}\n  ${result.manifestPath}`);
			return;
		}
		case "verify": {
			const publicKey = flag(args, "--public-key");
			const keyId = flag(args, "--key-id");
			const [siteDir] = args;
			if (!siteDir || !publicKey) throw new Error("usage: verify SITE --public-key KEY [--key-id ID]");
			const result = verifySite({ siteDir, publicKey, keyId });
			console.log(`ok: ${result.manifest.uiVersion} seq ${result.manifest.seq}, bundle ${typeof result.bundle === "string" ? result.bundle : `${result.bundle.files} files`}`);
			return;
		}
		default:
			console.error("usage: cli.ts keygen [--out DIR] | publish SRC --key PRIVATE.pem --out SITE [--previous PATH|URL] [--base-url URL] [--expires-in 30d] | verify SITE --public-key KEY [--key-id ID]");
			process.exitCode = 2;
	}
}

if (import.meta.main) {
	main(process.argv.slice(2)).catch((error) => {
		console.error(error instanceof Error ? error.message : String(error));
		process.exitCode = 1;
	});
}
