#!/bin/bash
# testcase/buffer-perf-concurrency-suite/run_suite_spdk.sh
#
# SPDK 专用性能测试套件，与 run_suite.sh 功能对齐：
#   perf on-CPU 火焰图 + perf stat + off-CPU 火焰图
#   扫描多种 SPDK buffer mode × 多线程并发
#
# 与 run_suite.sh 的关键区别：
#   - 测试开始前自动 bind_vfio（卸载文件系统，绑定 VFIO-PCI）
#   - 测试结束/中断时自动 unbind_vfio（重新挂载文件系统）
#   - CREATE VIEW 从 LBA map JSON 生成显式文件列表，不依赖文件系统 glob
#   - CREATE VIEW 与 SELECT 在同一 DuckDB session 内执行（SPDK 要求）
#   - 默认 binary 指向 build/spdk-release/duckdb（SPDK 启用构建）
#   - 默认 buffer mode 仅 spdk（可加 spdk-doublebuffer）
#   - 不使用 GlobalStaticBufferPool / ready-file attach-mode perf stat
#   - VFIO 下无内核块设备，使用 SPDK 提交/完成路径内部计数 IOPS 与带宽
#
# 前置条件：
#   1. hugepages 已配置：echo 8192 > /sys/.../hugepages-2048kB/nr_hugepages
#   2. LBA map 已生成：python3 testcase/spdk/gen_lba_map.py
#   3. 以 root 运行：sudo -E bash run_suite_spdk.sh
#
# 用法：
#   cd testcase/buffer-perf-concurrency-suite
#   sudo -E bash run_suite_spdk.sh
#
# 环境变量（可选）：
#   QUERY=q45                    单个查询（默认 q45）
#   QUERIES="q01 q45"            多个查询，"all" 自动发现
#   THREAD_LIST="1 8 16 24 48"
#   SSD_MODES="1ssd 6ssd 24ssd"  SSD 数量（与 run_suite.sh 格式一致）
#   BUFFER_MODES="spdk spdk-doublebuffer"
#   DUCKDB_BINARY                默认 build/spdk-release/duckdb
#   LBA_MAP                      默认 /tmp/pixels_lba_map.json
#   DATASET_PREFIX               默认 /clickbench/pixels-e0-fb/
#   PROPERTIES_PATH              默认 $HOME/opt/pixels/etc/pixels-cpp.properties
#   FLAMEGRAPH_DIR               FlameGraph 工具目录
#   RESULT_ROOT                  结果根目录（默认本目录下 results_spdk/）
#   SUITE_TAG                    本次运行子目录名（默认时间戳）
#   ENABLE_PIXELS_PROFILER=1     设置 pixel.enable.profiler=true（默认 0）
#   每个基准 case 总会输出 summary_io.csv（不依赖 Pixels profiler）
#   RUN_PERF_ONCPU=1  RUN_PERF_STAT=1  RUN_OFFCPU=1  — 关闭某项设为 0
#   DROP_CACHES=1                每次查询前 drop page cache
#   PERF_RECORD_FREQ=99
#   PERF_CALLGRAPH=fp            需帧指针编译（SPDK 构建已启用）
#   PERF_STAT_REPEAT=2
#   KEEP_PERF_DATA=0
#   OFFCPU_STACK_SIZE=32768  MIN_BLOCK_US=1

set -euo pipefail

SUITE_ROOT="$(cd "$(dirname "$0")" && pwd)"
SUITE_REPO_ROOT="$(cd "$SUITE_ROOT/../.." && pwd)"
TESTCASE_ROOT="$(cd "$SUITE_ROOT/.." && pwd)"

# 复用 lib_suite_common.sh 中的 suite_resolve_flamegraph_dir 和
# suite_apply_buffer_scenario（已内含 spdk / spdk-doublebuffer 场景）
source "${SUITE_ROOT}/lib_suite_common.sh"

# ── 环境变量 ──────────────────────────────────────────────────────────────────

QUERY="${QUERY:-q06 q45}"
QUERIES="${QUERIES:-$QUERY}"
SSD_MODES="${SSD_MODES:-24ssd}"
BUFFER_MODES="${BUFFER_MODES:-spdk}"
THREAD_LIST="${THREAD_LIST:-48}"

