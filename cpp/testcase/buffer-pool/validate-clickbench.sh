#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "Usage: $0 COLUMN_SIZE_FILE PIXELS_GLOB [THREADS]" >&2
    exit 2
fi

column_size_file=$1
pixels_glob=$2
threads=${3:-1}
cpp_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
repo_root=$(cd "$cpp_root/.." && pwd)
duckdb_bin="$cpp_root/build/release/duckdb"
work_dir=$(mktemp -d /tmp/pixels-buffer-pool-validation.XXXXXX)
trap 'rm -rf "$work_dir"' EXIT

if [[ ! -x "$duckdb_bin" ]]; then
    echo "DuckDB executable not found: $duckdb_bin" >&2
    exit 1
fi

sql="PRAGMA threads=$threads;
CREATE VIEW hits AS SELECT * FROM pixels_scan(['$pixels_glob']);
SELECT count(*) FROM hits;
SELECT count(*) FROM hits WHERE AdvEngineID <> 0;
SELECT sum(AdvEngineID), count(*), avg(AdvEngineID) FROM hits;"

for mode in legacy non-fixed dynamic static; do
    pixels_home="$work_dir/$mode"
    mkdir -p "$pixels_home/cpp/etc"
    cp "$cpp_root/etc/pixels-cpp.properties" "$pixels_home/cpp/etc/pixels-cpp.properties"
    sed -i \
        -e "s|^pixel.bufferpool.mode=.*|pixel.bufferpool.mode=$mode|" \
        -e "s|^pixel.column.size.path=.*|pixel.column.size.path=$column_size_file|" \
        -e "s|^pixel.static.buffer.threads=.*|pixel.static.buffer.threads=$threads|" \
        "$pixels_home/cpp/etc/pixels-cpp.properties"
    PIXELS_SRC="$repo_root" PIXELS_HOME="$pixels_home" \
        "$duckdb_bin" -csv -noheader -c "$sql" | tail -n 3 > "$work_dir/$mode.out"
done

diff -u "$work_dir/legacy.out" "$work_dir/dynamic.out"
diff -u "$work_dir/legacy.out" "$work_dir/static.out"
diff -u "$work_dir/legacy.out" "$work_dir/non-fixed.out"
cat "$work_dir/legacy.out"
echo "legacy, non-fixed, dynamic, and static results match"
