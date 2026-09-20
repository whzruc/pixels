#!/usr/bin/env bash
# Pixels/io_uring + Pixels/SPDK: 44 queries, single vs double buffer, 12/24/48 threads.
# Parquet/io_uring: archived q24 cross-backend check at the same thread counts.
# Usage: sudo -E ./testcase/run_doublebuffer_24ssd_experiment.sh check|run
# ONLY=iouring|parquet|spdk limits a resumed run to one backend.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SUITE_ROOT="$SCRIPT_DIR/performance-test"
ACTION="${1:-check}"
[[ "$ACTION" == check || "$ACTION" == run ]] || { echo "usage: $0 [check|run]" >&2; exit 2; }

IOURING_BINARY="${IOURING_BINARY:-/tmp/pixels-aug25-rebuild/cpp/build/aug25-rebuild/duckdb}"
SPDK_BINARY="${SPDK_BINARY:-/tmp/pixels-spdk-pr4-ab-build/duckdb}"
BASE_PROPS="${BASE_PROPS:-$CPP_ROOT/experiments/aug25-reproduction.properties}"
COLUMN_SIZES="${COLUMN_SIZES:-/home/whz/pixels/clickbench-size-e0.csv}"
LBA_MAP="${LBA_MAP:-/tmp/pixels_lba_map.json}"
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-/home/whz/FlameGraph}"
RESULT_ROOT="${RESULT_ROOT:-$CPP_ROOT/testcase/experiment-results}"
RUN_ROOT="${RUN_ROOT:-$RESULT_ROOT/doublebuffer-24ssd-$(date +%Y%m%d-%H%M%S)}"
THREAD_LIST="${THREAD_LIST:-12 24 48}"
REPEAT="${REPEAT:-3}"
ONLY="${ONLY:-all}"
RUN_PERF_ONCPU="${RUN_PERF_ONCPU:-1}"
RUN_PERF_STAT="${RUN_PERF_STAT:-1}"
RUN_OFFCPU="${RUN_OFFCPU:-1}"
RUN_IOSTAT="${RUN_IOSTAT:-1}"
KEEP_PERF_DATA=0
DROP_CACHES="${DROP_CACHES:-1}"

case "$ONLY" in all|iouring|parquet|spdk) ;; *) echo "ONLY must be all, iouring, parquet or spdk" >&2; exit 2;; esac
[[ "$REPEAT" =~ ^[1-9][0-9]*$ ]] || { echo "REPEAT must be positive" >&2; exit 2; }
for value in "$RUN_PERF_ONCPU" "$RUN_PERF_STAT" "$RUN_OFFCPU" "$RUN_IOSTAT" "$DROP_CACHES"; do
    [[ "$value" =~ ^[01]$ ]] || { echo "perf/iostat/cache flags must be 0 or 1" >&2; exit 2; }
done
for file in "$IOURING_BINARY" "$SPDK_BINARY" "$BASE_PROPS" "$COLUMN_SIZES" \
            "$CPP_ROOT/testcase/benchmark.json" "$SUITE_ROOT/run_suite.sh" \
            "$SUITE_ROOT/run_suite_spdk.sh" "$SUITE_ROOT/run_suite_parquet_uring.sh" \
            "$SUITE_ROOT/lib_suite_common.sh"; do
    [[ -f "$file" ]] || { echo "missing: $file" >&2; exit 1; }
done
[[ -x "$IOURING_BINARY" && -x "$SPDK_BINARY" ]] || { echo 'DuckDB binary is not executable' >&2; exit 1; }
ldd "$SPDK_BINARY" | grep -q 'libspdk_nvme' || { echo "SPDK binary lacks libspdk_nvme: $SPDK_BINARY" >&2; exit 1; }
if [[ "$RUN_PERF_ONCPU" == 1 || "$RUN_OFFCPU" == 1 ]]; then
    for file in "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" "$FLAMEGRAPH_DIR/flamegraph.pl"; do
        [[ -f "$file" ]] || { echo "missing: $file" >&2; exit 1; }
    done
fi
for disk in $(seq -w 1 24); do
    mountpoint -q "/data/9a3-$disk" || { echo "SSD not mounted: /data/9a3-$disk" >&2; exit 1; }
done
if [[ "$ONLY" == all || "$ONLY" == spdk ]]; then
    [[ -r "$LBA_MAP" ]] || { echo "cannot read LBA map: $LBA_MAP (run check with sudo)" >&2; exit 1; }
fi

# Explicit list excludes q44/q45 test-only SQL and stays exactly 44 queries.
QUERIES=""
for number in $(seq 0 43); do printf -v query 'q%02d' "$number"; QUERIES+=" $query"; done
QUERIES="${QUERIES# }"
for query in $QUERIES; do
    [[ -f "$CPP_ROOT/pixels-duckdb/duckdb/benchmark/clickbench/queries/$query.sql" ||
       -f "$CPP_ROOT/pixels-duckdb/duckdb/benchmark/clickbench/queries-test/$query.sql" ]] || {
        echo "missing query SQL: $query" >&2; exit 1;
    }
