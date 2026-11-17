// node:child_process lands with milestone 2 on the JSC runtime.
const missing = (name) => () => {
	throw new Error(`child_process.${name} isn't available on the JSC runtime yet`);
};
export const spawn = missing("spawn");
export const spawnSync = missing("spawnSync");
export const exec = missing("exec");
export const execSync = missing("execSync");
export const execFile = missing("execFile");
export const execFileSync = missing("execFileSync");
export const fork = missing("fork");
export default { spawn, spawnSync, exec, execSync, execFile, execFileSync, fork };
