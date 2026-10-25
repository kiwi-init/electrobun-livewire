#!/usr/bin/env bash
set -euo pipefail

# sni_tray.cpp against the mock StatusNotifierWatcher in
# tests/sni_tray_test.cpp, on a private session bus. Needs g++, pkg-config,
# GLib/GIO headers and dbus.
PACKAGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LINUX_DIR="${PACKAGE_DIR}/src/native/linux"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/electrobun-sni-tray.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT

# shellcheck disable=SC2046
"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror -pthread \
	"${LINUX_DIR}/sni_tray.cpp" "${LINUX_DIR}/tests/sni_tray_test.cpp" -o "${BUILD_DIR}/sni-tray-test" \
	$(pkg-config --cflags --libs gio-2.0)
dbus-run-session -- "${BUILD_DIR}/sni-tray-test"
