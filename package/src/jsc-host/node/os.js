// node:os on the Livewire JSC runtime.
const info = () => globalThis.__node.osInfo();
export const EOL = "\n";
export const homedir = () => info().homedir;
export const tmpdir = () => info().tmpdir;
export const hostname = () => info().hostname;
export const platform = () => "darwin";
export const type = () => "Darwin";
export const arch = () => "arm64";
export const release = () => info().release;
export const totalmem = () => info().totalmem;
export const cpus = () => Array.from({ length: info().cpus }, () => ({ model: "", speed: 0, times: {} }));
export const availableParallelism = () => info().cpus;
export const userInfo = () => {
	const i = info();
	return { username: i.username, uid: i.uid, gid: i.gid, homedir: i.homedir, shell: globalThis.process?.env?.SHELL ?? null };
};
export const endianness = () => "LE";
export default { EOL, homedir, tmpdir, hostname, platform, type, arch, release, totalmem, cpus, availableParallelism, userInfo, endianness };