SQL_DIR="${SQL_DIR:-$SUITE_REPO_ROOT/pixels-duckdb/duckdb/benchmark/clickbench/queries-test}"
SQL_DIR_EXTRA="${SQL_DIR_EXTRA:-$SUITE_REPO_ROOT/pixels-duckdb/duckdb/benchmark/clickbench/queries}"

DUCKDB_BINARY="${DUCKDB_BINARY:-$SUITE_REPO_ROOT/build/spdk-release/duckdb}"
LBA_MAP="${LBA_MAP:-/tmp/pixels_lba_map.json}"
DATASET_PREFIX="${DATASET_PREFIX:-/clickbench/pixels-e0-fb/}"
INVOKING_HOME="$HOME"
if [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]]; then
    INVOKING_HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
fi
PROPERTIES_PATH="${PROPERTIES_PATH:-$INVOKING_HOME/opt/pixels/etc/pixels-cpp.properties}"
PIXELS_SRC="${PIXELS_SRC:-$(cd "$SUITE_REPO_ROOT/.." && pwd)}"
PIXELS_HOME="${PIXELS_HOME:-$(cd "$(dirname "$PROPERTIES_PATH")/.." && pwd)}"
export PIXELS_SRC PIXELS_HOME PROPERTIES_PATH

RESULT_ROOT="${RESULT_ROOT:-$SUITE_ROOT/results_spdk}"
SUITE_TAG="${SUITE_TAG:-$(date +%Y%m%d_%H%M%S)}"

ENABLE_PIXELS_PROFILER="${ENABLE_PIXELS_PROFILER:-0}"
[[ "$ENABLE_PIXELS_PROFILER" == "0" || "$ENABLE_PIXELS_PROFILER" == "1" ]] || {
    echo "[error] ENABLE_PIXELS_PROFILER must be 0 or 1"
    exit 1
}
export ENABLE_PIXELS_PROFILER

RUN_PERF_ONCPU="${RUN_PERF_ONCPU:-1}"
RUN_PERF_STAT="${RUN_PERF_STAT:-1}"
RUN_OFFCPU="${RUN_OFFCPU:-1}"
DROP_CACHES="${DROP_CACHES:-1}"

PERF_RECORD_FREQ="${PERF_RECORD_FREQ:-99}"
PERF_CALLGRAPH="${PERF_CALLGRAPH:-fp}"
PERF_STAT_REPEAT="${PERF_STAT_REPEAT:-2}"
KEEP_PERF_DATA="${KEEP_PERF_DATA:-0}"

OFFCPU_STACK_SIZE="${OFFCPU_STACK_SIZE:-32768}"
MIN_BLOCK_US="${MIN_BLOCK_US:-1}"
OFFCPU_RELAX_KPTR="${OFFCPU_RELAX_KPTR:-1}"

OFFCPUTIME_BIN="${TESTCASE_ROOT}/offcputime_patched.py"
[[ -x "$OFFCPUTIME_BIN" ]] || OFFCPUTIME_BIN="/usr/sbin/offcputime-bpfcc"

# ── 数据盘 PCI 列表（排除系统盘 0000:07:00.0）────────────────────────────────

DATA_PCI=(
    0000:c2:00.0  0000:c3:00.0  0000:c4:00.0  0000:c5:00.0
    0000:81:00.0  0000:82:00.0  0000:83:00.0  0000:84:00.0
    0000:85:00.0  0000:86:00.0  0000:87:00.0  0000:88:00.0
    0000:41:00.0  0000:42:00.0  0000:43:00.0  0000:44:00.0
    0000:45:00.0  0000:46:00.0  0000:47:00.0  0000:48:00.0
    0000:01:00.0  0000:02:00.0  0000:03:00.0  0000:04:00.0
)

# ── VFIO 绑定 / 解绑 ──────────────────────────────────────────────────────────

VFIO_BOUND=0

