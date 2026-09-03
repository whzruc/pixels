#!/bin/bash
#
# buffer-perf-concurrency-suite — perf on-CPU 火焰图 + perf stat + off-CPU 火焰图，
# 扫描多种 buffer mode × 多线程并发。
#
# 用法:
#   cd testcase/buffer-perf-concurrency-suite
#   ./run_suite.sh
#
# 环境变量（可选）:
#   QUERY=q45                    单个查询（默认 q45）
#   QUERIES="q01 q24 q45"        多个查询（覆盖 QUERY）；"all" 自动发现所有 q* 查询
#   SQL_DIR_EXTRA=...            q01-q43 所在目录（默认 clickbench/queries）
#   SSD_MODES="1ssd 6ssd 24ssd"  SSD 数量（对应 benchmark.json 中 clickbench-pixels-e0-<N>ssd）
#   BENCHMARK_PREFIX=clickbench-pixels-e0  benchmark key 前缀（默认）
#   BENCHMARK=clickbench-pixels-e0-24ssd
#   THREAD_LIST="1 8 16 24 48"
#   BUFFER_MODES="singlebuffer doublebuffer nonfixed pread-singlebuffer pread-doublebuffer"
#     可用 mode（与 run_perf_four_modes.py TEST_SCENARIOS 一致）:
#       singlebuffer      = fixed (async io_uring + fixed buf + global static)
#       fixed             = 同上（别名）
#       nonfixed          = async io_uring + non-fixed buf + global static
#       pread-singlebuffer= sync pread, no global static, no double-buffer
#       pread-doublebuffer= sync pread, no global static, double-buffer
#       doublebuffer      = async io_uring + fixed buf + global static + double-buffer
#   DUCKDB_BINARY  SQL_DIR  BENCHMARK_JSON  PROPERTIES_PATH  FLAMEGRAPH_DIR
#   RESULT_ROOT    — 结果根目录（默认本目录下 results/）
#   SUITE_TAG      — 本次运行子目录名（默认时间戳）
#   ENABLE_PIXELS_PROFILER=1 — 设置 pixel.enable.profiler=true（默认 0）
#   RUN_PERF_ONCPU=1  RUN_PERF_STAT=1  RUN_OFFCPU=1  RUN_IOSTAT=1  — 关闭某项设为 0
#   IOSTAT_INTERVAL=1  — iostat 采样间隔（秒）
#   IOSTAT_DEVICES="nvme0n1 nvme1n1 ..."  — 手动指定监控设备（默认自动从 benchmark.json 推导）
#   PERF_RECORD_FREQ=99   PERF_CALLGRAPH=fp   — perf record（fp 需帧指针）
#   PERF_STAT_REPEAT=2    — perf stat -r N
#   OFFCPU_STACK_SIZE=32768  MIN_BLOCK_US=1
#   ALLOCATOR=glibc|jemalloc — process allocator (default: glibc)
#   JEMALLOC_LIB=/lib/x86_64-linux-gnu/libjemalloc.so.2 — jemalloc path
#
# 权限:
#   perf record/stat 通常需 sudo 或 perf_event_paranoia
#   off-CPU 需 root（bcc）；若未 root 则跳过 off-CPU 并提示
#

set -euo pipefail

SUITE_ROOT="$(cd "$(dirname "$0")" && pwd)"
SUITE_REPO_ROOT="$(cd "$SUITE_ROOT/../.." && pwd)"
TESTCASE_ROOT="$(cd "$SUITE_ROOT/.." && pwd)"

# shellcheck source=lib_suite_common.sh
source "${SUITE_ROOT}/lib_suite_common.sh"

QUERY="${QUERY:-q45}"
# QUERIES: space-separated list, e.g. "q01 q24 q45", or "all" for all discovered queries
QUERIES="${QUERIES:-$QUERY}"
# SSD_MODES: space-separated list of ssd counts, e.g. "1ssd 6ssd 24ssd"
# Each maps to benchmark key clickbench-pixels-e0-<N>ssd in benchmark.json
SSD_MODES="${SSD_MODES:-24ssd}"
BUFFER_MODES="${BUFFER_MODES:-singlebuffer doublebuffer nonfixed pread-singlebuffer pread-doublebuffer}"
THREAD_LIST="${THREAD_LIST:-1 8 16 24 48}"
# RESUME=1 reuses an existing SUITE_TAG and skips cases already recorded as
# complete.  This is intentionally opt-in so normal runs keep their old
# overwrite-and-rerun behaviour.
RESUME="${RESUME:-0}"
[[ "$RESUME" == "0" || "$RESUME" == "1" ]] || {
    echo "[error] RESUME must be 0 or 1"
    exit 1
}
# Primary SQL dir (q45 etc); secondary dir for q01-q43
SQL_DIR="${SQL_DIR:-$SUITE_REPO_ROOT/pixels-duckdb/duckdb/benchmark/clickbench/queries-test}"
SQL_DIR_EXTRA="${SQL_DIR_EXTRA:-$SUITE_REPO_ROOT/pixels-duckdb/duckdb/benchmark/clickbench/queries}"

DUCKDB_BINARY="${DUCKDB_BINARY:-$SUITE_REPO_ROOT/build/release/duckdb}"
BENCHMARK_PREFIX="${BENCHMARK_PREFIX:-clickbench-pixels-e0}"
BENCHMARK_JSON="${BENCHMARK_JSON:-$TESTCASE_ROOT/benchmark.json}"
INVOKING_HOME="$HOME"
if [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]]; then
    INVOKING_HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
