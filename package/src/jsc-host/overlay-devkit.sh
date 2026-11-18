#!/bin/sh
# A JSC host for a devkit released without one: builds this checkout's host
# against that devkit's own SDK (its bun:ffi glue) into <out>/api/jsc-host,
# laid out as package/build.ts lays it out, with the postBuild hook, bundler
# and shims beside it. The rest of <out>/api links to the devkit's api, so the
# hook bundles against that SDK and the devkit itself is left as installed (a
# project may pin its tree).
#   overlay-devkit.sh <project>/.hutch/devkit <out> [--check-lib <core lib>]...
# The hook is then <out>/api/jsc-host/post-build.ts. BUN names the Bun that
# runs the generator (default: bun).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
DEVKIT=$(cd "$1" && pwd)
OUT=$2
shift 2
[ -d "$DEVKIT/api/sdks/main" ] || { echo "overlay-devkit: no SDK at $DEVKIT/api/sdks/main" >&2; exit 1; }
[ -f "$DEVKIT/api/shared/native-devkit-manifest.ts" ] || { echo "overlay-devkit: $DEVKIT/api/shared has no native-devkit-manifest.ts" >&2; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT/api/jsc-host/ffi"
OUT=$(cd "$OUT" && pwd)
for entry in "$DEVKIT"/api/*; do
  name=$(basename "$entry")
  [ "$name" = jsc-host ] || ln -s "$entry" "$OUT/api/$name"
done
HOST="$OUT/api/jsc-host"
sh "$HERE/build-host.sh" "$HOST/main" --sdk "$DEVKIT/api/sdks/main" "$@"
cp -R "$HERE/node" "$HOST/node"
cp "$HERE/ffi/bun-ffi.js" "$HOST/ffi/bun-ffi.js"
cp "$HERE/bundle.ts" "$HERE/post-build.ts" "$HOST/"
echo "overlay-devkit: $HOST/post-build.ts runs the JSC host built from $HERE"
