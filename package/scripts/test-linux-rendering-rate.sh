#!/usr/bin/env bash
set -euo pipefail

# rendering_rate.h against the installed WebKitGTK: it lists the near-60fps
# feature and a WebKitSettings keeps it off. Needs g++, pkg-config,
# libwebkit2gtk-4.1-dev and a display (WebKitSettings starts GTK): CI runs it
# under run-under-wayland.sh.
PACKAGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_SOURCE="${PACKAGE_DIR}/src/native/linux/tests/rendering_rate_test.cpp"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/electrobun-rendering-rate.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT

# shellcheck disable=SC2046
"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror "${TEST_SOURCE}" -o "${BUILD_DIR}/rendering-rate-test" \
	$(pkg-config --cflags --libs webkit2gtk-4.1)
"${BUILD_DIR}/rendering-rate-test"
