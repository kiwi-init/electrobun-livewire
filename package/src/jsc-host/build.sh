#!/bin/sh
# Builds the Livewire JSC runtime: the prelude header, jsc-run (the test
# runner) and the app host. macOS only.
#   build.sh <out dir>
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1
mkdir -p "$OUT/gen"
GEN=$(mktemp -d)
cp "$HERE/prelude.js" "$GEN/jsr_prelude_js"
printf '\0' >> "$GEN/jsr_prelude_js"
(cd "$GEN" && xxd -i jsr_prelude_js) > "$OUT/gen/prelude.h"
rm -rf "$GEN"
CFLAGS="-O2 -Wall -Wextra -Werror -std=gnu11 -I$OUT/gen -I$HERE"
clang $CFLAGS -framework JavaScriptCore -framework CoreFoundation -o "$OUT/jsc-run" "$HERE/runtime.c" "$HERE/jsc-run.c"
