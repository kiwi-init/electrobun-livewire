#!/bin/sh
# Builds the JSC host an app runs as its main process (MacOS/main): the C
# runtime with its prelude, the Node natives, and bun:ffi over the system
# libffi, after checking an SDK's bun:ffi declarations against the types it
# binds and the core libraries it will load (ffi/check.ts). macOS
# (JavaScriptCore.framework, Foundation for NSURLSession) or Linux (WebKitGTK's
# libjavascriptcoregtk, GLib, libgcrypt, libsoup and libffi).
#   build-host.sh <out file> --sdk <sdks/main dir> [--check-lib <core lib>]...
# BUN names the Bun that runs the check (default: bun).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1
shift
GEN=$(mktemp -d)
trap 'rm -rf "$GEN"' EXIT
cp "$HERE/prelude.js" "$GEN/jsr_prelude_js"
printf '\0' >> "$GEN/jsr_prelude_js"
(cd "$GEN" && xxd -i jsr_prelude_js) > "$GEN/prelude.h"
"${BUN:-bun}" "$HERE/ffi/check.ts" "$@"
mkdir -p "$(dirname "$OUT")"
CFLAGS="-O2 -Wall -Wextra -Werror -std=gnu11 -I$GEN -I$HERE -I$HERE/ffi"
SOURCES="$HERE/runtime.c $HERE/worker.c $HERE/node.c $HERE/sqlite.c $HERE/vendor/monocypher/monocypher.c $HERE/vendor/monocypher/monocypher-ed25519.c $HERE/ffi/natives.c $HERE/app-host.c"
if [ "$(uname -s)" = "Darwin" ]; then
  # shellcheck disable=SC2086
  clang $CFLAGS -fobjc-arc -framework JavaScriptCore -framework CoreFoundation -framework Foundation -framework Security -lz -lsqlite3 -lffi -Wl,-rpath,@executable_path \
    -o "$OUT" $SOURCES "$HERE/http-darwin.m"
else
  # shellcheck disable=SC2086
  "${CC:-cc}" $CFLAGS $(pkg-config --cflags javascriptcoregtk-4.1 glib-2.0 sqlite3 libsoup-3.0 libffi) -Wl,-rpath,'$ORIGIN' -o "$OUT" $SOURCES "$HERE/http-linux.c" \
    $(pkg-config --libs javascriptcoregtk-4.1 glib-2.0 sqlite3 libsoup-3.0 libffi) $(libgcrypt-config --libs 2>/dev/null || echo -lgcrypt) -lz -ldl -lpthread
fi
