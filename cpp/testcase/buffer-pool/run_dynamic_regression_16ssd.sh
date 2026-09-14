#!/usr/bin/env bash
# Manual static/dynamic/legacy comparison for the largest regressions reported
# by testcase/buffer-pool/results/full-24ssd/REPORT.md.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CPP_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
SUITE="$CPP_ROOT/testcase/performance-test/run_suite.sh"
SOURCE_BENCHMARK_JSON=${SOURCE_BENCHMARK_JSON:-$CPP_ROOT/testcase/benchmark.json}
SOURCE_BENCHMARK=${SOURCE_BENCHMARK:-clickbench-pixels-e0-24ssd}
DUCKDB_BINARY=${DUCKDB_BINARY:-$CPP_ROOT/build/release/duckdb}
INVOKING_HOME=$HOME
if [[ -n ${SUDO_USER:-} && ${SUDO_USER} != root ]]; then
    INVOKING_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
fi
SOURCE_PROPERTIES=${SOURCE_PROPERTIES:-$INVOKING_HOME/opt/pixels/etc/pixels-cpp.properties}
COLUMN_SIZE_FILE=${COLUMN_SIZE_FILE:-/home/whz/pixels/clickbench-size-e0.csv}
if [[ -z ${FLAMEGRAPH_DIR:-} ]]; then
    for candidate in "$CPP_ROOT/third-party/FlameGraph" "$INVOKING_HOME/FlameGraph"; do
        if [[ -f "$candidate/stackcollapse-perf.pl" && -f "$candidate/flamegraph.pl" ]]; then
            FLAMEGRAPH_DIR=$candidate
            break
        fi
    done
fi
FLAMEGRAPH_DIR=${FLAMEGRAPH_DIR:-$INVOKING_HOME/FlameGraph}

# Top 20 regressions from full-24ssd/query_speedups.csv at 48 threads,
# ordered from the largest slowdown to the smallest (455% down to 71%).
QUERIES=${QUERIES:-"q25 q28 q26 q27 q43 q37 q30 q39 q40 q07 q38 q42 q02 q08 q22 q41 q23 q21 q01 q31"}
BUFFER_MODES=${BUFFER_MODES:-"legacy dynamic static"}
THREAD_LIST=${THREAD_LIST:-"48"}
PERF_STAT_REPEAT=${PERF_STAT_REPEAT:-3}
RUN_OFFCPU=${RUN_OFFCPU:-0}
RUN_IOSTAT=${RUN_IOSTAT:-1}
DROP_CACHES=${DROP_CACHES:-1}
KEEP_PERF_DATA=${KEEP_PERF_DATA:-1}
RESULT_ROOT=${RESULT_ROOT:-$SCRIPT_DIR/results}
SUITE_TAG=${SUITE_TAG:-dynamic-regression-16ssd-$(date +%Y%m%d-%H%M%S)}

fail() { echo "[error] $*" >&2; exit 1; }
[[ -x "$SUITE" ]] || fail "suite is not executable: $SUITE"
[[ -x "$DUCKDB_BINARY" ]] || fail "DuckDB binary is not executable: $DUCKDB_BINARY"
[[ -r "$SOURCE_BENCHMARK_JSON" ]] || fail "benchmark JSON not readable: $SOURCE_BENCHMARK_JSON"
[[ -r "$SOURCE_PROPERTIES" ]] || fail "properties not readable: $SOURCE_PROPERTIES"
[[ -r "$COLUMN_SIZE_FILE" ]] || fail "column-size CSV not readable: $COLUMN_SIZE_FILE"
[[ -f "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" ]] || fail "FlameGraph missing under: $FLAMEGRAPH_DIR"
command -v perf >/dev/null || fail "perf is not installed"
command -v iostat >/dev/null || [[ "$RUN_IOSTAT" == 0 ]] || fail "iostat is not installed (or set RUN_IOSTAT=0)"
[[ -x /usr/bin/time ]] || fail "/usr/bin/time is required for peak RSS and fault statistics"

WORK_DIR=$(mktemp -d /tmp/pixels-buffer-regression-16ssd.XXXXXX)
trap 'rm -rf "$WORK_DIR"' EXIT

# Keep the user's installed properties untouched. ConfigFactory resolves this
# isolated PIXELS_HOME in every child process launched by the suite.
TEST_PIXELS_HOME="$WORK_DIR/pixels-home"
mkdir -p "$TEST_PIXELS_HOME/cpp/etc"
cp "$SOURCE_PROPERTIES" "$TEST_PIXELS_HOME/cpp/etc/pixels-cpp.properties"
TEST_PROPERTIES="$TEST_PIXELS_HOME/cpp/etc/pixels-cpp.properties"