fi
PROPERTIES_PATH="${PROPERTIES_PATH:-$INVOKING_HOME/opt/pixels/etc/pixels-cpp.properties}"
PIXELS_SRC="${PIXELS_SRC:-$(cd "$SUITE_REPO_ROOT/.." && pwd)}"
PIXELS_HOME="${PIXELS_HOME:-$(cd "$(dirname "$PROPERTIES_PATH")/.." && pwd)}"
export PIXELS_SRC PIXELS_HOME PROPERTIES_PATH

RESULT_ROOT="${RESULT_ROOT:-$SUITE_ROOT/results}"
SUITE_TAG="${SUITE_TAG:-$(date +%Y%m%d_%H%M%S)}"

ENABLE_PIXELS_PROFILER="${ENABLE_PIXELS_PROFILER:-0}"
[[ "$ENABLE_PIXELS_PROFILER" == "0" || "$ENABLE_PIXELS_PROFILER" == "1" ]] || {
    echo "[error] ENABLE_PIXELS_PROFILER must be 0 or 1"
    exit 1
}
export ENABLE_PIXELS_PROFILER

# Pixels allocations use the process allocator (new/malloc/posix_memalign),
# while DuckDB's bundled jemalloc extension uses duckdb_je_* symbols and does
# not interpose libc globally.  Make the allocator choice explicit for A/B
# experiments and ensure every benchmark child inherits the same choice.
ALLOCATOR="${ALLOCATOR:-glibc}"
case "$ALLOCATOR" in
    glibc)
        ;;
    jemalloc)
        JEMALLOC_LIB="${JEMALLOC_LIB:-/lib/x86_64-linux-gnu/libjemalloc.so.2}"
        [[ -r "$JEMALLOC_LIB" ]] || {
            echo "[error] jemalloc library not found: $JEMALLOC_LIB"
            exit 1
        }
        if [[ -n "${LD_PRELOAD:-}" ]]; then
            export LD_PRELOAD="$JEMALLOC_LIB:$LD_PRELOAD"
        else
            export LD_PRELOAD="$JEMALLOC_LIB"
        fi
        ;;
    *)
        echo "[error] ALLOCATOR must be glibc or jemalloc (got: $ALLOCATOR)"
        exit 1
        ;;
esac
export ALLOCATOR JEMALLOC_LIB

RUN_PERF_ONCPU="${RUN_PERF_ONCPU:-1}"
RUN_PERF_STAT="${RUN_PERF_STAT:-1}"
RUN_OFFCPU="${RUN_OFFCPU:-1}"
RUN_IOSTAT="${RUN_IOSTAT:-1}"
IOSTAT_INTERVAL="${IOSTAT_INTERVAL:-1}"   # sampling interval in seconds
# IOSTAT_DEVICES: space-separated list of block devices to monitor.
# Auto-detected from benchmark data paths when empty.
IOSTAT_DEVICES="${IOSTAT_DEVICES:-}"
# RUN_INIT_BASELINE: superseded by attach-mode perf stat (run_perf_stat_attach).
# Kept for reference but disabled by default.
RUN_INIT_BASELINE="${RUN_INIT_BASELINE:-0}"

PERF_RECORD_FREQ="${PERF_RECORD_FREQ:-99}"
PERF_CALLGRAPH="${PERF_CALLGRAPH:-fp}"
PERF_STAT_REPEAT="${PERF_STAT_REPEAT:-2}"
# Keep perf.data after SVG generation (default: delete to save disk space)
KEEP_PERF_DATA="${KEEP_PERF_DATA:-0}"
# Drop page cache before each duckdb run for cold-cache IO measurements
DROP_CACHES="${DROP_CACHES:-1}"

OFFCPU_STACK_SIZE="${OFFCPU_STACK_SIZE:-32768}"
MIN_BLOCK_US="${MIN_BLOCK_US:-1}"
OFFCPU_RELAX_KPTR="${OFFCPU_RELAX_KPTR:-1}"

SCRIPT_DIR="$TESTCASE_ROOT"
OFFCPUTIME_BIN="${SCRIPT_DIR}/offcputime_patched.py"
[[ -x "$OFFCPUTIME_BIN" ]] || OFFCPUTIME_BIN="/usr/sbin/offcputime-bpfcc"

PERF_WRAPPER=(sudo -E)
if [[ "${PERF_NO_SUDO:-0}" == "1" ]]; then
    PERF_WRAPPER=()
fi

# Drop Linux page cache (sync + echo 3 > drop_caches) before each duckdb run
# so iostat captures real disk IO rather than cache hits.
# Tries: root direct → sudo -n → PERF_WRAPPER → skip with warning.
drop_page_cache() {
    [[ "${DROP_CACHES}" == "1" ]] || return 0
    sync
    if [[ "$EUID" -eq 0 ]]; then
        echo 3 > /proc/sys/vm/drop_caches
    elif sudo -n sh -c "echo 3 > /proc/sys/vm/drop_caches" 2>/dev/null; then
        : # sudo -n succeeded
    elif [[ "${#PERF_WRAPPER[@]}" -gt 0 ]]; then
        "${PERF_WRAPPER[@]}" sh -c "echo 3 > /proc/sys/vm/drop_caches" 2>/dev/null || \
            echo "  [warn] drop_caches: no permission (set DROP_CACHES=0 to suppress)"
    else
        echo "  [warn] drop_caches: not root and no sudo — page cache not dropped"
    fi
}

