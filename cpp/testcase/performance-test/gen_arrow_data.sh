#!/bin/bash
#
# gen_arrow_data.sh — 将 Parquet 文件转换为 Arrow IPC 格式，写入 24 块盘
#
# 用法：
#   ./gen_arrow_data.sh [--ssds "01 02 ..."] [--jobs N]
#
# 默认并发数 = SSD 数（每块盘一个 Python 进程），每个进程内部串行转换 640 个文件。
# 已存在的 .arrow 文件自动跳过（幂等）。
#
# 输入：/data/9a3-{NN}/clickbench/parquet-e0/hits/*.parquet
# 输出：/data/9a3-{NN}/clickbench/arrow-e0/hits/*.arrow
#
# 依赖：python3 + pyarrow

set -euo pipefail

PARQUET_SUBDIR="clickbench/parquet-e0/hits"
ARROW_SUBDIR="clickbench/arrow-e0/hits"
JOBS=24

# 默认处理全部 24 块盘
SSD_LIST=(01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ssds) read -ra SSD_LIST <<< "$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 1 ;;
    esac
done

echo "[gen_arrow_data] SSDs: ${SSD_LIST[*]}"
echo "[gen_arrow_data] parallel jobs: $JOBS"
echo ""

# Python 转换脚本（内联，由 bash xargs 调用）
PY_CONVERT=$(cat <<'PYEOF'
import sys, os
import pyarrow as pa
import pyarrow.parquet as pq
import pyarrow.ipc as ipc
from pathlib import Path

ssd = sys.argv[1]
parquet_subdir = sys.argv[2]
arrow_subdir   = sys.argv[3]

src = Path(f"/data/9a3-{ssd}/{parquet_subdir}")
dst = Path(f"/data/9a3-{ssd}/{arrow_subdir}")
dst.mkdir(parents=True, exist_ok=True)

pfiles = sorted(src.glob("*.parquet"))
if not pfiles:
    print(f"  [{ssd}] WARNING: no parquet files in {src}", flush=True)
    sys.exit(0)

errors = []
for i, pfile in enumerate(pfiles):
    afile = dst / (pfile.stem + ".arrow")
    if afile.exists() and afile.stat().st_size > 0:
        continue
    try:
        reader = pq.ParquetFile(str(pfile))
        schema = reader.schema_arrow
        tmp = str(afile) + ".tmp"
        with ipc.new_file(tmp, schema) as writer:
            for batch in reader.iter_batches():
                writer.write_batch(batch)
        os.replace(tmp, str(afile))
        if i % 64 == 0:
            print(f"  [{ssd}] {i+1}/{len(pfiles)}: {pfile.name}", flush=True)
    except Exception as e:
        errors.append(f"{pfile.name}: {e}")

if errors:
    print(f"  [{ssd}] ERRORS:", flush=True)
    for e in errors:
        print(f"    {e}", flush=True)
    sys.exit(1)

print(f"  [{ssd}] done — {len(pfiles)} files → {dst}", flush=True)
PYEOF
)

# 并行执行：每块盘一个 Python 进程
printf '%s\n' "${SSD_LIST[@]}" | \
    xargs -P "$JOBS" -I{} \
    python3 -c "$PY_CONVERT" {} "$PARQUET_SUBDIR" "$ARROW_SUBDIR"

echo ""
echo "[gen_arrow_data] all done"

# 打印各盘文件数和总大小
echo ""
echo "=== Summary ==="
total_files=0
total_gb=0
for ssd in "${SSD_LIST[@]}"; do
    d="/data/9a3-${ssd}/${ARROW_SUBDIR}"
    if [[ -d "$d" ]]; then
        n=$(ls "$d"/*.arrow 2>/dev/null | wc -l)
        sz=$(du -sh "$d" 2>/dev/null | cut -f1)
        echo "  9a3-${ssd}: $n files, $sz"
        total_files=$((total_files + n))
    else
        echo "  9a3-${ssd}: MISSING"
    fi
done
echo "  Total files: $total_files"
