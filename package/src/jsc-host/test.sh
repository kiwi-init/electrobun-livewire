#!/bin/sh
# Runs the runtime's tests: each tests/*.test.js must print what its
# "// expect:" lines say, in order, exiting with "// exit:" (default 0).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$(mktemp -d)}
"$HERE/build.sh" "$OUT" || exit 1
fail=0
for t in "$HERE"/tests/*.test.js; do
  expected=$(sed -n 's|^// expect: ||p' "$t")
  want_code=$(sed -n 's|^// exit: ||p' "$t")
  want_code=${want_code:-0}
  actual=$("$OUT/jsc-run" "$t" 2>&1)
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
