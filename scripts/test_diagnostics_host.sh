#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TEST_BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/deadbolt-diagnostics.XXXXXX")"
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT

"${CXX:-c++}" \
    -std=gnu++17 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$PROJECT_DIR/tests/host_stubs" \
    -I"$PROJECT_DIR/main" \
    "$PROJECT_DIR/main/diagnostics.cpp" \
    "$PROJECT_DIR/tests/diagnostics_host_test.cpp" \
    -o "$TEST_BUILD_DIR/diagnostics_host_test"

"$TEST_BUILD_DIR/diagnostics_host_test"
grep -qx 'CONFIG_APP_RETRIEVE_LEN_ELF_SHA=64' "$PROJECT_DIR/sdkconfig.defaults"
printf 'diagnostics host tests passed\n'
