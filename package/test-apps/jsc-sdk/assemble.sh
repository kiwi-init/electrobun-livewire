#!/bin/sh
# Builds the milestone 1 SDK check app for mainProcess "jsc" from a Livewire
# core release: the JSC host (runtime + Node shims + bun:ffi glue generated
# from that release's SDK), the app's main bundled for it, and the page.
# With --bun <bun binary>, also builds the same app on Bun for comparison.
# macOS builds .app bundles; Linux builds app folders (bin/ + Resources/) that
# run on the system libjavascriptcoregtk.
#   assemble.sh <extracted electrobun-core-<os>-<arch> dir> <out dir> [--bun <bun>]
set -eu
OS=$(uname -s)
if [ "$OS" = "Darwin" ]; then LIB_EXT=dylib; else LIB_EXT=so; fi
CORE=$(cd "$1" && pwd)
OUT=$2
BUN_BIN=""
[ "${3:-}" = "--bun" ] && BUN_BIN=$4
HERE=$(cd "$(dirname "$0")" && pwd)
HOST=$(cd "$HERE/../../src/jsc-host" && pwd)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK="$OUT/work"
rm -rf "$WORK"
mkdir -p "$WORK/gen" "$WORK/node_modules/electrobun"

# electrobun/bun and electrobun/view from the release's SDK sources.
ln -s "$CORE/api" "$WORK/node_modules/electrobun/api"
printf '{"name":"electrobun","type":"module","exports":{"./bun":"./api/sdks/main/index.ts","./view":"./api/browser/index.ts"}}' > "$WORK/node_modules/electrobun/package.json"
cp -R "$HERE/src" "$WORK/src"

# Runtime: prelude, ffi glue from this release's SDK (checked against its core), host.
TMPP=$(mktemp -d)
cp "$HOST/prelude.js" "$TMPP/jsr_prelude_js"
printf '\0' >> "$TMPP/jsr_prelude_js"
(cd "$TMPP" && xxd -i jsr_prelude_js) > "$WORK/gen/prelude.h"
rm -rf "$TMPP"
bun "$HOST/ffi/gen.ts" "$WORK/gen" --sdk "$CORE/api/sdks/main" --check-lib "$CORE/libElectrobunCore.$LIB_EXT" --check-lib "$CORE/libNativeWrapper.$LIB_EXT"

# Where an app's binaries and Resources live.
bin_dir() { if [ "$OS" = "Darwin" ]; then echo "$1/Contents/MacOS"; else echo "$1/bin"; fi; }
res_dir() { if [ "$OS" = "Darwin" ]; then echo "$1/Contents/Resources"; else echo "$1/Resources"; fi; }

# Bun.build rather than the bun build CLI, so Hutch's Bun runner works too.
bundle_js() { # <entry> <target> <outfile>
  (cd "$WORK" && bun -e 'const [entry, target, outfile] = process.argv.slice(-3);
const r = await Bun.build({ entrypoints: [entry], target, minify: true });
if (!r.success) { console.error(r.logs.join("\n")); process.exit(1); }
await Bun.write(outfile, await r.outputs[0].text());' "$1" "$2" "$3")
}

make_app() { # <app dir> <name> <identifier> <mainProcess>
  rm -rf "$1"
  BIN=$(bin_dir "$1"); RES=$(res_dir "$1")
  mkdir -p "$BIN" "$RES/app/bun" "$RES/app/views/main"
  cp "$CORE/launcher" "$CORE/libElectrobunCore.$LIB_EXT" "$CORE/libNativeWrapper.$LIB_EXT" "$CORE/libasar.$LIB_EXT" "$BIN/"
  cp "$CORE/preload-full.js" "$CORE/preload-sandboxed.js" "$RES/"
  bundle_js "$WORK/src/views/main/index.ts" browser "$RES/app/views/main/index.js"
  cp "$HERE/src/views/main/index.html" "$RES/app/views/main/"
  printf '{"mainProcess":"%s","defaultRenderer":"native","availableRenderers":["native"],"buildEnvironment":"dev","runtime":{"exitOnLastWindowClosed":true}}' "$4" > "$RES/build.json"
  printf '{"version":"0.0.1","hash":"m1","channel":"dev","baseUrl":"","name":"%s","identifier":"%s"}' "$2" "$3" > "$RES/version.json"
  [ "$OS" = "Darwin" ] || return 0
  cat > "$1/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>launcher</string>
<key>CFBundleIdentifier</key><string>$3</string>
<key>CFBundleName</key><string>$2</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.0.1</string>
<key>CFBundleVersion</key><string>0.0.1</string>
</dict></plist>
PLIST
}

if [ "$OS" = "Darwin" ]; then APP="$OUT/JSC SDK.app"; else APP="$OUT/JSC SDK"; fi
make_app "$APP" "JSC SDK" "dev.livewire.jsc-sdk" "zig"   # the launcher runs <bin>/main for native main processes
HOST_SOURCES="$HOST/runtime.c $HOST/node.c $WORK/gen/ffi_glue.c $HOST/app-host.c"
HOST_CFLAGS="-O2 -Wall -Wextra -Werror -std=gnu11 -I$WORK/gen -I$HOST -I$HOST/ffi"
if [ "$OS" = "Darwin" ]; then
  # shellcheck disable=SC2086
  clang $HOST_CFLAGS -framework JavaScriptCore -framework CoreFoundation -Wl,-rpath,@executable_path \
    -o "$(bin_dir "$APP")/main" $HOST_SOURCES
else
  # shellcheck disable=SC2086
  ${CC:-cc} $HOST_CFLAGS $(pkg-config --cflags javascriptcoregtk-4.1 glib-2.0) -Wl,-rpath,'$ORIGIN' \
    -o "$(bin_dir "$APP")/main" $HOST_SOURCES \
    $(pkg-config --libs javascriptcoregtk-4.1 glib-2.0) $(libgcrypt-config --libs 2>/dev/null || echo -lgcrypt) -ldl -lpthread
fi
(cd "$WORK" && bun "$HOST/bundle.ts" src/bun/index.ts "$(res_dir "$APP")/app/bun/index.js" --minify \
  --alias "electrobun/bun=$CORE/api/sdks/main/index.ts" --preload-dir "$CORE")
if [ "$OS" = "Darwin" ]; then
  # The JSC host gets allow-jit (the system JavaScriptCore only JITs with it),
  # then the bundle is sealed around it without re-signing nested code.
  codesign --force --deep -s - "$APP" >/dev/null 2>&1 || true
  codesign --force -s - --entitlements "$HOST/jit.entitlements" "$(bin_dir "$APP")/main"
  codesign --force -s - "$APP" >/dev/null 2>&1 || true
fi
echo "$APP"

if [ -n "$BUN_BIN" ]; then
  if [ "$OS" = "Darwin" ]; then BAPP="$OUT/Bun SDK.app"; else BAPP="$OUT/Bun SDK"; fi
  make_app "$BAPP" "Bun SDK" "dev.livewire.bun-sdk" "bun"
  cp "$BUN_BIN" "$(bin_dir "$BAPP")/bun"
  cp "$CORE/main.js" "$(res_dir "$BAPP")/"
  bundle_js "$WORK/src/bun/index.ts" bun "$(res_dir "$BAPP")/app/bun/index.js"
  if [ "$OS" = "Darwin" ]; then codesign --force --deep -s - "$BAPP" >/dev/null 2>&1 || true; fi
  echo "$BAPP"
fi
