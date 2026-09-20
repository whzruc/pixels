#!/usr/bin/env bash

set -euo pipefail

cpp_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
timestamp=$(date +%Y%m%d-%H%M%S)
result_dir=${1:-"$cpp_root/testcase/buffer-pool/results/$timestamp"}
if [[ $# -gt 0 ]]; then
    shift
fi

mkdir -p "$result_dir"
echo "Results: $result_dir"

python3 "$cpp_root/testcase/buffer-pool/run-24ssd-benchmark.py" \
    --root "$result_dir" \
    --cpp-root "$cpp_root" \
    --repo-root "$(cd "$cpp_root/.." && pwd)" \
    "$@" 2>&1 | tee -a "$result_dir/runner.log"

python3 "$cpp_root/testcase/buffer-pool/analyze-24ssd-results.py" "$result_dir"
