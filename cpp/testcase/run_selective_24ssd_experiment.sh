#!/usr/bin/env bash
# 24 SSD × 44 ClickBench queries × seven buffer modes × three repetitions.
# Usage: ./testcase/run_selective_24ssd_experiment.sh check
#        sudo -E ./testcase/run_selective_24ssd_experiment.sh run
# Set RUN_DIR to resume an interrupted run; PROFILE=0 / OFF_CPU=0 disable profiling.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ACTION="${1:-check}"
[[ "$ACTION" == check || "$ACTION" == run ]] || { echo "usage: $0 [check|run]" >&2; exit 2; }

DUCKDB_BINARY="${DUCKDB_BINARY:-/tmp/pixels-aug25-rebuild/cpp/build/aug25-rebuild/duckdb}"
BASE_PROPS="${BASE_PROPS:-$CPP_ROOT/experiments/aug25-reproduction.properties}"
COLUMN_SIZES="${COLUMN_SIZES:-/home/whz/pixels/clickbench-size-e0.csv}"
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-/home/whz/FlameGraph}"
RESULT_ROOT="${RESULT_ROOT:-$CPP_ROOT/testcase/experiment-results}"
RUN_DIR="${RUN_DIR:-$RESULT_ROOT/selective-24ssd-$(date +%Y%m%d-%H%M%S)}"
PROFILE="${PROFILE:-1}"
OFF_CPU="${OFF_CPU:-1}"
REPEAT="${REPEAT:-3}"
TIMEOUT="${TIMEOUT:-1800}"

for file in "$DUCKDB_BINARY" "$BASE_PROPS" "$COLUMN_SIZES" "$CPP_ROOT/testcase/benchmark.json"; do
    [[ -f "$file" ]] || { echo "missing: $file" >&2; exit 1; }
done
[[ -x "$DUCKDB_BINARY" ]] || { echo "not executable: $DUCKDB_BINARY" >&2; exit 1; }
[[ "$PROFILE" =~ ^[01]$ && "$OFF_CPU" =~ ^[01]$ ]] || { echo "PROFILE and OFF_CPU must be 0 or 1" >&2; exit 2; }
[[ "$REPEAT" =~ ^[1-9][0-9]*$ && "$TIMEOUT" =~ ^[1-9][0-9]*$ ]] || { echo "REPEAT and TIMEOUT must be positive integers" >&2; exit 2; }
if [[ "$PROFILE" == 1 || "$OFF_CPU" == 1 ]]; then
    for file in "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" "$FLAMEGRAPH_DIR/flamegraph.pl"; do
        [[ -f "$file" ]] || { echo "missing: $file" >&2; exit 1; }
    done
fi

# Never silently benchmark fewer than 24 complete SSD paths.
python3 - "$CPP_ROOT/testcase/benchmark.json" <<'PY'
import glob, json, re, sys
benchmark = json.load(open(sys.argv[1]))['clickbench-pixels-e0-24ssd']
patterns = re.findall(r'"(/[^"]+)"', benchmark)
counts = [len(glob.glob(pattern)) for pattern in patterns]
if len(patterns) != 24 or any(count != 640 for count in counts):
    sys.exit(f'expected 24 × 640 Pixels files, found {len(patterns)} paths: {counts}')
print(f'input: {len(patterns)} SSDs, {sum(counts)} files')
PY

echo "binary: $DUCKDB_BINARY"
echo "output: $RUN_DIR"
echo "matrix: 44 queries × 7 modes × $REPEAT repetitions; perf=$PROFILE off-CPU=$OFF_CPU"
if [[ "$ACTION" == check ]]; then
    echo 'preflight OK; run with sudo -E and the run argument'
    exit 0
fi
[[ "$EUID" -eq 0 ]] || { echo "run requires root for perf/off-CPU; use sudo -E $0 run" >&2; exit 1; }

args=(
    --binary "$DUCKDB_BINARY"
    --properties "$BASE_PROPS"
    --column-sizes "$COLUMN_SIZES"
    --benchmark-json "$CPP_ROOT/testcase/benchmark.json"
    --benchmark clickbench-pixels-e0-24ssd
    --ssds 24 --threads 48 --repeat "$REPEAT" --query-set full
    --modes legacy dynamic selective-0 selective-1 selective-2 selective-4 static
    --buffer-hugepages on --timeout "$TIMEOUT"
    --output "$RUN_DIR"
)
if [[ "$PROFILE" == 1 ]]; then
    args+=(--perf --perf-call-graph dwarf --flamegraph-dir "$FLAMEGRAPH_DIR")
fi
if [[ "$OFF_CPU" == 1 ]]; then
    args+=(--off-cpu --flamegraph-dir "$FLAMEGRAPH_DIR")
fi
if [[ -f "$RUN_DIR/manifest.json" ]]; then
    args+=(--resume)
fi
exec python3 "$CPP_ROOT/testcase/buffer-pool/run_selective_benchmark.py" "${args[@]}"
