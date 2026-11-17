// node:process: the global process object.
const p = globalThis.process;
export default p;
export const { argv, env, platform, arch, pid, execPath, versions } = p;
export const cwd = (...a) => p.cwd(...a);
export const exit = (...a) => p.exit(...a);
export const nextTick = (...a) => p.nextTick(...a);
export const on = (...a) => p.on(...a);
export const once = (...a) => p.once(...a);
export const off = (...a) => p.off(...a);
export const emit = (...a) => p.emit(...a);
export const hrtime = p.hrtime;
export const stdout = p.stdout, stderr = p.stderr;
export const kill = (...a) => p.kill(...a);
