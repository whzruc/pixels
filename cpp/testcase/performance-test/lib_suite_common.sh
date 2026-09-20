#!/bin/bash
# Shared helpers for buffer-perf-concurrency-suite (sourced by run_suite.sh).
# Buffer scenarios match run_perf_four_modes.py TEST_SCENARIOS exactly.

suite_resolve_flamegraph_dir() {
    local repo="${SUITE_REPO_ROOT:?}"
    FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-}"
    if [[ -z $FLAMEGRAPH_DIR ]]; then
        for d in "$repo/third-party/FlameGraph" "$HOME/FlameGraph"; do
            if [[ -f $d/stackcollapse-perf.pl ]] && [[ -f $d/flamegraph.pl ]]; then
                FLAMEGRAPH_DIR=$d
                break
            fi
        done
        FLAMEGRAPH_DIR=${FLAMEGRAPH_DIR:-$HOME/FlameGraph}
    fi
    STACKCOLLAPSE_PERF="${FLAMEGRAPH_DIR}/stackcollapse-perf.pl"
    FLAMEGRAPH_PL="${FLAMEGRAPH_DIR}/flamegraph.pl"
}

suite_ensure_iouring_selected() {
    [[ -f "$PROPERTIES_PATH" ]] || {
        echo "[error] PROPERTIES_PATH missing: $PROPERTIES_PATH"
        return 1
    }
    python3 - "$PROPERTIES_PATH" <<'PY'
import sys
path = sys.argv[1]
key, val = "localfs.async.lib", "iouring"
with open(path) as f:
    lines = f.readlines()
new_lines, changed = [], False
for line in lines:
    if line.strip().startswith(key):
        new_lines.append(f"{key}={val}\n")
        changed = True
    else:
        new_lines.append(line)
if not changed:
    new_lines.append(f"{key}={val}\n")
with open(path, "w") as f:
    f.writelines(new_lines)
PY
}

