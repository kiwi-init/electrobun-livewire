#!/usr/bin/env bash
set -euo pipefail

# spawn_fallbacks.h against fake xdg-open, gio and notify-send that record
# their argv. Needs g++, pkg-config and GLib headers.
PACKAGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_SOURCE="${PACKAGE_DIR}/src/native/linux/tests/spawn_fallbacks_test.cpp"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/electrobun-spawn-fallbacks.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT

# shellcheck disable=SC2046
"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror "${TEST_SOURCE}" -o "${BUILD_DIR}/spawn-fallbacks-test" \
	$(pkg-config --cflags --libs glib-2.0)
"${BUILD_DIR}/spawn-fallbacks-test"
