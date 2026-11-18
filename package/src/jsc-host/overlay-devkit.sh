#!/bin/sh
# A JSC host for a devkit released without one: builds this checkout's host
# against that devkit's own SDK (its bun:ffi glue) into <out>/api/jsc-host,
# laid out as package/build.ts lays it out, with the postBuild hook, bundler
# and shims beside it. The rest of <out>/api links to the devkit's api, so the
# hook bundles against that SDK and the devkit itself is left as installed (a
# project may pin its tree).
#   overlay-devkit.sh <project>/.hutch/devkit <out> [--launcher] [--check-lib <core lib>]...
# --launcher also builds this checkout's launcher (src/launcher, as a release
# does) into <out>/api/jsc-host/launcher, and the hook puts it in the app in
# place of the devkit's: a devkit released before mainProcess "jsc" (#311) has
# a launcher that starts Bun instead. ZIG names a Zig of the pinned version;
# without it, that version is downloaded from ziglang.org.
# The hook is then <out>/api/jsc-host/post-build.ts. BUN names the Bun that
# runs the generator (default: bun).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
DEVKIT=$(cd "$1" && pwd)
OUT=$2
shift 2
LAUNCHER=
if [ "${1:-}" = --launcher ]; then LAUNCHER=1; shift; fi
[ -d "$DEVKIT/api/sdks/main" ] || { echo "overlay-devkit: no SDK at $DEVKIT/api/sdks/main" >&2; exit 1; }
[ -f "$DEVKIT/api/shared/native-devkit-manifest.ts" ] || { echo "overlay-devkit: $DEVKIT/api/shared has no native-devkit-manifest.ts" >&2; exit 1; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
# <out> is wiped: never the devkit, a folder inside it, one holding it, or /.
case "$OUT/" in "$DEVKIT/"*) echo "overlay-devkit: <out> $OUT is inside the devkit $DEVKIT" >&2; exit 1 ;; esac
case "$DEVKIT/" in "$OUT/"*) echo "overlay-devkit: <out> $OUT holds the devkit $DEVKIT" >&2; exit 1 ;; esac
rm -rf "$OUT/api"
mkdir -p "$OUT/api/jsc-host/ffi"
for entry in "$DEVKIT"/api/*; do
  name=$(basename "$entry")
  [ "$name" = jsc-host ] || ln -s "$entry" "$OUT/api/$name"
done
HOST="$OUT/api/jsc-host"
sh "$HERE/build-host.sh" "$HOST/main" --sdk "$DEVKIT/api/sdks/main" "$@"
cp -R "$HERE/node" "$HOST/node"
cp "$HERE/ffi/bun-ffi.js" "$HOST/ffi/bun-ffi.js"
cp "$HERE/bundle.ts" "$HERE/post-build.ts" "$HOST/"
if [ -n "$LAUNCHER" ]; then
  version=$(sed -n 's/^export const ZIG_VERSION = "\(.*\)";$/\1/p' "$HERE/../shared/build-dependencies.ts")
  case $(uname -m) in arm64 | aarch64) arch=aarch64 ;; *) arch=x86_64 ;; esac
  case $(uname -s) in Darwin) os=macos target=$arch-macos ;; *) os=linux target=$arch-linux-gnu ;; esac
  if [ -z "${ZIG:-}" ]; then
    folder=zig-$arch-$os-$version
    mkdir -p "$OUT/zig"
    [ -x "$OUT/zig/$folder/zig" ] || curl -fsSL --retry 5 "https://ziglang.org/download/$version/$folder.tar.xz" | tar -xJ -C "$OUT/zig"
    ZIG="$OUT/zig/$folder/zig"
  fi
  (cd "$HERE/../launcher" && "$ZIG" build -Doptimize=ReleaseSmall -Dtarget="$target" --prefix "$OUT/launcher-build" --cache-dir "$OUT/zig-cache")
  cp "$OUT/launcher-build/bin/launcher" "$HOST/launcher"
fi
echo "overlay-devkit: $HOST/post-build.ts runs the JSC host built from $HERE${LAUNCHER:+, with its launcher}"