done

echo "io_uring/Parquet binary: $IOURING_BINARY"
echo "SPDK binary: $SPDK_BINARY"
echo "output: $RUN_ROOT"
echo "matrix: 24 SSD, 44 Pixels queries, 12/24/48 threads, $REPEAT repetitions; Parquet q24"
echo "profilers: on-CPU=$RUN_PERF_ONCPU stat=$RUN_PERF_STAT off-CPU=$RUN_OFFCPU iostat=$RUN_IOSTAT; perf.data deleted after analysis"
if [[ "$ACTION" == check ]]; then
    echo 'preflight OK; run with sudo -E and the run argument'
    exit 0
fi
[[ "$EUID" -eq 0 ]] || { echo "run requires root for SPDK device binding; use sudo -E $0 run" >&2; exit 1; }

mkdir -p "$RUN_ROOT/config/etc"
PROPERTIES_PATH="$RUN_ROOT/config/etc/pixels-cpp.properties"
if [[ ! -f "$PROPERTIES_PATH" ]]; then
    python3 - "$BASE_PROPS" "$PROPERTIES_PATH" "$COLUMN_SIZES" "$LBA_MAP" <<'PY'
import sys
source, output, column_sizes, lba_map = sys.argv[1:]
values = {}
for line in open(source):
    if '=' in line and not line.lstrip().startswith('#'):
        key, value = line.split('=', 1)
        values[key.strip()] = value.strip()
values.update({
    'localfs.block.size': '4096',
    'localfs.spdk.lba_map': lba_map,
    'pixel.bufferpool.bufferpoolSize': '107257252',
    'pixel.bufferpool.bufferNum': '20',
    'pixel.bufferpool.extraSize': '3145728',
    'pixel.bufferpool.fixedsize': 'false',
    'pixel.bufferpool.hugepage': 'true',
    'pixel.column.size.path': column_sizes,
    'pixel.globalStaticBytebuffer.columnSize': column_sizes,
    'pixel.static.buffer.threads': '48',
    'pixel.threads': '48',
    'parquet.threads': '48',
    'pixel.stride': '10000',
    'pixel.enable.profiler': 'false',
    'pixels.enable.dynamic.buffer': 'false',
    'pixels.dynamic.selective.enabled': 'false',
})
with open(output, 'w') as file:
    file.writelines(f'{key}={value}\n' for key, value in sorted(values.items()))
PY
fi

PIXELS_HOME="$RUN_ROOT/config"
PIXELS_SRC="$(cd "$CPP_ROOT/.." && pwd)"
export PROPERTIES_PATH PIXELS_HOME PIXELS_SRC FLAMEGRAPH_DIR LBA_MAP
export RUN_PERF_ONCPU RUN_PERF_STAT RUN_OFFCPU RUN_IOSTAT KEEP_PERF_DATA DROP_CACHES
export THREAD_LIST QUERIES
export SSD_MODES=24ssd
export BENCHMARK_JSON="$CPP_ROOT/testcase/benchmark.json"
export RESULT_ROOT="$RUN_ROOT"

for repetition in $(seq 1 "$REPEAT"); do
    if [[ "$ONLY" == all || "$ONLY" == iouring ]]; then
        export DUCKDB_BINARY="$IOURING_BINARY" SUITE_TAG="iouring-r$repetition"
        export BENCHMARK_PREFIX=clickbench-pixels-e0 BUFFER_MODES='singlebuffer doublebuffer'
        bash "$SUITE_ROOT/run_suite.sh"
    fi
    if [[ "$ONLY" == all || "$ONLY" == parquet ]]; then
        export DUCKDB_BINARY="$IOURING_BINARY" SUITE_TAG="parquet-r$repetition"
        export BENCHMARK_PREFIX=clickbench-parquet-uring-e0
        export BUFFER_MODES='pq-async-singlebuffer pq-async-doublebuffer'
        QUERIES=q24 bash "$SUITE_ROOT/run_suite_parquet_uring.sh"
    fi
    if [[ "$ONLY" == all || "$ONLY" == spdk ]]; then
        export DUCKDB_BINARY="$SPDK_BINARY" SUITE_TAG="spdk-r$repetition"
        export BUFFER_MODES='spdk spdk-doublebuffer'
        bash "$SUITE_ROOT/run_suite_spdk.sh"
        for disk in $(seq -w 1 24); do
            mountpoint -q "/data/9a3-$disk" || {
                echo "SPDK cleanup did not restore /data/9a3-$disk; stop before further runs" >&2
                exit 1
            }
        done
    fi
done
echo "[done] $RUN_ROOT"
