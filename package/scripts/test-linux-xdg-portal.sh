#!/usr/bin/env bash
set -euo pipefail

# xdg_portal.h against the mock portal in tests/xdg_portal_test.cpp, on a
# private session bus. Needs g++, pkg-config, GLib/GIO headers and dbus.
PACKAGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_SOURCE="${PACKAGE_DIR}/src/native/linux/tests/xdg_portal_test.cpp"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/electrobun-xdg-portal.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT

# shellcheck disable=SC2046
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror "${TEST_SOURCE}" -o "${BUILD_DIR}/xdg-portal-test" \
	$(pkg-config --cflags --libs gio-2.0)
dbus-run-session -- "${BUILD_DIR}/xdg-portal-test"