python3 - "$TEST_PROPERTIES" "$COLUMN_SIZE_FILE" "$THREAD_LIST" <<'PY'
import sys

path, column_sizes, threads = sys.argv[1:]
max_threads = max(map(int, threads.split()))
wanted = {
    "pixel.column.size.path": column_sizes,
    "pixel.globalStaticBytebuffer.columnSize": column_sizes,
    "pixel.static.buffer.threads": str(max_threads),
    "pixel.threads": str(max_threads),
}
lines = open(path, encoding="utf-8").readlines()
seen = set()
out = []
for line in lines:
    key = line.split("=", 1)[0].strip()
    if key in wanted:
        out.append(f"{key}={wanted[key]}\n")
        seen.add(key)
    else:
        out.append(line)
for key, value in wanted.items():
    if key not in seen:
        out.append(f"{key}={value}\n")
open(path, "w", encoding="utf-8").writelines(out)
PY

# Derive a 16-SSD benchmark from the first 16 paths in the established 24-SSD
# definition. Fail if its shape changed instead of silently testing fewer disks.
TEST_BENCHMARK_JSON="$WORK_DIR/benchmark-16ssd.json"
python3 - "$SOURCE_BENCHMARK_JSON" "$SOURCE_BENCHMARK" "$TEST_BENCHMARK_JSON" <<'PY'
import json
import re
import sys

source, key, target = sys.argv[1:]
data = json.load(open(source, encoding="utf-8"))
if key not in data:
    raise SystemExit(f"missing source benchmark: {key}")
paths = re.findall(r'"(/[^"]+)"', data[key])
if len(paths) < 16:
    raise SystemExit(f"{key} only contains {len(paths)} data paths; need 16")
selected = paths[:16]
quoted = ",\n    ".join(json.dumps(path) for path in selected)
data = {"clickbench-pixels-e0-16ssd": f"CREATE VIEW hits AS SELECT * FROM pixels_scan([\n    {quoted}\n]);"}
with open(target, "w", encoding="utf-8") as output:
    json.dump(data, output, indent=2)
    output.write("\n")
print("[16ssd] " + "\n[16ssd] ".join(selected))
PY

echo "[run] queries: $QUERIES"
echo "[run] modes: $BUFFER_MODES"
echo "[run] threads: $THREAD_LIST"
echo "[run] results: $RESULT_ROOT/$SUITE_TAG"
echo "[run] properties copy: $TEST_PROPERTIES"

# The suite performs separate repetitions for wall/RSS, on-CPU perf record,
# perf stat, and iostat. This avoids multiplexing all profilers in one process.
PIXELS_SRC=$(cd "$CPP_ROOT/.." && pwd) \
PIXELS_HOME="$TEST_PIXELS_HOME" \
PROPERTIES_PATH="$TEST_PROPERTIES" \
DUCKDB_BINARY="$DUCKDB_BINARY" \
BENCHMARK_JSON="$TEST_BENCHMARK_JSON" \
BENCHMARK_PREFIX=clickbench-pixels-e0 \
SSD_MODES=16ssd \
QUERIES="$QUERIES" \
BUFFER_MODES="$BUFFER_MODES" \
THREAD_LIST="$THREAD_LIST" \
PERF_STAT_REPEAT="$PERF_STAT_REPEAT" \
PERF_RECORD_FREQ="${PERF_RECORD_FREQ:-99}" \
PERF_CALLGRAPH="${PERF_CALLGRAPH:-fp}" \
FLAMEGRAPH_DIR="$FLAMEGRAPH_DIR" \
RESULT_ROOT="$RESULT_ROOT" \
SUITE_TAG="$SUITE_TAG" \
ENABLE_PIXELS_PROFILER="${ENABLE_PIXELS_PROFILER:-1}" \
RUN_PERF_ONCPU=1 \
RUN_PERF_STAT=1 \
RUN_OFFCPU="$RUN_OFFCPU" \
RUN_IOSTAT="$RUN_IOSTAT" \
DROP_CACHES="$DROP_CACHES" \
KEEP_PERF_DATA="$KEEP_PERF_DATA" \
"$SUITE"

python3 "$CPP_ROOT/testcase/performance-test/parse_results.py" \
    "$RESULT_ROOT/$SUITE_TAG"

echo "Completed: $RESULT_ROOT/$SUITE_TAG"
echo "Each case contains duckdb.log, resource_usage.txt, perf_stat.txt and an on-CPU SVG."