bind_vfio() {
    [[ "$VFIO_BOUND" == "1" ]] && return 0
    # Mark cleanup as required before the first destructive operation.  If the
    # script is interrupted halfway through unmounting/binding, EXIT cleanup
    # must restore the partially transitioned device set as well.
    VFIO_BOUND=1
    echo ""
    echo "[bind_vfio] 卸载数据盘挂载点..."
    for i in $(seq -w 1 24); do
        mnt="/data/9a3-$i"
        if mountpoint -q "$mnt" 2>/dev/null; then
            umount "$mnt" && echo "  umount $mnt OK" || echo "  [WARN] umount $mnt failed"
        fi
    done

    echo "[bind_vfio] 加载 vfio-pci 模块..."
    modprobe vfio-pci
    if [[ -f /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]]; then
        echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null || true
    fi

    echo "[bind_vfio] 解绑内核 nvme 驱动，绑定 vfio-pci..."
    for pci in "${DATA_PCI[@]}"; do
        if [[ -e "/sys/bus/pci/drivers/nvme/$pci" ]]; then
            echo "$pci" > /sys/bus/pci/drivers/nvme/unbind
        fi
        echo "vfio-pci" > "/sys/bus/pci/devices/$pci/driver_override" 2>/dev/null || true
        if [[ -e /sys/bus/pci/drivers/vfio-pci ]]; then
            echo "$pci" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null && \
                echo "  $pci → vfio-pci OK" || \
                echo "  [WARN] $pci → vfio-pci bind failed (already bound?)"
        fi
    done
    echo "[bind_vfio] 完成。/data/9a3-XX 挂载点不再可用，SPDK 可以使用设备。"
}

unbind_vfio() {
    [[ "$VFIO_BOUND" == "0" ]] && return 0
    echo ""
    echo "[unbind_vfio] 从 vfio-pci 解绑，归还内核 nvme 驱动..."
    for pci in "${DATA_PCI[@]}"; do
        if [[ -e "/sys/bus/pci/drivers/vfio-pci/$pci" ]]; then
            echo "$pci" > /sys/bus/pci/drivers/vfio-pci/unbind && \
                echo "  unbind $pci from vfio-pci OK" || true
        fi
        echo "" > "/sys/bus/pci/devices/$pci/driver_override" 2>/dev/null || true
        if [[ -e "/sys/bus/pci/drivers/nvme/$pci" ]]; then
            echo "  $pci → nvme already OK"
        elif echo "$pci" > /sys/bus/pci/drivers/nvme/bind 2>/dev/null; then
            echo "  $pci → nvme OK"
        else
            echo "  [WARN] $pci → nvme bind failed"
        fi
    done

    echo "[unbind_vfio] 等待内核枚举 NVMe 设备 (2s)..."
    sleep 2

    echo "[unbind_vfio] 重新挂载文件系统（/etc/fstab）..."
    mount -a && echo "[unbind_vfio] mount -a OK" || echo "[WARN] mount -a 有错误，请手动检查"

    echo "[unbind_vfio] 验证挂载点..."
    local ok=0 fail=0
    for i in $(seq -w 1 24); do
        # Under `set -e`, `(( ok++ ))` returns status 1 when the old value is
        # zero and aborts cleanup on the first mount.  Assignment arithmetic
        # always has a successful shell status.
        if mountpoint -q "/data/9a3-$i" 2>/dev/null; then
            ok=$((ok + 1))
        else
            fail=$((fail + 1))
        fi
    done
    echo "[unbind_vfio] 挂载：$ok/24 OK，$fail 未挂载"
    VFIO_BOUND=0
}

# 脚本退出时保证解绑（Ctrl+C / set -e 触发的 exit 均生效），并保留进入
# cleanup 前的真实退出码，避免清理命令将成功 benchmark 改报为失败。
cleanup_on_exit() {
    local original_rc=$?
    trap - EXIT
    # Once recovery starts, repeated Ctrl-C must not interrupt it and leave the
    # machine in another partially bound state.
    trap '' INT TERM
    set +e
    unbind_vfio
    exit "$original_rc"
}
trap cleanup_on_exit EXIT

on_signal() {
    local signal_rc="$1"
    echo ""
    echo "[signal] interrupted; restoring NVMe devices and mounts..."
    exit "$signal_rc"
}
trap 'on_signal 130' INT
trap 'on_signal 143' TERM

# ── 工具函数 ──────────────────────────────────────────────────────────────────

usage() {
    sed -n '1,55p' "$0"
    exit 0
}
[[ "${1:-}" == "-h" || "${1:-}" == "--help" ]] && usage

# Recovery-only mode for a previous process that was killed before its cleanup
# trap could run.  This does not start a benchmark or bind any new device.
if [[ "${1:-}" == "--restore-devices" ]]; then
    [[ "$EUID" -eq 0 ]] || {
        echo "[error] --restore-devices requires root; use sudo -E bash $0 --restore-devices"
        exit 1
    }
    VFIO_BOUND=1
    unbind_vfio
    trap - EXIT INT TERM
    exit 0
