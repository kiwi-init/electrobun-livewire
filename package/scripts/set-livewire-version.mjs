#!/usr/bin/env node
// Stamps a Livewire release version everywhere the product version lives
// (package, lockfile, Kitchen, npm bootstrap, Rust SDK), for the PR that
// precedes a release candidate. No git operations: commit and open a PR.
//
//   node package/scripts/set-livewire-version.mjs 2.1.0-livewire.rc.1
//
// Livewire versions carry a `livewire` prerelease identifier so they can never
// collide with an upstream Electrobun version in Hutch's per-version cache.

import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { parseStrictSemVer } from "../src/shared/strict-semver.js";
import {
	createRustSdkVersionUpdates,
	parseRepositoryPragmaPins,
	stampNpmBootstrapPairedVersions,
	updateKitchenVersions,
	updateNpmBootstrapVersion,
} from "./version-config.mjs";

/** True for a strict SemVer whose prerelease identifiers include "livewire". */
export function isLivewireVersion(version) {
	const parsed = parseStrictSemVer(version);
	return Boolean(parsed?.prerelease?.split(".").includes("livewire"));
}

export function setLivewireVersion(version, repoRoot) {
	if (!isLivewireVersion(version)) {
		throw new Error(
			`${JSON.stringify(version)} isn't a Livewire version: use SemVer with a "livewire" prerelease identifier, e.g. 2.1.0-livewire.rc.1`,
		);
	}
	const packageDir = join(repoRoot, "package");
	execFileSync("npm", ["version", version, "--no-git-tag-version", "--allow-same-version"], {
		cwd: packageDir,
		stdio: "ignore",
	});

	const kitchenConfigPath = join(repoRoot, "kitchen", "electrobun.config.ts");
	const kitchenHutchConfigPath = join(repoRoot, "kitchen", "hutch.config.ts");
	const kitchen = updateKitchenVersions(
		readFileSync(kitchenHutchConfigPath, "utf8"),
		readFileSync(kitchenConfigPath, "utf8"),
		version,
	);
	const npmBootstrapPath = join(repoRoot, "npm", "electrobun", "package.json");
	const npmResolverPath = join(repoRoot, "npm", "electrobun", "bin", "resolve-hutch.cjs");
	const npmBootstrap = updateNpmBootstrapVersion(readFileSync(npmBootstrapPath, "utf8"), version);
	const npmResolver = stampNpmBootstrapPairedVersions(
		readFileSync(npmResolverPath, "utf8"),
		parseRepositoryPragmaPins(readFileSync(join(packageDir, "hutch.config.ts"), "utf8")),
	);
	const rustSdk = createRustSdkVersionUpdates(repoRoot, version);

	writeFileSync(kitchenHutchConfigPath, kitchen.hutchConfig);
	writeFileSync(kitchenConfigPath, kitchen.electrobunConfig);
	writeFileSync(npmBootstrapPath, npmBootstrap);
	writeFileSync(npmResolverPath, npmResolver);
	for (const update of rustSdk) writeFileSync(update.path, update.source);
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
	try {
		const version = process.argv[2];
		setLivewireVersion(version, join(dirname(fileURLToPath(import.meta.url)), "..", ".."));
		console.log(`Set the Livewire version to ${version}. Commit it, open a PR, and dispatch livewire-rc.yml once it's merged.`);
	} catch (error) {
		console.error(error instanceof Error ? error.message : String(error));
		process.exitCode = 1;
	}
}
