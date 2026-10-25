#!/usr/bin/env bash
# Compare a Kitchen AUTO_RUN log against a list of known failures.
#
#   check-kitchen-results.sh LOG KNOWN_FAILURES
#
# Fails on a test that failed and isn't listed, or when the run never printed
# its summary (a crash). A listed test that passed is reported so the list
# can shrink. Annotations and a step summary go to GitHub when available.
set -euo pipefail

log="$1"
known_file="$2"

results="$(sed $'s/\x1b\\[[0-9;]*m//g' "$log" | awk '
  /Running: / { sub(/.*Running: /, ""); name = $0; next }
  /✓ PASSED/ && name != "" { print "pass\t" name; name = ""; next }
  /✗ FAILED/ && name != "" { print "fail\t" name; name = ""; next }
  /- SKIPPED/ && name != "" { print "skip\t" name; name = ""; next }
')"

if ! sed $'s/\x1b\\[[0-9;]*m//g' "$log" | grep -Eq '[0-9]+ passed'; then
  echo "::error::Kitchen never printed its summary; the app crashed or hung."
  exit 1
fi

known="$(grep -v '^[[:space:]]*\(#\|$\)' "$known_file" || true)"
is_known() { grep -Fxq -- "$1" <<< "$known"; }

new_failures=()
fixed=()
while IFS=$'\t' read -r status name; do
  [[ -z "$name" ]] && continue
  if [[ "$status" == fail ]] && ! is_known "$name"; then
    new_failures+=("$name")
  elif [[ "$status" == pass ]] && is_known "$name"; then
    fixed+=("$name")
  fi
done <<< "$results"

count() { grep -c "^$1"$'\t' <<< "$results" || true; }
summary="$(count pass) passed, $(count fail) failed ($((${#new_failures[@]})) new), $(count skip) skipped"
echo "$summary"
{
  echo "### Kitchen on Wayland"
  echo "$summary"
  for name in ${new_failures[@]+"${new_failures[@]}"}; do echo "- New failure: \`$name\`"; done
  for name in ${fixed[@]+"${fixed[@]}"}; do echo "- Passes now, remove from \`$known_file\`: \`$name\`"; done
} >> "${GITHUB_STEP_SUMMARY:-/dev/null}"

for name in ${fixed[@]+"${fixed[@]}"}; do
  echo "::notice::Passes now, remove it from $known_file: $name"
done
for name in ${new_failures[@]+"${new_failures[@]}"}; do
  echo "::error::New Wayland failure: $name"
done
[[ ${#new_failures[@]} -eq 0 ]]