fi

drop_page_cache() {
    [[ "${DROP_CACHES}" == "1" ]] || return 0
    sync
    if [[ "$EUID" -eq 0 ]]; then
        echo 3 > /proc/sys/vm/drop_caches
    elif sudo -n sh -c "echo 3 > /proc/sys/vm/drop_caches" 2>/dev/null; then
        :
    else
        echo "  [warn] cannot drop page cache (not root)"
    fi
}

# 将 "24ssd" → 24，"6ssd" → 6 等格式转为整数
ssd_mode_to_count() {
    echo "${1//ssd/}"
}

# 将 "24ssd" → "clickbench-pixels-spdk-e0-24ssd"（仅用于命名）
ssd_mode_to_label() {
    echo "clickbench-pixels-spdk-e0-${1}"
}

# 从 LBA map 生成当前 SSD 数量的 CREATE VIEW SQL
load_create_view_spdk() {
    local ssd_count="$1"
    CREATE_VIEW_SQL=$(python3 - "$LBA_MAP" "$DATASET_PREFIX" "$ssd_count" <<'PY'
import json, sys

lba_map_path = sys.argv[1]
prefix       = sys.argv[2]
ssd_count    = int(sys.argv[3])

with open(lba_map_path) as f:
    lba_map = json.load(f)

paths = []
for p in sorted(lba_map.keys()):
    if prefix not in p:
        continue
    try:
        disk_num = int(p.split('/data/9a3-')[1].split('/')[0])
    except (IndexError, ValueError):
        continue
    if 1 <= disk_num <= ssd_count:
        paths.append(p)

if not paths:
    print(f"[error] LBA map 中未找到 prefix='{prefix}' ssd_count={ssd_count} 的文件", file=sys.stderr)
    sys.exit(1)

file_list = ',\n    '.join(f'"{p}"' for p in paths)
print(f"CREATE VIEW hits AS SELECT * FROM pixels_scan([\n    {file_list}\n]);")
PY
    ) || return 1
}

# 准备临时 SQL 文件（CREATE VIEW + SELECT 合并，同一 session）
# 返回路径；调用方负责删除
spdk_make_sql() {
    local threads="$1"
    local tmp_sql
    tmp_sql=$(mktemp /tmp/pixels_spdk_query_XXXXXX.sql)
    printf '%s\nset threads=%s;\n.timer on\nexplain analyze %s\n.exit\n' \
        "$CREATE_VIEW_SQL" "$threads" "$QUERY_CONTENT" >"$tmp_sql"
    echo "$tmp_sql"
}

append_summary() {
    local mode="$1" threads="$2" log="$3"
    local total_time="" shell_wall_time=""
    total_time=$(grep -oP 'Total Time:\s*\K[\d.]+' "$log" 2>/dev/null | tail -1 || true)
    shell_wall_time=$(grep -oP 'Run Time \(s\): real \K[\d.]+' "$log" 2>/dev/null | tail -1 || true)
    [[ -n "$total_time" ]] || total_time="NA"
    [[ -n "$shell_wall_time" ]] || shell_wall_time="NA"
    echo "${query},${ssd_mode},${mode},${threads},${total_time},${shell_wall_time},explain_analyze_total_time" >>"$SUMMARY_CSV"
}

KPTR_SAVED=""
relax_kptr() {
    [[ "$OFFCPU_RELAX_KPTR" == "0" ]] && return 0
    [[ -r /proc/sys/kernel/kptr_restrict ]] || return 0
    KPTR_SAVED=$(cat /proc/sys/kernel/kptr_restrict 2>/dev/null || true)
    echo 0 >/proc/sys/kernel/kptr_restrict 2>/dev/null || true
}
restore_kptr() {
    [[ -n "${KPTR_SAVED:-}" ]] && [[ -w /proc/sys/kernel/kptr_restrict ]] && \
        echo "$KPTR_SAVED" >/proc/sys/kernel/kptr_restrict 2>/dev/null || true
}

# ── perf 采集函数 ─────────────────────────────────────────────────────────────

