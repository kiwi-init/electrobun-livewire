#!/bin/sh
# Runs the runtime's tests: each tests/**/*.test.js (a classic script) or
# *.test.mjs (bundled first, as an app would be) must print what its
# "// expect:" lines say, in order, exiting with "// exit:" (default 0).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$(mktemp -d)}
"$HERE/build.sh" "$OUT" || exit 1
# Not JSC_*: JavaScriptCore reads JSC_-prefixed variables as engine options.
case $(uname -s) in Darwin) LIB_EXT=dylib ;; *) LIB_EXT=so ;; esac
export LIVEWIRE_TESTLIB="$OUT/libjsctest.$LIB_EXT"
mkdir -p "$OUT/bundled"
fail=0
for t in "$HERE"/tests/*.test.js "$HERE"/tests/*/*.test.mjs; do
  [ -e "$t" ] || continue
  run=$t
  case $t in
    *.mjs)
      run="$OUT/bundled/$(basename "$t" .mjs).js"
      bun "$HERE/bundle.ts" "$t" "$run" || { echo "FAIL $(basename "$t") (bundle)"; fail=1; continue; } ;;
  esac
  expected=$(sed -n 's|^// expect: ||p' "$t")
  want_code=$(sed -n 's|^// exit: ||p' "$t")
  want_code=${want_code:-0}
  # A 10 s limit per test, so a loop that never goes idle fails instead of hanging.
  actual=$(perl -e 'alarm 10; exec @ARGV' "$OUT/jsc-run" "$run" 2>&1)
  code=$?
  if [ $code -ne "$want_code" ] || [ "$actual" != "$expected" ]; then
    echo "FAIL $(basename "$t") (exit $code)"
    echo "--- expected"; echo "$expected"; echo "--- actual"; echo "$actual"
    fail=1
  else
    echo "ok   $(basename "$t")"
  fi
done
exit $fail
