#!/bin/sh
# Builds the milestone 1 SDK check app for mainProcess "jsc" from a Livewire
# core release: the JSC host (runtime + Node shims + bun:ffi glue generated
# from that release's SDK), the app's main bundled for it, and the page.
# With --bun <bun binary>, also builds the same app on Bun for comparison.
#   assemble.sh <extracted electrobun-core-darwin-arm64 dir> <out dir> [--bun <bun>]
set -eu
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
bun "$HOST/ffi/gen.ts" "$WORK/gen" --sdk "$CORE/api/sdks/main" --check-lib "$CORE/libElectrobunCore.dylib" --check-lib "$CORE/libNativeWrapper.dylib"

# Bun.build rather than the bun build CLI, so Hutch's Bun runner works too.
bundle_js() { # <entry> <target> <outfile>
  (cd "$WORK" && bun -e 'const [entry, target, outfile] = process.argv.slice(-3);
const r = await Bun.build({ entrypoints: [entry], target, minify: true });
if (!r.success) { console.error(r.logs.join("\n")); process.exit(1); }
await Bun.write(outfile, await r.outputs[0].text());' "$1" "$2" "$3")
}

make_app() { # <app dir> <name> <identifier> <mainProcess>
  rm -rf "$1"
  mkdir -p "$1/Contents/MacOS" "$1/Contents/Resources/app/bun" "$1/Contents/Resources/app/views/main"
  cp "$CORE/launcher" "$CORE/libElectrobunCore.dylib" "$CORE/libNativeWrapper.dylib" "$CORE/libasar.dylib" "$1/Contents/MacOS/"
  cp "$CORE/preload-full.js" "$CORE/preload-sandboxed.js" "$1/Contents/Resources/"
  bundle_js "$WORK/src/views/main/index.ts" browser "$1/Contents/Resources/app/views/main/index.js"
  cp "$HERE/src/views/main/index.html" "$1/Contents/Resources/app/views/main/"
  printf '{"mainProcess":"%s","defaultRenderer":"native","availableRenderers":["native"],"buildEnvironment":"dev","runtime":{"exitOnLastWindowClosed":true}}' "$4" > "$1/Contents/Resources/build.json"
  printf '{"version":"0.0.1","hash":"m1","channel":"dev","baseUrl":"","name":"%s","identifier":"%s"}' "$2" "$3" > "$1/Contents/Resources/version.json"
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

APP="$OUT/JSC SDK.app"
make_app "$APP" "JSC SDK" "dev.livewire.jsc-sdk" "zig"   # the launcher runs MacOS/main for native main processes
clang -O2 -Wall -Wextra -Werror -std=gnu11 -I"$WORK/gen" -I"$HOST" -I"$HOST/ffi" \
  -framework JavaScriptCore -framework CoreFoundation -Wl,-rpath,@executable_path \
  -o "$APP/Contents/MacOS/main" \
  "$HOST/runtime.c" "$HOST/node.c" "$WORK/gen/ffi_glue.c" "$HOST/app-host.c"
(cd "$WORK" && bun "$HOST/bundle.ts" src/bun/index.ts "$APP/Contents/Resources/app/bun/index.js" --minify \
  --alias "electrobun/bun=$CORE/api/sdks/main/index.ts" --preload-dir "$CORE")
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || true
echo "$APP"

if [ -n "$BUN_BIN" ]; then
  BAPP="$OUT/Bun SDK.app"
  make_app "$BAPP" "Bun SDK" "dev.livewire.bun-sdk" "bun"
  cp "$BUN_BIN" "$BAPP/Contents/MacOS/bun"
  cp "$CORE/main.js" "$BAPP/Contents/Resources/"
  bundle_js "$WORK/src/bun/index.ts" bun "$BAPP/Contents/Resources/app/bun/index.js"
  codesign --force --deep -s - "$BAPP" >/dev/null 2>&1 || true
  echo "$BAPP"
fi