# All five scenarios from run_perf_four_modes.py TEST_SCENARIOS
suite_apply_buffer_scenario() {
    local mode="$1"
    [[ -f "$PROPERTIES_PATH" ]] || return 1
    python3 - "$PROPERTIES_PATH" "$mode" <<'PY'
import os
import sys
path, mode = sys.argv[1], sys.argv[2]
SCENARIOS = {
    # async io_uring + fixed buffer + global static (no double-buffer)
    "singlebuffer": {
        "localfs.iouring.use.fixed.buffer": "true",
        "pixel.enable.globalStaticBytebuffer": "true",
        "localfs.enable.async.io": "true",
        "pixels.doublebuffer": "false",
    },
    # alias: same as singlebuffer
    "fixed": {
        "localfs.iouring.use.fixed.buffer": "true",
        "pixel.enable.globalStaticBytebuffer": "true",
        "localfs.enable.async.io": "true",
        "pixels.doublebuffer": "false",
    },
    # async io_uring + non-fixed buffer + global static
    "nonfixed": {
        "localfs.iouring.use.fixed.buffer": "false",
        "pixel.enable.globalStaticBytebuffer": "true",
        "localfs.enable.async.io": "true",
        "pixels.doublebuffer": "false",
    },
    # sync pread + no global static + no double-buffer
    "pread-singlebuffer": {
        "localfs.iouring.use.fixed.buffer": "false",
        "pixel.enable.globalStaticBytebuffer": "false",
        "localfs.enable.async.io": "false",
        "pixels.doublebuffer": "false",
    },
    # sync pread + no global static + double-buffer
    "pread-doublebuffer": {
        "localfs.iouring.use.fixed.buffer": "false",
        "pixel.enable.globalStaticBytebuffer": "false",
        "localfs.enable.async.io": "false",
        "pixels.doublebuffer": "true",
    },
    # async io_uring + fixed buffer + global static + double-buffer
    "doublebuffer": {
        "pixel.enable.globalStaticBytebuffer": "true",
        "localfs.iouring.use.fixed.buffer": "true",
        "localfs.enable.async.io": "true",
        "pixels.doublebuffer": "true",
    },
    # ── read_parquet_uring modes ──────────────────────────────────────────
    # read_parquet_uring manages its own io_uring ring + buffers; it does NOT
    # use GlobalStaticBufferPool. Only localfs.enable.async.io and
    # pixels.doublebuffer matter. pixel.enable.globalStaticBytebuffer is set
    # to false so the heavy pixels-format init is skipped entirely.
    "pq-async-doublebuffer": {
        "localfs.enable.async.io": "true",
        "pixels.doublebuffer": "true",
        "localfs.iouring.use.fixed.buffer": "true",
        "pixel.enable.globalStaticBytebuffer": "false",
    },
    "pq-async-singlebuffer": {
        "localfs.enable.async.io": "true",
        "pixels.doublebuffer": "false",
        "localfs.iouring.use.fixed.buffer": "true",
        "pixel.enable.globalStaticBytebuffer": "false",
    },
    "pq-pread": {
        "localfs.enable.async.io": "false",
        "pixels.doublebuffer": "false",
        "localfs.iouring.use.fixed.buffer": "false",
        "pixel.enable.globalStaticBytebuffer": "false",
    },
    # ── SPDK user-space NVMe modes ────────────────────────────────────────
    # Requires: VFIO-bound NVMe devices (bind_vfio.sh), hugepages,
    #           localfs.enable.spdk=true in properties,
    #           SPDK-enabled binary (make spdk-release).
    # GlobalStaticBufferPool is NOT used; SPDK manages its own DMA buffers.
    "spdk": {
        "localfs.async.lib": "spdk",
        "localfs.enable.spdk": "true",
        "localfs.enable.async.io": "true",
        "pixel.enable.globalStaticBytebuffer": "false",
        "pixels.doublebuffer": "false",
    },
    "spdk-doublebuffer": {
        "localfs.async.lib": "spdk",
        "localfs.enable.spdk": "true",
        "localfs.enable.async.io": "true",
        "pixel.enable.globalStaticBytebuffer": "false",
        "pixels.doublebuffer": "true",
    },
}
if mode not in SCENARIOS:
    sys.exit(f"unknown buffer mode: {mode}. valid: {', '.join(SCENARIOS)}")
configs = dict(SCENARIOS[mode])
configs["pixel.enable.profiler"] = (
    "true" if os.environ.get("ENABLE_PIXELS_PROFILER", "0") == "1" else "false"
)
with open(path) as f:
    lines = f.readlines()
modified = set()
new_lines = []
for line in lines:
    hit = False
    for param_name, param_value in configs.items():
        if line.strip().startswith(param_name):
            new_lines.append(f"{param_name}={param_value}\n")
            modified.add(param_name)
            hit = True
            break
    if not hit:
        new_lines.append(line)
for param_name, param_value in configs.items():
    if param_name not in modified:
        new_lines.append(f"{param_name}={param_value}\n")
with open(path, "w") as f:
    f.writelines(new_lines)
print(
    f"[config] {mode} applied, pixel.enable.profiler="
    f"{configs['pixel.enable.profiler']} → pixels-cpp.properties"
)
PY
}

suite_load_create_view_sql() {
    local bench_json="$1"
    local benchmark="$2"
    CREATE_VIEW_SQL=$(python3 -c "
import json, sys
d = json.load(open('$bench_json'))
if '$benchmark' not in d:
    print('Error: benchmark not in json', file=sys.stderr)
    sys.exit(1)
print(d['$benchmark'].strip())
") || return 1
}

suite_load_query_content() {
    local qfile="$1"
    QUERY_CONTENT=$(cat "$qfile")
}

suite_prepare_temp_db_and_sql() {
    local threads="$1"
    local temp_db
    temp_db=$(mktemp -u /tmp/pixels_suite_XXXXXX.duckdb)
    printf '%s\nset threads=%s;\n.exit\n' "$CREATE_VIEW_SQL" "$threads" \
        | "$DUCKDB_BINARY" "$temp_db" >/dev/null 2>&1 || {
        rm -f "$temp_db"
        return 1
    }
    local tmp_sql
    tmp_sql=$(mktemp /tmp/pixels_suite_query_XXXXXX.sql)
    printf 'set threads=%s;\n.timer on\nexplain analyze %s\n.exit\n' \
        "$threads" "$QUERY_CONTENT" >"$tmp_sql"
    echo "$temp_db|$tmp_sql"
}

suite_cleanup_temp_pair() {
    local pair="$1"
    local db sql
    db="${pair%%|*}"
    sql="${pair##*|}"
    rm -f "$db" "$sql"
}
