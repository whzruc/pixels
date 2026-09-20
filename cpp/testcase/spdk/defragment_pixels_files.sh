#!/usr/bin/env bash
# Defragment Pixels files, verify that every file has one extent, then
# regenerate the SPDK LBA map while the filesystems are still mounted.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
LBA_MAP="${LBA_MAP:-/tmp/pixels_lba_map.json}"
SCAN_ONLY=0
SKIP_LBA_MAP=0
DATA_DIRS=()

usage() {
    cat <<'EOF'
Usage: sudo bash testcase/spdk/defragment_pixels_files.sh [options] [DATA_DIR ...]

By default, processes:
  /data/9a3-01/clickbench/pixels-e0-fb ... /data/9a3-24/clickbench/pixels-e0-fb

Options:
  --scan-only       Only scan and report fragmented files; make no changes.
  --skip-lba-map    Defragment and verify, but do not regenerate the LBA map.
  --lba-map PATH    Write the LBA map to PATH (default: /tmp/pixels_lba_map.json).
  -h, --help        Show this help.

Reports are written to:
  /tmp/pixels_fragmented_files.txt
  /tmp/pixels_still_fragmented.txt
EOF
}

while (($#)); do
    case "$1" in
        --scan-only)
            SCAN_ONLY=1
            shift
            ;;
        --skip-lba-map)
            SKIP_LBA_MAP=1
            shift
            ;;
        --lba-map)
            [[ $# -ge 2 ]] || { echo "[error] --lba-map requires a path" >&2; exit 2; }
            LBA_MAP=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            DATA_DIRS+=("$@")
            break
            ;;
        -*)
            echo "[error] unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            DATA_DIRS+=("$1")
            shift
            ;;
    esac
done

if [[ $EUID -ne 0 ]]; then
    echo "[error] run as root: sudo bash $0" >&2
    exit 1
fi

for tool in findmnt find filefrag xfs_fsr python3 awk; do
    command -v "$tool" >/dev/null || {
        echo "[error] required command not found: $tool" >&2
        exit 1
    }
done

if ((${#DATA_DIRS[@]} == 0)); then
    for number in $(seq -w 1 24); do
        DATA_DIRS+=("/data/9a3-$number/clickbench/pixels-e0-fb")
    done
fi

for directory in "${DATA_DIRS[@]}"; do
    [[ -d "$directory" ]] || {
        echo "[error] data directory is missing: $directory" >&2
        exit 1
    }
    filesystem=$(findmnt -n -o FSTYPE --target "$directory")
    [[ "$filesystem" == "xfs" ]] || {
        echo "[error] $directory is on $filesystem, not XFS" >&2
        exit 1
    }
done

extent_count() {
    local file=$1
    LC_ALL=C filefrag "$file" |
        awk '/extent found|extents found/ { count=$(NF-2) } END { print count }'
}

scan_fragmented() {
    local output=$1
    local file extents files=0 fragmented=0
    : > "$output"

    while IFS= read -r -d '' file; do
        files=$((files + 1))
        extents=$(extent_count "$file")
        if [[ ! "$extents" =~ ^[0-9]+$ ]]; then
            echo "[error] could not determine extent count: $file" >&2
            return 1
        fi
        if ((extents != 1)); then
            fragmented=$((fragmented + 1))
            printf '%s\n' "$file" >> "$output"
            printf '[fragmented] %4d extents  %s\n' "$extents" "$file"
        fi
        if ((files % 500 == 0)); then
            echo "[scan] checked $files files; found $fragmented fragmented"
        fi
    done < <(find "${DATA_DIRS[@]}" -type f -name '*.pxl' -print0)

    if ((files == 0)); then
        echo "[error] no .pxl files found" >&2
        return 1
    fi
    echo "[scan] checked $files files; found $fragmented fragmented"
}

FRAGMENTED_LIST=/tmp/pixels_fragmented_files.txt
REMAINING_LIST=/tmp/pixels_still_fragmented.txt

echo "[step 1/4] Scanning Pixels files"
scan_fragmented "$FRAGMENTED_LIST"

fragmented_count=$(wc -l < "$FRAGMENTED_LIST")
if ((SCAN_ONLY)); then
    echo "[done] scan-only mode; list: $FRAGMENTED_LIST"
    ((fragmented_count == 0))
    exit
fi

echo "[step 2/4] Defragmenting $fragmented_count files"
current=0
failed=0
while IFS= read -r file; do
    current=$((current + 1))
    echo "[xfs_fsr $current/$fragmented_count] $file"
    if ! xfs_fsr "$file"; then
        echo "[warning] xfs_fsr failed: $file" >&2
        failed=$((failed + 1))
    fi
done < "$FRAGMENTED_LIST"
echo "[xfs_fsr] command failures: $failed"

echo "[step 3/4] Verifying every Pixels file has exactly one extent"
scan_fragmented "$REMAINING_LIST"
remaining_count=$(wc -l < "$REMAINING_LIST")
if ((remaining_count != 0)); then
    echo "[error] $remaining_count files still do not have exactly one extent" >&2
    echo "[error] see $REMAINING_LIST" >&2
    echo "[hint] free space on the affected filesystem and run this script again" >&2
    exit 1
fi

if ((SKIP_LBA_MAP)); then
    echo "[done] all files have one extent; LBA map generation was skipped"
    exit 0
fi

echo "[step 4/4] Generating SPDK LBA map: $LBA_MAP"
python3 "$REPO_ROOT/testcase/spdk/gen_lba_map.py" \
    --output "$LBA_MAP" \
    "${DATA_DIRS[@]}"
[[ -s "$LBA_MAP" ]] || {
    echo "[error] LBA map was not created: $LBA_MAP" >&2
    exit 1
}

echo "[done] every .pxl file has one extent"
echo "[done] LBA map generated: $LBA_MAP"
echo "[next] run the benchmark with SKIP_LBA_MAP=1 to reuse this verified map"
