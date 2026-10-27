#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "SKIP: macOS rendering-rate native test requires Darwin"
  exit 0
fi

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_binary="$(mktemp -t electrobun-rendering-rate-test.XXXXXX)"
trap 'rm -f "$test_binary"' EXIT

xcrun clang++ \
  -std=c++20 \
  -fobjc-arc \
  -fblocks \
  -framework Cocoa \
  -framework WebKit \
  "$package_dir/src/native/macos/rendering_rate.test.mm" \
  -o "$test_binary"

"$test_binary"