run_perf_oncpu() {
    local out_prefix="$1"
    local sql="$2"       # 含 CREATE VIEW + SELECT 的 SQL 文件
    local title="$3"

    local tmp_db data perf_txt folded svg
    tmp_db=$(mktemp -u /tmp/pixels_spdk_XXXXXX.duckdb)
    data="${out_prefix}_oncpu.data"
    perf_txt="${out_prefix}_oncpu.perf.script"
    folded="${out_prefix}_oncpu.folded"
    svg="${out_prefix}_oncpu.svg"

    drop_page_cache
    perf record \
        -F "$PERF_RECORD_FREQ" \
        -e cpu-clock \
        -g "--call-graph=${PERF_CALLGRAPH}" \
        -o "$data" -- \
        "$DUCKDB_BINARY" "$tmp_db" <"$sql"
    rm -f "$tmp_db"

    perf script -i "$data" >"$perf_txt"
    "$STACKCOLLAPSE_PERF" "$perf_txt" >"$folded"
    "$FLAMEGRAPH_PL" \
        --title="$title" \
        --countname="samples" \
        --color=hot \
        --width=1600 \
        "$folded" >"$svg"
    rm -f "$perf_txt" "$folded"
    [[ "${KEEP_PERF_DATA:-0}" != "1" ]] && rm -f "$data"
    echo "  → $svg"
}

run_perf_stat() {
    local out_txt="$1"
    local sql="$2"

    local rep="$PERF_STAT_REPEAT"
    [[ "$rep" =~ ^[0-9]+$ ]] || rep=2
    [[ "$rep" -lt 1 ]] && rep=1

    # SPDK 无 GlobalStaticBufferPool ready-file；对整个进程采样
    # 每次 run 使用独立 tmp_db，使 SPDK 重新初始化
    local run_outputs=() i perf_out tmp_db
    for (( i=1; i<=rep; i++ )); do
        perf_out=$(mktemp /tmp/pixels_spdk_stat_XXXXXX.txt)
        tmp_db=$(mktemp -u /tmp/pixels_spdk_XXXXXX.duckdb)
        drop_page_cache
        set +e
        perf stat \
            -ddd \
            -e cycles,instructions,cache-references,cache-misses,branches,branch-misses,page-faults,context-switches,cpu-migrations,task-clock \
            -o "$perf_out" \
            -- "$DUCKDB_BINARY" "$tmp_db" <"$sql"
        local pst=$?
        set -e
        rm -f "$tmp_db"
        if [[ $pst -ne 0 ]]; then
            echo "  [warn] run ${i}/${rep}: perf stat failed ($pst), retry minimal -ddd"
            drop_page_cache
            tmp_db=$(mktemp -u /tmp/pixels_spdk_XXXXXX.duckdb)
            set +e
            perf stat -ddd -o "$perf_out" -- "$DUCKDB_BINARY" "$tmp_db" <"$sql" || true
            set -e
            rm -f "$tmp_db"
        fi
        run_outputs+=( "$perf_out" )
    done

    {
        echo "# perf stat (whole-process, SPDK) — ${rep} run(s)"
        echo "# started on $(date)"
        echo ""
        for f in "${run_outputs[@]}"; do
            [[ -f "$f" ]] && cat "$f"
            echo ""
        done
    } >"$out_txt"

    rm -f "${run_outputs[@]}"
    echo "  → $out_txt"
}

