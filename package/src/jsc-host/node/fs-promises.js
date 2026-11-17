import fs from "./fs.js";
export const { stat, lstat, access, readFile, writeFile, appendFile, mkdir, rm, rmdir, unlink, rename, realpath, chmod, readdir, copyFile, symlink, link, readlink, utimes } = fs.promises;
export default fs.promises;
