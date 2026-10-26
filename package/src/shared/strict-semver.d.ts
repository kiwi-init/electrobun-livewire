// Types for strict-semver.js, so apps that typecheck the devkit's sources
// (AppEngine's contract.ts imports it) don't need allowJs.
export type StrictSemVer = { major: string; minor: string; patch: string; prerelease: string | null; build: string | null };
export function parseStrictSemVer(value: unknown): StrictSemVer | null;
export function isStrictSemVer(value: unknown): boolean;
export function assertStrictSemVer(value: unknown, label?: string): string;
export function isSemVerPrerelease(value: unknown): boolean;
export function isExactOdinRelease(value: unknown): boolean;
export function assertExactOdinRelease(value: unknown, label?: string): string;
