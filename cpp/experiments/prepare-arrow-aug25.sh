#!/usr/bin/env bash
# Apply the two CMake-only Arrow changes present in the validated Aug-25 build.
# Run after: git submodule update --init --recursive cpp/third-party/arrow
set -euo pipefail

CPP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARROW="$CPP_ROOT/third-party/arrow"
PATCH="$CPP_ROOT/experiments/arrow-aug25.patch"
EXPECTED=9105a4109a80a1c01eabb24ee4b9f7c94ee942cb

[[ -f "$ARROW/.git" || -d "$ARROW/.git" ]] || {
    echo "Arrow submodule is absent; initialize it first" >&2
    exit 1
}
[[ "$(git -C "$ARROW" rev-parse HEAD)" == "$EXPECTED" ]] || {
    echo "Arrow HEAD differs from the pinned version $EXPECTED" >&2
    exit 1
}
if git -C "$ARROW" apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "Aug-25 Arrow CMake patch is already applied"
    exit 0
fi
[[ -z "$(git -C "$ARROW" status --porcelain)" ]] || {
    echo 'Arrow has unrelated local changes; refusing to apply patch' >&2
    exit 1
}
git -C "$ARROW" apply --check "$PATCH"
git -C "$ARROW" apply "$PATCH"
echo 'Applied exact Aug-25 Arrow CMake patch'
