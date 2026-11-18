#!/bin/sh
# Builds the Livewire JSC runtime for its tests: the prelude header, the
# generated bun:ffi glue (SDK declarations plus the test library's), the test
# library, and jsc-run. macOS (JavaScriptCore.framework) or Linux (WebKitGTK's
# libjavascriptcoregtk, GLib, libgcrypt, libsoup and libffi).
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
SOURCES="$HERE/runtime.c $HERE/worker.c $HERE/node.c $HERE/sqlite.c $HERE/vendor/monocypher/monocypher.c $HERE/vendor/monocypher/monocypher-ed25519.c $OUT/gen/ffi_glue.c $HERE/jsc-run.c"
if [ "$(uname -s)" = "Darwin" ]; then
  clang -O2 -dynamiclib -o "$OUT/libjsctest.dylib" "$HERE/tests/ffi/testlib.c"
  # shellcheck disable=SC2086
  clang $CFLAGS -fobjc-arc -framework JavaScriptCore -framework CoreFoundation -framework Foundation -framework Security -lz -lsqlite3 -lffi -o "$OUT/jsc-run" $SOURCES "$HERE/http-darwin.m"
  # The system JavaScriptCore only JITs in a process with this entitlement;
  # without it, everything runs in the interpreter (6-8x slower).
  codesign --force -s - --entitlements "$HERE/jit.entitlements" "$OUT/jsc-run"
else
  CC=${CC:-cc}
  "$CC" -O2 -shared -fPIC -o "$OUT/libjsctest.so" "$HERE/tests/ffi/testlib.c"
  # shellcheck disable=SC2086
  "$CC" $CFLAGS $(pkg-config --cflags javascriptcoregtk-4.1 glib-2.0 sqlite3 libsoup-3.0 libffi) -o "$OUT/jsc-run" $SOURCES "$HERE/http-linux.c" \
    $(pkg-config --libs javascriptcoregtk-4.1 glib-2.0 sqlite3 libsoup-3.0 libffi) $(libgcrypt-config --libs 2>/dev/null || echo -lgcrypt) -lz -ldl -lpthread
fi
