#!/bin/sh
# Assemble the JSC-host hello-world .app from a Livewire core release (spike, T109).
#   assemble.sh <extracted electrobun-core-darwin-arm64 dir> <out dir>
set -eu
CORE=$1
OUT=$2
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
APP="$OUT/JSC Hello.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/app/views/main"

clang -O2 -Wall -Wextra -framework JavaScriptCore -Wl,-rpath,@executable_path -o "$APP/Contents/MacOS/main" "$ROOT/package/src/jsc-host/host.c"
cp "$CORE/launcher" "$CORE/libElectrobunCore.dylib" "$CORE/libNativeWrapper.dylib" "$CORE/libasar.dylib" "$APP/Contents/MacOS/"
cp "$CORE/preload-full.js" "$CORE/preload-sandboxed.js" "$APP/Contents/Resources/"

# Build time: TS -> one classic script for JSContext (bun is only the bundler here).
bun build "$HERE/src/main.ts" --target=browser --format=iife --minify --outfile "$APP/Contents/Resources/app.js" >/dev/null
cp "$HERE/src/views/main/index.html" "$APP/Contents/Resources/app/views/main/"

cat > "$APP/Contents/Resources/build.json" <<'JSON'
{"mainProcess":"zig","defaultRenderer":"native","availableRenderers":["native"],"buildEnvironment":"dev","runtime":{"exitOnLastWindowClosed":true}}
JSON
cat > "$APP/Contents/Resources/version.json" <<'JSON'
{"version":"0.0.1","hash":"spike","channel":"dev","baseUrl":"","name":"JSC Hello","identifier":"dev.livewire.jsc-hello"}
JSON
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>launcher</string>
<key>CFBundleIdentifier</key><string>dev.livewire.jsc-hello</string>
<key>CFBundleName</key><string>JSC Hello</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.0.1</string>
<key>CFBundleVersion</key><string>0.0.1</string>
</dict></plist>
PLIST
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || true
echo "$APP"
