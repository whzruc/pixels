#!/bin/bash
#
# run_suite_parquet_uring.sh — perf suite wrapper for read_parquet_uring
#
# 在 run_suite.sh 基础上，设置 Parquet 专用默认值：
#   BENCHMARK_PREFIX = clickbench-parquet-uring-e0
#   BUFFER_MODES     = pq-async-doublebuffer pq-async-singlebuffer pq-pread
#   SSD_MODES        = 1ssd  （默认只跑单盘，加快速度）
#
# 用法：
#   cd testcase/performance-test
#   ./run_suite_parquet_uring.sh                # 默认 q45，1ssd，3种 Parquet 模式
#
# 覆盖示例：
#   QUERY=q01 SSD_MODES="1ssd 6ssd" ./run_suite_parquet_uring.sh
#   QUERIES="q01 q05 q45" BUFFER_MODES="pq-async-doublebuffer pq-pread" ./run_suite_parquet_uring.sh
#   QUERIES=all SSD_MODES=6ssd THREAD_LIST="8 24" ./run_suite_parquet_uring.sh
#
# 可用 BUFFER_MODES（read_parquet_uring 专用）：
#   pq-async-doublebuffer   io_uring + double-buffer（主要测试目标）
#   pq-async-singlebuffer   io_uring + single-buffer
#   pq-pread                pread 同步 IO（基线）
#
# 所有 run_suite.sh 的环境变量仍然有效（THREAD_LIST、RUN_PERF_ONCPU 等）。

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── Parquet-specific defaults ────────────────────────────────────────────────
export BENCHMARK_PREFIX="${BENCHMARK_PREFIX:-clickbench-parquet-uring-e0}"
export SSD_MODES="${SSD_MODES:-1ssd}"
export BUFFER_MODES="${BUFFER_MODES:-pq-async-doublebuffer pq-async-singlebuffer pq-pread}"
export QUERY="${QUERY:-q45}"
export THREAD_LIST="${THREAD_LIST:-1 8 24}"

# duckdb.bin 比 duckdb 更新（包含 read_parquet_uring）
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
if [[ -x "$REPO_ROOT/build/release/duckdb.bin" ]]; then
    export DUCKDB_BINARY="${DUCKDB_BINARY:-$REPO_ROOT/build/release/duckdb.bin}"
elif [[ -x "$REPO_ROOT/build/release/duckdb" ]]; then
    export DUCKDB_BINARY="${DUCKDB_BINARY:-$REPO_ROOT/build/release/duckdb}"
fi

echo "[parquet-uring suite] BENCHMARK_PREFIX=$BENCHMARK_PREFIX"
echo "[parquet-pixels suite] SSD_MODES=$SSD_MODES"
echo "[parquet-pixels suite] BUFFER_MODES=$BUFFER_MODES"
echo "[parquet-pixels suite] DUCKDB_BINARY=${DUCKDB_BINARY:-<default>}"
echo ""

exec "$SCRIPT_DIR/run_suite.sh" "$@"