run_offcpu() {
    local out_dir="$1"
    local threads="$2"

    if [[ "$EUID" -ne 0 ]]; then
        echo "  [skip] off-CPU 需要 root"
        return 0
    fi
    if [[ ! -x "$OFFCPUTIME_BIN" ]]; then
        echo "  [skip] offcputime 未找到: $OFFCPUTIME_BIN"
        return 0
    fi

    relax_kptr

    local sql folded svg duck_log err
    sql=$(spdk_make_sql "$threads")
    folded="${out_dir}/offcpu.folded"
    svg="${out_dir}/offcpu.svg"
    duck_log="${out_dir}/duckdb_offcpu.log"
    err="${out_dir}/offcputime.err"

    # 先跑一次获取参考耗时
    local tmp_db trace_s measured_s
    tmp_db=$(mktemp -u /tmp/pixels_spdk_XXXXXX.duckdb)
    drop_page_cache
    "$DUCKDB_BINARY" "$tmp_db" <"$sql" >"$duck_log" 2>&1 || {
        rm -f "$tmp_db" "$sql"
        echo "  [warn] off-CPU: dry run failed"
        return 0
    }
    rm -f "$tmp_db"
    local query_time_s
    query_time_s=$(grep -oP 'Run Time \(s\): real \K[\d.]+' "$duck_log" | tail -1 || echo "30")
    measured_s=$(python3 -c "print(int(float('$query_time_s')))")
    trace_s=$(( measured_s / 2 ))
    [[ "$trace_s" -lt 5 ]] && trace_s=5

    # 正式 off-CPU 采集
    tmp_db=$(mktemp -u /tmp/pixels_spdk_XXXXXX.duckdb)
    drop_page_cache
    "$DUCKDB_BINARY" "$tmp_db" <"$sql" >"$duck_log" 2>&1 &
    local duck_pid=$!

    # 等待进程出现（最多 5s）
    local w=0
    while [[ ! -f "/proc/${duck_pid}/maps" ]] && [[ $w -lt 50 ]]; do
        sleep 0.1; w=$(( w + 1 ))
    done

    # SPDK 初始化（spdk_env_init + spdk_nvme_probe）通常耗时 1-3s
    # 等待 4s 让 SPDK 完成初始化后再开始采集 off-CPU
    echo "  off-CPU: 等待 SPDK 初始化 (4s)..."
    local init_wait=0
    while [[ $init_wait -lt 40 ]]; do
        kill -0 "$duck_pid" 2>/dev/null || break
        sleep 0.1; init_wait=$(( init_wait + 1 ))
    done

    if ! kill -0 "$duck_pid" 2>/dev/null; then
        echo "  [warn] off-CPU: duckdb 在采集前已退出"
        rm -f "$tmp_db" "$sql"
        return 0
    fi

    "$OFFCPUTIME_BIN" --stack-storage-size "$OFFCPU_STACK_SIZE" \
        -p "$duck_pid" -f -m "$MIN_BLOCK_US" "$trace_s" \
        >"$folded" 2>"$err" &
    local oc_pid=$!
    sleep 0.5
    if ! kill -0 "$oc_pid" 2>/dev/null; then
        cat "$err" >&2
        kill "$duck_pid" 2>/dev/null || true
        rm -f "$tmp_db" "$sql"
        return 1
    fi
    wait "$oc_pid" || true
    kill -0 "$duck_pid" 2>/dev/null && wait "$duck_pid" || true
    rm -f "$tmp_db" "$sql"

    if [[ -s "$folded" ]]; then
        "$FLAMEGRAPH_PL" \
            --title="${query} spdk-e0-${ssd_mode} ${mode} off-CPU ${threads}t (${trace_s}s)" \
            --countname="microseconds" \
            --color=io \
            --width=1600 \
            "$folded" >"$svg"
        rm -f "$folded"
        echo "  → $svg"
    else
        echo "  [warn] off-CPU folded 为空"
        [[ -s "$err" ]] && cat "$err" >&2
    fi
}

# ── SQL 文件 & query 展开 ─────────────────────────────────────────────────────

resolve_query_sql() {
    local q="$1"
    local f
    f="${SQL_DIR}/${q}.sql"
    [[ -f "$f" ]] && { echo "$f"; return 0; }
    f="${SQL_DIR_EXTRA}/${q}.sql"
    [[ -f "$f" ]] && { echo "$f"; return 0; }
    echo "[error] SQL not found for '$q' in $SQL_DIR or $SQL_DIR_EXTRA" >&2
    return 1
}

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

# ── 前置检查 ──────────────────────────────────────────────────────────────────

if [[ "$EUID" -ne 0 ]]; then
    echo "[error] 本脚本需要 root 权限（VFIO 绑定 + hugepage + perf）"
    echo "        请使用: sudo -E bash $0"
    exit 1
fi

[[ -x "$DUCKDB_BINARY" ]]   || { echo "[error] SPDK DuckDB 未找到: $DUCKDB_BINARY"; exit 1; }
[[ -f "$LBA_MAP" ]]         || { echo "[error] LBA map 未找到: $LBA_MAP  (先运行 gen_lba_map.py)"; exit 1; }
[[ -f "$PROPERTIES_PATH" ]] || { echo "[error] properties 未找到: $PROPERTIES_PATH"; exit 1; }

# 检查 hugepages
HP=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)
if [[ "$HP" -lt 512 ]]; then
    echo "[error] hugepages 不足 ($HP)，SPDK 至少需要 512 页（建议 8192）"
    echo "        echo 8192 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
    exit 1
