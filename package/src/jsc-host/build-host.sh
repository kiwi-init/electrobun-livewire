#!/bin/sh
# Builds the JSC host an app runs as its main process (MacOS/main): the C
# runtime with its prelude, the Node natives, and bun:ffi glue generated from
# an SDK and checked against the core libraries it will load. macOS for now;
# Linux follows #301.
#   build-host.sh <out file> --sdk <sdks/main dir> [--check-lib <core lib>]...
# BUN names the Bun that runs the generator (default: bun).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1
shift
GEN=$(mktemp -d)
trap 'rm -rf "$GEN"' EXIT
cp "$HERE/prelude.js" "$GEN/jsr_prelude_js"
printf '\0' >> "$GEN/jsr_prelude_js"
(cd "$GEN" && xxd -i jsr_prelude_js) > "$GEN/prelude.h"
"${BUN:-bun}" "$HERE/ffi/gen.ts" "$GEN" "$@"
mkdir -p "$(dirname "$OUT")"
clang -O2 -Wall -Wextra -Werror -std=gnu11 -I"$GEN" -I"$HERE" -I"$HERE/ffi" \
  -framework JavaScriptCore -framework CoreFoundation -Wl,-rpath,@executable_path \
  -o "$OUT" "$HERE/runtime.c" "$HERE/node.c" "$GEN/ffi_glue.c" "$HERE/app-host.c"
