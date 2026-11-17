// node:path on the Livewire JSC runtime: Bun's browser polyfill (POSIX), plus
// the names it lacks. The runtime is macOS only, so win32 is only there for
// code that names it on paths it never takes.
import * as base from "path";
const posix = { ...(base.default ?? base) };
posix.posix = posix;
export const win32 = { ...posix, sep: "\\", delimiter: ";" };
posix.win32 = win32;
export const { resolve, normalize, isAbsolute, join, relative, dirname, basename, extname, format, parse, sep, delimiter, toNamespacedPath } = posix;
export { posix };
export default posix;