usage() {
    sed -n '1,35p' "$0"
    exit 0
}
[[ "${1:-}" == "-h" || "${1:-}" == "--help" ]] && usage

suite_resolve_flamegraph_dir
export PYTHONUNBUFFERED=1

# Resolve a query name to its SQL file path (searches SQL_DIR then SQL_DIR_EXTRA)
resolve_query_sql() {
    local q="$1"
    local f
    f="${SQL_DIR}/${q}.sql"
    [[ -f "$f" ]] && { echo "$f"; return 0; }
    f="${SQL_DIR_EXTRA}/${q}.sql"
    [[ -f "$f" ]] && { echo "$f"; return 0; }
    echo "[error] SQL not found for query '$q' in $SQL_DIR or $SQL_DIR_EXTRA" >&2
    return 1
}

# Expand "all" to every q* SQL found across both dirs (sorted, deduped, skip load/q00)
expand_queries() {
    local raw="$1"
    if [[ "$raw" == "all" ]]; then
        {
            [[ -d "$SQL_DIR" ]]       && ls "$SQL_DIR"/*.sql 2>/dev/null | xargs -I{} basename {} .sql
            [[ -d "$SQL_DIR_EXTRA" ]] && ls "$SQL_DIR_EXTRA"/*.sql 2>/dev/null | xargs -I{} basename {} .sql
        } | grep -E '^q[0-9]+$' | sort -t q -k2 -n | uniq
    else
        echo "$raw"
    fi
}

QUERIES=$(expand_queries "$QUERIES")

# Validate all query SQL files and benchmark keys up front
for _q in $QUERIES; do
    resolve_query_sql "$_q" >/dev/null || exit 1
done
for _ssd in $SSD_MODES; do
    _bk="${BENCHMARK_PREFIX}-${_ssd}"
    python3 -c "
import json, sys
d = json.load(open('$BENCHMARK_JSON'))
if '$_bk' not in d:
    print(f'Missing benchmark key: $_bk', file=sys.stderr); sys.exit(1)
" || exit 1
done
[[ -x "$DUCKDB_BINARY" ]] || { echo "Missing duckdb: $DUCKDB_BINARY"; exit 1; }
[[ -f "$BENCHMARK_JSON" ]] || { echo "Missing benchmark.json: $BENCHMARK_JSON"; exit 1; }
[[ -f "$STACKCOLLAPSE_PERF" && -f "$FLAMEGRAPH_PL" ]] || {
    echo "FlameGraph not found. Set FLAMEGRAPH_DIR (need stackcollapse-perf.pl + flamegraph.pl)"
    exit 1
}

RUN_DIR="${RESULT_ROOT}/${SUITE_TAG}"
mkdir -p "$RUN_DIR"

META_FILE="${RUN_DIR}/RUN_META.txt"
{
    echo "SUITE_TAG=$SUITE_TAG"
    echo "DATE=$(date -Iseconds)"
    echo "QUERIES=$QUERIES SSD_MODES=$SSD_MODES BENCHMARK_PREFIX=$BENCHMARK_PREFIX"
    echo "BUFFER_MODES=$BUFFER_MODES"
    echo "THREAD_LIST=$THREAD_LIST"
    echo "DUCKDB_BINARY=$DUCKDB_BINARY"
    echo "PROPERTIES_PATH=$PROPERTIES_PATH"
    echo "ENABLE_PIXELS_PROFILER=$ENABLE_PIXELS_PROFILER"
    echo "PERF_RECORD_FREQ=$PERF_RECORD_FREQ PERF_CALLGRAPH=$PERF_CALLGRAPH PERF_STAT_REPEAT=$PERF_STAT_REPEAT"
    echo "RUN_PERF_ONCPU=$RUN_PERF_ONCPU RUN_PERF_STAT=$RUN_PERF_STAT RUN_OFFCPU=$RUN_OFFCPU RUN_IOSTAT=$RUN_IOSTAT"
} | tee "$META_FILE"

SUMMARY_CSV="${RUN_DIR}/summary_wall_time.csv"
# wall_time_s is the DuckDB EXPLAIN ANALYZE "Total Time", i.e. query-engine
# execution time. shell_wall_time_s retains the CLI ".timer on" end-to-end
# value for startup/result-printing/cleanup diagnostics.
if [[ "$RESUME" != "1" || ! -s "$SUMMARY_CSV" ]]; then
    echo "query,ssd_mode,mode,threads,wall_time_s,shell_wall_time_s,note" >"$SUMMARY_CSV"
fi
IO_SUMMARY_CSV="${RUN_DIR}/summary_io.csv"
if [[ "$RESUME" != "1" || ! -s "$IO_SUMMARY_CSV" ]]; then
    echo "query,ssd_mode,mode,threads,source,read_iops,read_kib_s,read_mib_s" >"$IO_SUMMARY_CSV"
fi

INIT_BASELINE_DIR="${RUN_DIR}/init_baselines"
[[ "$RUN_PERF_STAT" == "1" && "$RUN_INIT_BASELINE" == "1" ]] && mkdir -p "$INIT_BASELINE_DIR"

# Auto-detect block devices backing benchmark data paths (if not set explicitly).
# Derives unique parent NVMe devices from the mount points used by the benchmark.
_resolve_iostat_devices() {
    local bench_json="$1"
    python3 - "$bench_json" <<'PY'
import json, re, subprocess, sys

data = json.load(open(sys.argv[1]))
# Collect all unique /data/<dir> roots across all benchmark entries
roots = set()
for v in data.values():
    for p in re.findall(r'"(/[^"]+)"', v):
        parts = p.strip('/').split('/')
        if len(parts) >= 2:
            roots.add('/' + '/'.join(parts[:2]))

devs = set()
for root in sorted(roots):
    try:
        out = subprocess.check_output(['df', '--output=source', root],
                                      stderr=subprocess.DEVNULL, text=True)
        src = out.strip().splitlines()[-1].replace('/dev/', '')
        # strip partition suffix (nvme0n1p1 -> nvme0n1)
        import re as _re
        src = _re.sub(r'p\d+$', '', src)
        if src.startswith('nvme') or src.startswith('sd'):
            devs.add(src)
    except Exception:
        pass
print(' '.join(sorted(devs)))
PY
}

if [[ "$RUN_IOSTAT" == "1" && -z "$IOSTAT_DEVICES" ]]; then
    IOSTAT_DEVICES=$(_resolve_iostat_devices "$BENCHMARK_JSON" || true)
    if [[ -z "$IOSTAT_DEVICES" ]]; then
        echo "[warn] Could not auto-detect iostat devices; set IOSTAT_DEVICES explicitly or RUN_IOSTAT=0"
        RUN_IOSTAT=0
    else
        echo "[iostat] monitoring devices: $IOSTAT_DEVICES"
    fi
fi

KPTR_SAVED=""
relax_kptr() {
    [[ "$OFFCPU_RELAX_KPTR" == "0" ]] && return 0
    [[ -r /proc/sys/kernel/kptr_restrict ]] || return 0
    KPTR_SAVED=$(cat /proc/sys/kernel/kptr_restrict 2>/dev/null || true)
    echo 0 >/proc/sys/kernel/kptr_restrict 2>/dev/null || true
}
restore_kptr() {
    [[ -n "${KPTR_SAVED:-}" ]] && [[ -w /proc/sys/kernel/kptr_restrict ]] && echo "$KPTR_SAVED" >/proc/sys/kernel/kptr_restrict 2>/dev/null || true
}
trap restore_kptr EXIT

append_summary() {
    local mode="$1" threads="$2" log="$3"
    local total_time="" shell_wall_time=""
    total_time=$(grep -oP 'Total Time:\s*\K[\d.]+' "$log" 2>/dev/null | tail -1 || true)
    shell_wall_time=$(grep -oP 'Run Time \(s\): real \K[\d.]+' "$log" 2>/dev/null | tail -1 || true)
    [[ -n "$total_time" ]] || total_time="NA"
    [[ -n "$shell_wall_time" ]] || shell_wall_time="NA"
    echo "${query},${ssd_mode},${mode},${threads},${total_time},${shell_wall_time},explain_analyze_total_time" >>"$SUMMARY_CSV"
}

case_is_complete() {
    local tag="$1" query="$2" ssd_mode="$3" mode="$4" threads="$5"
    [[ "$RESUME" == "1" ]] || return 1
    [[ -f "${RUN_DIR}/${tag}/.complete" ]] && return 0

    # Compatibility with runs made before .complete markers were introduced.
    # A summary row is written only after DuckDB exits successfully.
    awk -F, -v q="$query" -v s="$ssd_mode" -v m="$mode" -v t="$threads" \
        'NR > 1 && $1 == q && $2 == s && $3 == m && $4 == t { found=1; exit }
         END { exit !found }' "$SUMMARY_CSV"
}

# Measure perf stat for BufferPool init only (trivial query, 1 thread).
# Saves result to ${INIT_BASELINE_DIR}/init_baseline_${mode}.txt.
# Requires CREATE_VIEW_SQL to be loaded beforehand.
collect_init_baseline() {
    local mode="$1"
    local bfile="${INIT_BASELINE_DIR}/init_baseline_${mode}.txt"
    [[ -f "$bfile" ]] && { echo "  [baseline] already collected: $bfile"; return 0; }

    local temp_db temp_sql
    temp_db=$(mktemp -u /tmp/pixels_suite_XXXXXX.duckdb)
    printf '%s\nset threads=1;\n.exit\n' "$CREATE_VIEW_SQL" \
        | "$DUCKDB_BINARY" "$temp_db" >/dev/null 2>&1 || {
        echo "[warn] baseline: failed to create temp db for mode=$mode"
        rm -f "$temp_db"
        return 0
    }
    temp_sql=$(mktemp /tmp/pixels_suite_init_XXXXXX.sql)
    # One thread, minimal pixels scan — enough to trigger GlobalStaticBufferPool init.
    printf 'set threads=1;\n.timer on\nexplain analyze SELECT * FROM hits LIMIT 1;\n.exit\n' \
        >"$temp_sql"

    local rep="$PERF_STAT_REPEAT"
    [[ "$rep" =~ ^[0-9]+$ ]] || rep=2
    [[ "$rep" -lt 1 ]] && rep=1
    set +e
    "${PERF_WRAPPER[@]}" perf stat \
        -r "$rep" -ddd \
        -e cycles,instructions,cache-references,cache-misses,branches,branch-misses,page-faults,context-switches,cpu-migrations,task-clock \
        -o "$bfile" \
        -- "$DUCKDB_BINARY" "$temp_db" <"$temp_sql"
    local st=$?
    set -e
    if [[ $st -ne 0 ]]; then
        echo "[warn] baseline perf stat failed ($st) for mode=$mode, retry minimal -ddd"
        set +e
        "${PERF_WRAPPER[@]}" perf stat -r "$rep" -ddd -o "$bfile" \
            -- "$DUCKDB_BINARY" "$temp_db" <"$temp_sql" || true
        set -e
    fi
    rm -f "$temp_db" "$temp_sql"
    echo "  [baseline] → $bfile"
}

run_perf_oncpu() {
    local out_prefix="$1"
    local db="$2"
    local sql="$3"
    local title="$4"

    local data perf_txt folded svg
    data="${out_prefix}_oncpu_cpu-clock.data"
    perf_txt="${out_prefix}_oncpu_cpu-clock.perf.script"
    folded="${out_prefix}_oncpu_cpu-clock.folded"
    svg="${out_prefix}_oncpu_cpu-clock.svg"

    "${PERF_WRAPPER[@]}" perf record \
        -F "$PERF_RECORD_FREQ" \
        -e cpu-clock \
        -g "--call-graph=${PERF_CALLGRAPH}" \
        -o "$data" -- \
        "$DUCKDB_BINARY" "$db" <"$sql"

    "${PERF_WRAPPER[@]}" perf script -i "$data" >"$perf_txt"
    "$STACKCOLLAPSE_PERF" "$perf_txt" >"$folded"
    "$FLAMEGRAPH_PL" \
        --title="$title" \
        --countname="samples" \
        --color=hot \
        --width=1600 \
        "$folded" >"$svg"
    rm -f "$perf_txt" "$folded"
    if [[ "${KEEP_PERF_DATA:-0}" != "1" ]]; then
        rm -f "$data"
    fi
    echo "  → $svg"
}

run_perf_stat() {
    local out_txt="$1"
    local db="$2"
    local sql="$3"

    local rep="$PERF_STAT_REPEAT"
    [[ "$rep" =~ ^[0-9]+$ ]] || rep=2
    [[ "$rep" -lt 1 ]] && rep=1
    set +e
    "${PERF_WRAPPER[@]}" perf stat \
        -r "$rep" \
        -ddd \
        -e cycles,instructions,cache-references,cache-misses,branches,branch-misses,page-faults,context-switches,cpu-migrations,task-clock \
        -o "$out_txt" \
        -- \
        "$DUCKDB_BINARY" "$db" <"$sql"
    local pst=$?
    set -e
    if [[ "$pst" -ne 0 ]]; then
        echo "[warn] perf stat with -e bundle failed ($pst), retry minimal -ddd"
        "${PERF_WRAPPER[@]}" perf stat -r "$rep" -ddd -o "$out_txt" -- \
            "$DUCKDB_BINARY" "$db" <"$sql"
    fi
    echo "  → $out_txt"
}

# Attach-mode perf stat: start duckdb in background, wait for the ready-file
# written by the C++ extension after GlobalStaticBufferPool init, then attach
# perf stat -p <pid>.  Counts only the post-init query phase.
# For pread-* modes (no BufferPool) falls back to run_perf_stat directly.
run_perf_stat_attach() {
    local out_txt="$1"
    local db="$2"
    local sql="$3"
    local mode="$4"

    local rep="$PERF_STAT_REPEAT"
    [[ "$rep" =~ ^[0-9]+$ ]] || rep=2
    [[ "$rep" -lt 1 ]] && rep=1

    local ready_file duck_log duck_pid w perf_pid perf_out
    local tmp_dir
    tmp_dir=$(mktemp -d /tmp/pixels_perf_attach_XXXXXX)

    local run_outputs=()
    local i
    for (( i=1; i<=rep; i++ )); do
        duck_log="${tmp_dir}/duck_${i}.log"
        perf_out="${tmp_dir}/perf_${i}.txt"

        if [[ "$mode" == pread-* || "$mode" == pq-* || "$mode" == arrow-* ]]; then
            # pread-* / pq-* have no GlobalStaticBufferPool and no ready-file.
            # Launch in background and attach perf stat -p once the thread-pool
            # is up (~0.5 s is ample; query execution takes 8-28 s).
            "$DUCKDB_BINARY" "$db" <"$sql" >"$duck_log" 2>&1 &
            duck_pid=$!
            w=0
            while [[ $w -lt 10 ]]; do
                kill -0 "$duck_pid" 2>/dev/null || break
                sleep 0.05
                w=$(( w + 1 ))
            done
            if ! kill -0 "$duck_pid" 2>/dev/null; then
                echo "  [warn] pread run ${i}/${rep}: duckdb exited before perf could attach"
                run_outputs+=( "$perf_out" )
                continue
            fi
            echo "  pread run ${i}/${rep}: attaching perf stat -p ${duck_pid} (after ${w}×50ms)"
        else
            ready_file="${tmp_dir}/ready_${i}"
            PIXELS_PERF_READY_FILE="$ready_file" \
                "$DUCKDB_BINARY" "$db" <"$sql" >"$duck_log" 2>&1 &
            duck_pid=$!

            w=0
            while [[ ! -f "$ready_file" ]] && [[ $w -lt 300 ]]; do
                kill -0 "$duck_pid" 2>/dev/null || break
                sleep 0.1
                w=$(( w + 1 ))
            done

            if [[ ! -f "$ready_file" ]]; then
                echo "  [warn] run ${i}/${rep}: ready-file not found after ${w}×100ms, falling back to full-process stat"
                wait "$duck_pid" || true
                set +e
                "${PERF_WRAPPER[@]}" perf stat -ddd \
                    -e cycles,instructions,cache-references,cache-misses,branches,branch-misses,page-faults,context-switches,cpu-migrations,task-clock \
                    -o "$perf_out" \
                    -- "$DUCKDB_BINARY" "$db" <"$sql"
                set -e
                run_outputs+=( "$perf_out" )
                rm -f "$ready_file"
                continue
            fi
            echo "  run ${i}/${rep}: ready-file detected (waited ${w}×100ms), attaching perf stat"
            rm -f "$ready_file"
        fi

        set +e
        "${PERF_WRAPPER[@]}" perf stat -p "$duck_pid" \
            -ddd \
            -e cycles,instructions,cache-references,cache-misses,branches,branch-misses,page-faults,context-switches,cpu-migrations,task-clock \
            -o "$perf_out" &
        perf_pid=$!
        set -e

        wait "$duck_pid" || true
        wait "$perf_pid" || true
        run_outputs+=( "$perf_out" )
    done

    {
        echo "# perf stat (attach mode, post-init only) — ${rep} run(s)"
        echo "# started on $(date)"
        echo ""
        for f in "${run_outputs[@]}"; do
            [[ -f "$f" ]] && cat "$f"
            echo ""
        done
    } >"$out_txt"

    rm -rf "$tmp_dir"
    echo "  → $out_txt"
}

# Run iostat alongside duckdb, sampling every IOSTAT_INTERVAL seconds.
# Produces <out_dir>/iostat.txt with per-device read throughput time series,
# and <out_dir>/iostat_summary.txt with per-device aggregate stats.
run_iostat_with_duckdb() {
    local out_dir="$1"
    local db="$2"
    local sql="$3"
    local duck_log="${out_dir}/duckdb_iostat.log"
    local raw="${out_dir}/iostat.txt"
    local summary="${out_dir}/iostat_summary.txt"

    local devargs=()
    for dev in $IOSTAT_DEVICES; do devargs+=("$dev"); done

    # Launch duckdb in background.
    "$DUCKDB_BINARY" "$db" <"$sql" >"$duck_log" 2>&1 &
    local duck_pid=$!

    # Run iostat for the duration of the duckdb process.
    # -d: device only  -x: extended stats  -y: skip first (cumulative) report
    # -p: per-partition (skipped; we use parent devices)
    iostat -dxy "${devargs[@]}" "$IOSTAT_INTERVAL" \
        | awk -v devs="$(IFS=,; echo "${devargs[*]}")" '
            /^Device/ { header=$0; next }
            /^$/ { if (block!="") { print ts; print header; printf "%s\n", block; block="" } next }
            { block = (block=="" ? $0 : block "\n" $0) }
            END { if (block!="") { print header; printf "%s\n", block } }
        ' >"$raw" &
    local iostat_pid=$!

    wait "$duck_pid" || true
    # Give iostat one extra interval to flush its last sample, then kill it.
    sleep "$IOSTAT_INTERVAL"
    kill "$iostat_pid" 2>/dev/null || true
    wait "$iostat_pid" 2>/dev/null || true

    # Summarise: per-device mean rkB/s, peak rkB/s, mean r/s, mean r_await.
    python3 - "$raw" "$summary" <<'PY'
import sys, re, collections

raw_path, summary_path = sys.argv[1], sys.argv[2]
# iostat -dxy output: fields depend on kernel version; find column indices dynamically
DevStats = collections.defaultdict(lambda: {'rkBs': [], 'rs': [], 'r_await': [], 'aqu': [], 'util': []})

header_cols = []
with open(raw_path) as f:
    for line in f:
        line = line.rstrip()
        if line.startswith('Device'):
            header_cols = line.split()
            continue
        if not line or line.startswith('Linux'):
            continue
        parts = line.split()
        if not parts:
            continue
        dev = parts[0]
        if not (dev.startswith('nvme') or dev.startswith('sd')):
            continue
        def col(name):
            try:
                idx = header_cols.index(name)
                return float(parts[idx])
            except (ValueError, IndexError):
                return None
        v = col('rkB/s') or col('r_kB/s') or 0.0
        DevStats[dev]['rkBs'].append(v)
        v = col('r/s'); DevStats[dev]['rs'].append(v or 0.0)
        v = col('r_await'); DevStats[dev]['r_await'].append(v or 0.0)
        v = col('aqu-sz') or col('avgqu-sz') or col('aqu_sz'); DevStats[dev]['aqu'].append(v or 0.0)
        v = col('%util'); DevStats[dev]['util'].append(v or 0.0)

lines = ['device,samples,mean_rkBs,peak_rkBs,mean_rs,mean_r_await_ms,mean_aqu_sz,mean_util_pct']
total_mean = 0.0
total_iops = 0.0
for dev in sorted(DevStats):
    s = DevStats[dev]
    n = len(s['rkBs'])
    if n == 0:
        continue
    mean_rkBs = sum(s['rkBs']) / n
    peak_rkBs = max(s['rkBs'])
    mean_rs   = sum(s['rs'])   / n
    mean_rawa = sum(s['r_await']) / n
    mean_aqu  = sum(s['aqu'])  / n
    mean_util = sum(s['util']) / n
    total_mean += mean_rkBs
    total_iops += mean_rs
    lines.append(f"{dev},{n},{mean_rkBs:.1f},{peak_rkBs:.1f},{mean_rs:.1f},{mean_rawa:.2f},{mean_aqu:.2f},{mean_util:.1f}")
lines.append(f"TOTAL,,{total_mean:.1f},,{total_iops:.1f},,,")

with open(summary_path, 'w') as f:
    f.write('\n'.join(lines) + '\n')
print('\n'.join(lines))
PY

    local total_line
    total_line=$(awk -F, '$1=="TOTAL" {print; exit}' "$summary")
    if [[ -n "$total_line" ]]; then
        local read_kib_s read_iops read_mib_s
        read_kib_s=$(awk -F, '{print $3}' <<<"$total_line")
        read_iops=$(awk -F, '{print $5}' <<<"$total_line")
        read_mib_s=$(awk -v x="$read_kib_s" 'BEGIN {printf "%.6f", x/1024}')
        echo "${query},${ssd_mode},${mode},${threads},iostat,${read_iops},${read_kib_s},${read_mib_s}" >>"$IO_SUMMARY_CSV"
    fi

    echo "  → $raw"
    echo "  → $summary"
}

run_offcpu_case() {
    local out_dir="$1"
    local mode="$2"
    local threads="$3"

    if [[ "$EUID" -ne 0 ]]; then
        echo "[skip] off-CPU needs root (bcc). Run: sudo -E $0 ..."
        return 0
    fi

    relax_kptr

    local folded svg log err trace_s
    folded="${out_dir}/offcpu.folded"
    svg="${out_dir}/offcpu.svg"
    log="${out_dir}/duckdb_offcpu.log"
    err="${out_dir}/offcputime.err"

    suite_apply_buffer_scenario "$mode" || return 1
    sleep 0.3

    local pair db sql
    pair=$(suite_prepare_temp_db_and_sql "$threads") || return 1
    db="${pair%%|*}"
    sql="${pair##*|}"

    drop_page_cache
    "$DUCKDB_BINARY" "$db" <"$sql" >"$log" 2>&1 || {
        suite_cleanup_temp_pair "$pair"
        return 1
    }
    local query_time_s measured_s trace_s
    query_time_s=$(grep -oP 'Run Time \(s\): real \K[\d.]+' "$log" | tail -1 || true)
    [[ -n "$query_time_s" ]] || query_time_s=$(grep -oP '\([\d.]+s\)' "$log" | grep -oP '[\d.]+' | tail -1 || true)
    [[ -n "$query_time_s" ]] || query_time_s="30"
    measured_s=$(python3 -c "print(int(float('$query_time_s')))")
    trace_s=$(( measured_s / 2 ))
    [[ "$trace_s" -lt 5 ]] && trace_s=5

    # 触发信号在 stdout；stdout+stderr -> trigger_log（监控用），结束后复制到 log
    local trigger_log="${out_dir}/duckdb_offcpu.trigger"
    drop_page_cache
    "$DUCKDB_BINARY" "$db" <"$sql" >"$trigger_log" 2>&1 &
    local duck_pid=$!
    local w=0
    while [[ ! -f "/proc/${duck_pid}/maps" ]] && [[ $w -lt 50 ]]; do sleep 0.1; w=$((w + 1)); done

    # pread-*/pq-* 模式没有 GlobalStaticBufferPool，跳过触发等待直接开始采集
    local trig="Global Static ByteBuffer Pool initialized successfully"
    if [[ "$mode" == pread-* || "$mode" == pq-* || "$mode" == arrow-* ]]; then
        echo "  ${mode}: no BufferPool trigger, starting offcputime immediately"
    else
        w=0
        while [[ $w -lt 300 ]]; do
            grep -qF "$trig" "$trigger_log" 2>/dev/null && break
            kill -0 "$duck_pid" 2>/dev/null || break
            sleep 0.1
            w=$((w + 1))
        done
        if grep -qF "$trig" "$trigger_log" 2>/dev/null; then
            echo "  trigger detected (waited ${w}×100ms)"
        else
            echo "  [warn] trigger not found after ${w}×100ms, starting offcputime anyway"
        fi
    fi

    "$OFFCPUTIME_BIN" --stack-storage-size "$OFFCPU_STACK_SIZE" \
        -p "$duck_pid" -f -m "$MIN_BLOCK_US" "$trace_s" \
        >"$folded" 2>"$err" &
    local oc_pid=$!
    sleep 0.5
    if ! kill -0 "$oc_pid" 2>/dev/null; then
        cat "$err" >&2
        kill "$duck_pid" 2>/dev/null || true
        suite_cleanup_temp_pair "$pair"
        rm -f "$trigger_log"
        return 1
    fi
    wait "$oc_pid"
    kill -0 "$duck_pid" 2>/dev/null && wait "$duck_pid" || true
    # trigger_log contains stdout+stderr; copy to log for reference
    cp "$trigger_log" "$log" 2>/dev/null || true
    suite_cleanup_temp_pair "$pair"
    rm -f "$trigger_log"

    if [[ -s "$folded" ]]; then
        "$FLAMEGRAPH_PL" \
            --title="${query} ${BENCHMARK} ${mode} off-CPU ${threads}t (${trace_s}s)" \
            --countname="microseconds" \
            --color=io \
            --width=1600 \
            "$folded" >"$svg"
        rm -f "$folded"
        echo "  → $svg"
    else
        echo "[warn] empty offcpu folded"; cat "$err" >&2
    fi
}

suite_ensure_iouring_selected || exit 1

# The baseline subtraction pre-pass is superseded by run_perf_stat_attach
# (which attaches perf stat only after the ready-file fires, so init is never
# counted).  Keep RUN_INIT_BASELINE=0 as default; the block below is a no-op.
if [[ "$RUN_PERF_STAT" == "1" && "$RUN_INIT_BASELINE" == "1" ]]; then
    echo ""
    echo "=== Collecting BufferPool init perf baselines ==="
    _bk0="${BENCHMARK_PREFIX}-$(echo "$SSD_MODES" | awk '{print $1}')"
    suite_load_create_view_sql "$BENCHMARK_JSON" "$_bk0" || {
        echo "[warn] Could not load create-view SQL for baseline; skipping init baselines"
        RUN_INIT_BASELINE=0
    }
    if [[ "$RUN_INIT_BASELINE" == "1" ]]; then
        for _mode in $BUFFER_MODES; do
            [[ "$_mode" == pread-* ]] && { echo "  [baseline] skip pread mode: $_mode"; continue; }
            echo "--- init baseline: ${_mode} ---"
            suite_apply_buffer_scenario "$_mode" || continue
            sleep 0.2
            collect_init_baseline "$_mode" || true
        done
        CREATE_VIEW_SQL=""  # will be reloaded in the main loop
    fi
fi

for query in $QUERIES; do
    QUERY_SQL_FILE=$(resolve_query_sql "$query") || exit 1
    suite_load_query_content "$QUERY_SQL_FILE" || exit 1

    for ssd_mode in $SSD_MODES; do
        BENCHMARK="${BENCHMARK_PREFIX}-${ssd_mode}"
        suite_load_create_view_sql "$BENCHMARK_JSON" "$BENCHMARK" || exit 1

        for mode in $BUFFER_MODES; do
            for threads in $THREAD_LIST; do
                tag="${query}_${ssd_mode}_${mode}_t${threads}"
                OUT="${RUN_DIR}/${tag}"
                mkdir -p "$OUT"
                echo ""
                echo "========== ${tag} =========="

                if case_is_complete "$tag" "$query" "$ssd_mode" "$mode" "$threads"; then
                    echo "[resume] already complete, skipping ${tag}"
                    continue
                fi

                suite_apply_buffer_scenario "$mode" || exit 1
                sleep 0.3

                pair=$(suite_prepare_temp_db_and_sql "$threads") || exit 1
                db="${pair%%|*}"
                sql="${pair##*|}"

                duck_log="${OUT}/duckdb.log"
                drop_page_cache
                if "$DUCKDB_BINARY" "$db" <"$sql" >"$duck_log" 2>&1; then
                    append_summary "$mode" "$threads" "$duck_log"
                else
                    echo "[error] duckdb failed $tag"; cat "$duck_log"; suite_cleanup_temp_pair "$pair"; exit 1
                fi

                prefix="${OUT}/${tag}"

                if [[ "$RUN_PERF_ONCPU" == "1" ]]; then
                    echo "--- perf record on-CPU ---"
                    suite_apply_buffer_scenario "$mode"
                    pair2=$(suite_prepare_temp_db_and_sql "$threads") || exit 1
                    db2="${pair2%%|*}"
                    sql2="${pair2##*|}"
                    drop_page_cache
                    run_perf_oncpu "$prefix" "$db2" "$sql2" "${query} ${BENCHMARK} ${mode} ${threads}t on-CPU"
                    suite_cleanup_temp_pair "$pair2"
                fi

                if [[ "$RUN_PERF_STAT" == "1" ]]; then
                    echo "--- perf stat (attach mode) ---"
                    suite_apply_buffer_scenario "$mode"
                    pair3=$(suite_prepare_temp_db_and_sql "$threads") || exit 1
                    db3="${pair3%%|*}"
                    sql3="${pair3##*|}"
                    drop_page_cache
                    run_perf_stat_attach "${OUT}/perf_stat.txt" "$db3" "$sql3" "$mode"
                    suite_cleanup_temp_pair "$pair3"
                fi

                if [[ "$RUN_IOSTAT" == "1" ]]; then
                    echo "--- iostat (${IOSTAT_INTERVAL}s interval) ---"
                    suite_apply_buffer_scenario "$mode"
                    pair4=$(suite_prepare_temp_db_and_sql "$threads") || exit 1
                    db4="${pair4%%|*}"
                    sql4="${pair4##*|}"
                    drop_page_cache
                    run_iostat_with_duckdb "$OUT" "$db4" "$sql4"
                    suite_cleanup_temp_pair "$pair4"
                fi

                suite_cleanup_temp_pair "$pair"

                if [[ "$RUN_OFFCPU" == "1" ]]; then
                    echo "--- off-CPU (bcc) ---"
                    run_offcpu_case "$OUT" "$mode" "$threads"
                fi
                touch "${OUT}/.complete"
            done
        done
    done
done

echo ""
echo "--- Suite complete ---"
echo "Results: $RUN_DIR"
echo "Summary: $SUMMARY_CSV"
if [[ "$RUN_IOSTAT" == "1" ]]; then
    echo "I/O summary: $IO_SUMMARY_CSV"
    cat "$IO_SUMMARY_CSV"
fi
find "$RUN_DIR" -maxdepth 2 -type f \( -name '*.svg' -o -name 'perf_stat.txt' -o -name 'perf_stat_corrected.txt' -o -name 'duckdb.log' -o -name 'iostat_summary.txt' -o -name 'summary_io.csv' \) | sort
