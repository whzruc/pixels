#!/usr/bin/env bash

set -euo pipefail

cpp_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
timestamp=$(date +%Y%m%d-%H%M%S)
result_dir=${1:-"$cpp_root/testcase/buffer-pool/results/full-buffer-stats-$timestamp"}
if [[ $# -gt 0 ]]; then
    shift
fi

"$cpp_root/testcase/buffer-pool/run-24ssd-benchmark.sh" \
    "$result_dir" \
    --threads 12 24 48 \
    --modes legacy dynamic static static-hugepage \
    --repeats 3 \
    --buffer-pool-stats \
    --isolated-query-stats \
    --isolated-repeats 1 \
    "$@"

python3 "$cpp_root/testcase/buffer-pool/check-24ssd-completeness.py" "$result_dir"
echo "Complete results: $result_dir"
