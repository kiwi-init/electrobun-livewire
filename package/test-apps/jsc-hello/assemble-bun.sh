#!/bin/sh
# Assemble the Bun baseline of the same hello-world (spike, T110).
#   assemble-bun.sh <extracted electrobun-core-darwin-arm64 dir> <bun binary> <out dir>
set -eu
CORE=$1
BUN_BIN=$2
OUT=$3
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$OUT/Bun Hello.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/app/views/main" "$APP/Contents/Resources/app/bun"

cp "$CORE/launcher" "$CORE/libElectrobunCore.dylib" "$CORE/libNativeWrapper.dylib" "$CORE/libasar.dylib" "$APP/Contents/MacOS/"
cp "$BUN_BIN" "$APP/Contents/MacOS/bun"
cp "$CORE/preload-full.js" "$CORE/preload-sandboxed.js" "$CORE/main.js" "$APP/Contents/Resources/"

# Resolve electrobun/bun to the release's SDK sources.
WORK=$(mktemp -d)
mkdir -p "$WORK/node_modules/electrobun"
ln -s "$CORE/api" "$WORK/node_modules/electrobun/api"
printf '{"name":"electrobun","type":"module","exports":{"./bun":"./api/sdks/main/index.ts"}}' > "$WORK/node_modules/electrobun/package.json"
cp "$HERE/src/bun-baseline.ts" "$WORK/index.ts"
(cd "$WORK" && bun build index.ts --target=bun --minify --outfile "$APP/Contents/Resources/app/bun/index.js" >/dev/null)
rm -rf "$WORK"
cp "$HERE/src/views/main/index.html" "$APP/Contents/Resources/app/views/main/"

cat > "$APP/Contents/Resources/build.json" <<'JSON'
{"mainProcess":"bun","defaultRenderer":"native","availableRenderers":["native"],"buildEnvironment":"dev","runtime":{"exitOnLastWindowClosed":true}}
JSON
cat > "$APP/Contents/Resources/version.json" <<'JSON'
{"version":"0.0.1","hash":"spike","channel":"dev","baseUrl":"","name":"Bun Hello","identifier":"dev.livewire.bun-hello"}
JSON
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>launcher</string>
<key>CFBundleIdentifier</key><string>dev.livewire.bun-hello</string>
<key>CFBundleName</key><string>Bun Hello</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.0.1</string>
<key>CFBundleVersion</key><string>0.0.1</string>
</dict></plist>
PLIST
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || true
echo "$APP"