fi

suite_resolve_flamegraph_dir
[[ -f "$STACKCOLLAPSE_PERF" && -f "$FLAMEGRAPH_PL" ]] || {
    echo "[error] FlameGraph 未找到，设置 FLAMEGRAPH_DIR"
    exit 1
}

QUERIES=$(expand_queries "$QUERIES")
for _q in $QUERIES; do
    resolve_query_sql "$_q" >/dev/null || exit 1
done

# 验证所有 buffer mode 合法（必须是 spdk 或 spdk-doublebuffer）
for _mode in $BUFFER_MODES; do
    case "$_mode" in
        spdk|spdk-doublebuffer) ;;
        *) echo "[error] 非法 SPDK buffer mode: $_mode（仅支持 spdk / spdk-doublebuffer）"; exit 1 ;;
    esac
done

# ── 初始化结果目录 ────────────────────────────────────────────────────────────

RUN_DIR="${RESULT_ROOT}/${SUITE_TAG}"
mkdir -p "$RUN_DIR"

META_FILE="${RUN_DIR}/RUN_META.txt"
{
    echo "SUITE_TAG=$SUITE_TAG"
    echo "DATE=$(date -Iseconds)"
    echo "QUERIES=$QUERIES  SSD_MODES=$SSD_MODES"
    echo "BUFFER_MODES=$BUFFER_MODES"
    echo "THREAD_LIST=$THREAD_LIST"
    echo "DUCKDB_BINARY=$DUCKDB_BINARY"
    echo "LBA_MAP=$LBA_MAP"
    echo "DATASET_PREFIX=$DATASET_PREFIX"
    echo "PROPERTIES_PATH=$PROPERTIES_PATH"
    echo "PIXELS_SRC=$PIXELS_SRC"
    echo "PIXELS_HOME=$PIXELS_HOME"
    echo "ENABLE_PIXELS_PROFILER=$ENABLE_PIXELS_PROFILER"
    echo "HUGEPAGES=$HP"
    echo "PERF_RECORD_FREQ=$PERF_RECORD_FREQ  PERF_CALLGRAPH=$PERF_CALLGRAPH  PERF_STAT_REPEAT=$PERF_STAT_REPEAT"
    echo "RUN_PERF_ONCPU=$RUN_PERF_ONCPU  RUN_PERF_STAT=$RUN_PERF_STAT  RUN_OFFCPU=$RUN_OFFCPU"
} | tee "$META_FILE"

SUMMARY_CSV="${RUN_DIR}/summary_wall_time.csv"
# wall_time_s is the DuckDB EXPLAIN ANALYZE "Total Time". Keep the shell
# ".timer on" wall clock separately so query execution is not mixed with
# process/result-printing/cleanup overhead.
echo "query,ssd_mode,mode,threads,wall_time_s,shell_wall_time_s,note" >"$SUMMARY_CSV"
IO_SUMMARY_CSV="${RUN_DIR}/summary_io.csv"
echo "query,ssd_mode,mode,threads,submitted,completed,errors,read_bytes,io_elapsed_s,read_iops,read_mib_s" >"$IO_SUMMARY_CSV"

append_spdk_io_summary() {
    local mode="$1" threads="$2" log="$3"
    local stats
    stats=$(grep '^SPDK_IO_STATS ' "$log" 2>/dev/null | tail -1 || true)
    if [[ -z "$stats" ]]; then
        echo "[warn] SPDK_IO_STATS missing from $log (rebuild build/spdk-release)"
        echo "${query},${ssd_mode},${mode},${threads},NA,NA,NA,NA,NA,NA,NA" >>"$IO_SUMMARY_CSV"
        return
    fi
    python3 - "$query" "$ssd_mode" "$mode" "$threads" "$stats" "$IO_SUMMARY_CSV" <<'PY'
import sys
query, ssd, mode, threads, line, output = sys.argv[1:]
values = dict(token.split("=", 1) for token in line.split()[1:] if "=" in token)
keys = ("submitted", "completed", "errors", "read_bytes", "elapsed_s", "read_iops", "read_mib_s")
with open(output, "a") as f:
    f.write(",".join([query, ssd, mode, threads] + [values.get(k, "NA") for k in keys]) + "\n")
PY
}

# ── 绑定 VFIO ─────────────────────────────────────────────────────────────────

bind_vfio

