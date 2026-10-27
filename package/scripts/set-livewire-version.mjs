#!/usr/bin/env node
// Stamps a Livewire release version everywhere the product version lives
// (package, lockfile, Kitchen, npm bootstrap, Rust SDK), for the PR that
// precedes a release. No git operations: commit and open a PR.
//
//   node package/scripts/set-livewire-version.mjs 2026.10.0
//   node package/scripts/set-livewire-version.mjs --next
//
// Livewire is versioned like Channels: CalVer YYYY.M.PATCH, with PATCH back at
// 0 each new month. `--next` stamps the version after package.json's for
// today's UTC month. A year as the major can never collide with an upstream
// Electrobun version in Hutch's per-version cache.

import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import {
	createRustSdkVersionUpdates,
	parseRepositoryPragmaPins,
	stampNpmBootstrapPairedVersions,
	updateKitchenVersions,
	updateNpmBootstrapVersion,
} from "./version-config.mjs";

const LIVEWIRE_VERSION = /^20\d\d\.(?:[1-9]|1[0-2])\.(?:0|[1-9]\d*)$/;

/** True for a Livewire release version: CalVer YYYY.M.PATCH, like 2026.10.0. */
export function isLivewireVersion(version) {
	return typeof version === "string" && LIVEWIRE_VERSION.test(version);
}

/** The release after `current` in `now`'s UTC month: PATCH + 1 within a month, else M.0. */
export function nextLivewireVersion(current, now) {
	const month = `${now.getUTCFullYear()}.${now.getUTCMonth() + 1}`;
	if (!isLivewireVersion(current)) return `${month}.0`;
	const [year, mon, patch] = current.split(".");
	return `${year}.${mon}` === month ? `${month}.${Number(patch) + 1}` : `${month}.0`;
}

export function setLivewireVersion(version, repoRoot) {
	if (!isLivewireVersion(version)) {
		throw new Error(
			`${JSON.stringify(version)} isn't a Livewire version: use CalVer YYYY.M.PATCH, e.g. 2026.10.0`,
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
		const repoRoot = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
		const version =
			process.argv[2] === "--next"
				? nextLivewireVersion(
						JSON.parse(readFileSync(join(repoRoot, "package", "package.json"), "utf8")).version,
						new Date(),
					)
				: process.argv[2];
		setLivewireVersion(version, repoRoot);
		console.log(`Set the Livewire version to ${version}. Commit it, open a PR, and dispatch livewire-release.yml on main once it's merged.`);
	} catch (error) {
		console.error(error instanceof Error ? error.message : String(error));
		process.exitCode = 1;
	}
}
