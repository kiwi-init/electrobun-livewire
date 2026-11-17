#!/bin/sh
# Builds the Livewire JSC runtime for its tests: the prelude header, the
# generated bun:ffi glue (SDK declarations plus the test library's), the test
# library, and jsc-run. macOS only.
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
bun "$HERE/ffi/gen.ts" "$OUT/gen" --extra "$HERE/tests/ffi/decls.json"
CFLAGS="-O2 -Wall -Wextra -Werror -std=gnu11 -I$OUT/gen -I$HERE -I$HERE/ffi"
clang -O2 -dynamiclib -o "$OUT/libjsctest.dylib" "$HERE/tests/ffi/testlib.c"
clang $CFLAGS -framework JavaScriptCore -framework CoreFoundation -o "$OUT/jsc-run" \
  "$HERE/runtime.c" "$HERE/node.c" "$OUT/gen/ffi_glue.c" "$HERE/jsc-run.c"
