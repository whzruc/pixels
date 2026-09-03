#!/usr/bin/env bash
# Resume the newest (or RUN_STAMP-selected) run_all_comparison.sh invocation.
# Completed phases are skipped from workflow.log; an interrupted suite resumes
# at the first unfinished query/SSD/buffer/thread case.

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_ROOT="${RESULTS_ROOT:-$SCRIPT_DIR/comparison-results}"

if [[ -n "${RUN_STAMP:-}" ]]; then
    run_root="$RESULTS_ROOT/comparison-total-${RUN_STAMP}"
else
    run_root=$(find "$RESULTS_ROOT" -mindepth 1 -maxdepth 1 -type d \
        -name 'comparison-total-*' -printf '%T@ %p\n' 2>/dev/null \
        | sort -nr | awk 'NR == 1 { sub(/^[^ ]+ /, ""); print; exit }')
fi

if [[ -z "${run_root:-}" || ! -d "$run_root" ]]; then
    echo "[error] no comparison run found under $RESULTS_ROOT" >&2
    exit 2
fi

run_name="${run_root##*/}"
RUN_STAMP="${run_name#comparison-total-}"
workflow_log="$run_root/workflow.log"

phase_succeeded() {
    local phase="$1"
    [[ -f "$workflow_log" ]] && grep -Fq "===== phase success: $phase (" "$workflow_log"
}

export RUN_STAMP RUN_ROOT="$run_root" RESUME=1
phase_succeeded "LBA map generation" && export SKIP_LBA_MAP=1
phase_succeeded "SPDK Pixels benchmark" && export SKIP_SPDK=1
phase_succeeded "io_uring and pread Pixels benchmark" && export SKIP_PIXELS=1
phase_succeeded "Parquet benchmark" && export SKIP_PARQUET=1

echo "[resume] run stamp: $RUN_STAMP"
echo "[resume] results:   $RUN_ROOT"
echo "[resume] skips:     LBA=${SKIP_LBA_MAP:-0} SPDK=${SKIP_SPDK:-0} PIXELS=${SKIP_PIXELS:-0} PARQUET=${SKIP_PARQUET:-0}"

exec bash "$SCRIPT_DIR/run_all_comparison.sh" "$@"