# trap EXIT 已注册，脚本任何退出路径均会调用 unbind_vfio

# ── 主循环 ────────────────────────────────────────────────────────────────────

for query in $QUERIES; do
    QUERY_SQL_FILE=$(resolve_query_sql "$query") || exit 1
    suite_load_query_content "$QUERY_SQL_FILE"

    for ssd_mode in $SSD_MODES; do
        ssd_count=$(ssd_mode_to_count "$ssd_mode")
        echo ""
        echo "========== 生成 CREATE VIEW（LBA map, ${ssd_count} 盘）=========="
        load_create_view_spdk "$ssd_count" || {
            echo "[error] 无法从 LBA map 生成 CREATE VIEW（ssd_count=$ssd_count prefix=$DATASET_PREFIX）"
            exit 1
        }
        FILE_COUNT=$(python3 -c "
import json
m = json.load(open('$LBA_MAP'))
prefix = '$DATASET_PREFIX'
n = $ssd_count
c = sum(1 for p in m if prefix in p and
        1 <= int(p.split('/data/9a3-')[1].split('/')[0]) <= n)
print(c)
" 2>/dev/null || echo "?")
        echo "  ${FILE_COUNT} 文件"

        for mode in $BUFFER_MODES; do
            for threads in $THREAD_LIST; do
                tag="${query}_${ssd_mode}_${mode}_t${threads}"
                OUT="${RUN_DIR}/${tag}"
                mkdir -p "$OUT"
                echo ""
                echo "========== ${tag} =========="

                # 切换 properties
                suite_apply_buffer_scenario "$mode" || exit 1
                sleep 0.3

                # ── 基准计时 ──────────────────────────────────────────────────
                sql=$(spdk_make_sql "$threads")
                duck_log="${OUT}/duckdb.log"
                tmp_db=$(mktemp -u /tmp/pixels_spdk_XXXXXX.duckdb)
                drop_page_cache

                if "$DUCKDB_BINARY" "$tmp_db" <"$sql" >"$duck_log" 2>&1; then
                    append_summary "$mode" "$threads" "$duck_log"
                    append_spdk_io_summary "$mode" "$threads" "$duck_log"
                    cat "$duck_log"
                else
                    echo "[error] duckdb 执行失败: $tag"
                    cat "$duck_log"
                    rm -f "$tmp_db" "$sql"
                    exit 1
                fi
                rm -f "$tmp_db" "$sql"

                prefix_out="${OUT}/${tag}"

                # ── perf on-CPU 火焰图 ────────────────────────────────────────
                if [[ "$RUN_PERF_ONCPU" == "1" ]]; then
                    echo "--- perf on-CPU ---"
                    suite_apply_buffer_scenario "$mode"
                    sql2=$(spdk_make_sql "$threads")
                    run_perf_oncpu "$prefix_out" "$sql2" \
                        "${query} spdk-e0-${ssd_mode} ${mode} ${threads}t on-CPU"
                    rm -f "$sql2"
                fi

                # ── perf stat ─────────────────────────────────────────────────
                if [[ "$RUN_PERF_STAT" == "1" ]]; then
                    echo "--- perf stat ---"
                    suite_apply_buffer_scenario "$mode"
                    sql3=$(spdk_make_sql "$threads")
                    run_perf_stat "${OUT}/perf_stat.txt" "$sql3"
                    rm -f "$sql3"
                fi

                # ── off-CPU ───────────────────────────────────────────────────
                if [[ "$RUN_OFFCPU" == "1" ]]; then
                    echo "--- off-CPU (bcc) ---"
                    suite_apply_buffer_scenario "$mode"
                    run_offcpu "$OUT" "$threads"
                fi
            done
        done
    done
done

# ── 汇总 ─────────────────────────────────────────────────────────────────────

echo ""
echo "--- Suite complete ---"
echo "Results: $RUN_DIR"
echo "Summary: $SUMMARY_CSV"
cat "$SUMMARY_CSV"
echo "I/O summary: $IO_SUMMARY_CSV"
cat "$IO_SUMMARY_CSV"
echo ""
find "$RUN_DIR" -maxdepth 2 -type f \
    \( -name '*.svg' -o -name 'perf_stat.txt' -o -name 'duckdb.log' -o -name 'summary_io.csv' \) | sort

# EXIT trap 将调用 unbind_vfio 恢复文件系统
