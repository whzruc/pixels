#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d /tmp/pixels-selective-unit.XXXXXX)
trap 'rm -f "$work/test"; rmdir "$work"' EXIT
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread \
    ${SANITIZER_FLAGS:-} -I"$root/pixels-common/include" \
    "$root/tests/BufferPool/SelectiveBufferSchedulerTest.cpp" -o "$work/test"
"$work/test"
