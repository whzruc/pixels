#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
output=${1:-"${repo_root}/testcase/buffer-pool/results/selective-v2-16ssd-full-$(date +%Y%m%d-%H%M%S)"}
properties=${2:-"${repo_root}/pixels-cpp.properties"}
flamegraph_dir=${3:-"${FLAMEGRAPH_DIR:-/home/whz/FlameGraph}"}
resume_args=()
if [[ -d "${output}" ]]; then
    resume_args+=(--resume)
fi

if [[ ! -f "${properties}" ]]; then
    echo "properties file not found: ${properties}" >&2
    exit 2
fi
if [[ ! -x "${repo_root}/build/release/duckdb" ]]; then
    echo "release binary not found: ${repo_root}/build/release/duckdb" >&2
    exit 2
fi
if [[ ! -f "${flamegraph_dir}/flamegraph.pl" || ! -f "${flamegraph_dir}/stackcollapse-perf.pl" ]]; then
    echo "FlameGraph scripts not found under: ${flamegraph_dir}" >&2
    exit 2
fi

exec python3 "${repo_root}/testcase/buffer-pool/run_selective_benchmark.py" \
    --binary "${repo_root}/build/release/duckdb" \
    --properties "${properties}" \
    --column-sizes /home/whz/pixels/clickbench-size-e0.csv \
    --benchmark-json "${repo_root}/testcase/benchmark.json" \
    --benchmark clickbench-pixels-e0-24ssd \
    --ssds 16 \
    --threads 48 \
    --repeat 3 \
    --query-set full \
    --modes \
        legacy \
        dynamic \
        selective-4 \
        selective-base \
        selective-gate \
        selective-gate-cost \
        selective-all \
        static-locked \
        static-lockfree \
    --perf \
    --flamegraph-dir "${flamegraph_dir}" \
    --timeout 900 \
    "${resume_args[@]}" \
    --output "${output}"
