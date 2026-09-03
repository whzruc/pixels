#!/usr/bin/env bash
#
# Run the complete 24-SSD comparison:
#   1. preflight + SPDK LBA map
#   2. SPDK Pixels
#   3. io_uring/pread Pixels
#   4. Parquet io_uring/pread
#   5. comparison reports
#
# A notification is sent after every phase and after the whole workflow.
#
# 126 SMTP example (use a client authorization code, not the web password):
#   export SMTP_USER='whzsxdt@126.com'
#   export SMTP_PASSWORD='your-126-client-authorization-code'
#   bash testcase/performance-test/run_all_comparison.sh
#
# Useful controls:
#   DRY_RUN=1                 Print the workflow without running it.
#   SKIP_LBA_MAP=1            Reuse an existing /tmp/pixels_lba_map.json.
#   SKIP_SPDK=1               Skip the SPDK phase.
#   SKIP_PIXELS=1             Skip the io_uring/pread Pixels phase.
#   SKIP_PARQUET=1            Skip the Parquet phase.
#   SKIP_ANALYSIS=1           Skip report generation.
#   SPDK_BUFFER_MODES=...     SPDK modes (default: spdk spdk-doublebuffer).
#   PIXELS_BUFFER_MODES=...   Pixels modes; use "singlebuffer doublebuffer"
#                             for an io_uring-only comparison.
#   PARQUET_BUFFER_MODES=...  Parquet modes.
#   REQUIRE_EMAIL=1           Require a working mail backend (default: 0).
#   DISABLE_EMAIL=1           Never send email, even if SMTP variables exist.
#   EMAIL_TEST_ONLY=1         Send one test message and exit.
#   EMAIL_TO=...              Override the notification recipient.
#   RUN_STAMP=YYYYmmdd_HHMMSS Reuse predictable suite tags.
#   RESUME=1                  Keep summaries and skip completed individual cases.
#   ENABLE_PERF_PROFILING=1   Enable on-CPU/off-CPU flame graphs and perf stat.
#   KEEP_PERF_DATA=1          Keep perf.data after flame graph generation.

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# sudo normally resets HOME to /root.  Defaults for user-installed Pixels
# files must still refer to the account that invoked this workflow.
INVOKING_HOME="$HOME"
if [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]]; then
    INVOKING_HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
fi
PROPERTIES_PATH="${PROPERTIES_PATH:-$INVOKING_HOME/opt/pixels/etc/pixels-cpp.properties}"
PIXELS_SRC="${PIXELS_SRC:-$(cd "$REPO_ROOT/.." && pwd)}"
PIXELS_HOME="${PIXELS_HOME:-$(cd "$(dirname "$PROPERTIES_PATH")/.." && pwd)}"

EMAIL_TO="${EMAIL_TO:-whzsxdt@126.com}"
SMTP_URL="${SMTP_URL:-smtps://smtp.126.com:465}"
SMTP_USER="${SMTP_USER:-}"
SMTP_PASSWORD="${SMTP_PASSWORD:-}"
SMTP_FROM="${SMTP_FROM:-${SMTP_USER:-$EMAIL_TO}}"
# Email is optional by default. If SMTP credentials or a local mail command are
# available, notifications are sent automatically; otherwise the workflow
# continues and records that notifications were skipped.
REQUIRE_EMAIL="${REQUIRE_EMAIL:-0}"
DISABLE_EMAIL="${DISABLE_EMAIL:-0}"
EMAIL_TEST_ONLY="${EMAIL_TEST_ONLY:-0}"
DRY_RUN="${DRY_RUN:-0}"
RESUME="${RESUME:-0}"

RUN_STAMP="${RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
COMPARISON_TAG="${COMPARISON_TAG:-comparison-total-${RUN_STAMP}}"
SPDK_TAG="${SPDK_TAG:-${COMPARISON_TAG}-spdk}"
PIXELS_TAG="${PIXELS_TAG:-${COMPARISON_TAG}-pixels}"
PARQUET_TAG="${PARQUET_TAG:-${COMPARISON_TAG}-parquet}"
RUN_ROOT="${RUN_ROOT:-$SCRIPT_DIR/comparison-results/$COMPARISON_TAG}"

THREAD_LIST="${THREAD_LIST:-4 8 12 24 48}"
SSD_MODES="${SSD_MODES:-24ssd}"
DROP_CACHES="${DROP_CACHES:-1}"
ENABLE_PIXELS_PROFILER="${ENABLE_PIXELS_PROFILER:-0}"
ENABLE_PERF_PROFILING="${ENABLE_PERF_PROFILING:-0}"
RUN_PERF_ONCPU="${RUN_PERF_ONCPU:-$ENABLE_PERF_PROFILING}"
RUN_PERF_STAT="${RUN_PERF_STAT:-$ENABLE_PERF_PROFILING}"
RUN_OFFCPU="${RUN_OFFCPU:-$ENABLE_PERF_PROFILING}"
KEEP_PERF_DATA="${KEEP_PERF_DATA:-0}"
PERF_RECORD_FREQ="${PERF_RECORD_FREQ:-99}"
PERF_CALLGRAPH="${PERF_CALLGRAPH:-fp}"
PERF_STAT_REPEAT="${PERF_STAT_REPEAT:-2}"
OFFCPU_STACK_SIZE="${OFFCPU_STACK_SIZE:-32768}"
MIN_BLOCK_US="${MIN_BLOCK_US:-1}"
HUGEPAGES="${HUGEPAGES:-32768}"
LBA_MAP="${LBA_MAP:-/tmp/pixels_lba_map.json}"
SPDK_BUFFER_MODES="${SPDK_BUFFER_MODES:-spdk spdk-doublebuffer}"
PIXELS_BUFFER_MODES="${PIXELS_BUFFER_MODES:-pread-singlebuffer pread-doublebuffer singlebuffer doublebuffer}"
PARQUET_BUFFER_MODES="${PARQUET_BUFFER_MODES:-pq-async-singlebuffer pq-async-doublebuffer pq-pread}"

for option_name in ENABLE_PIXELS_PROFILER ENABLE_PERF_PROFILING RUN_PERF_ONCPU \
    RUN_PERF_STAT RUN_OFFCPU KEEP_PERF_DATA; do
    option_value="${!option_name}"
    [[ "$option_value" == "0" || "$option_value" == "1" ]] || {
        echo "[error] $option_name must be 0 or 1" >&2
        exit 1
    }
done

SKIP_LBA_MAP="${SKIP_LBA_MAP:-0}"
SKIP_SPDK="${SKIP_SPDK:-0}"
SKIP_PIXELS="${SKIP_PIXELS:-0}"
SKIP_PARQUET="${SKIP_PARQUET:-0}"
SKIP_ANALYSIS="${SKIP_ANALYSIS:-0}"

COMPARISON_QUERIES="${COMPARISON_QUERIES:-}"
if [[ -z "$COMPARISON_QUERIES" ]]; then
    printf -v COMPARISON_QUERIES 'q%02d ' {0..42}
    COMPARISON_QUERIES="${COMPARISON_QUERIES}q45"
fi

WORKFLOW_LOG_DIR="${WORKFLOW_LOG_DIR:-$RUN_ROOT}"
mkdir -p "$WORKFLOW_LOG_DIR"
WORKFLOW_LOG="$WORKFLOW_LOG_DIR/workflow.log"
SPDK_RESULT_DIR="$RUN_ROOT/$SPDK_TAG"
PIXELS_RESULT_DIR="$RUN_ROOT/$PIXELS_TAG"
PARQUET_RESULT_DIR="$RUN_ROOT/$PARQUET_TAG"
ANALYSIS_RESULT_DIR="$RUN_ROOT/analysis"
START_EPOCH=$(date +%s)
CURRENT_PHASE="startup"
MAIL_BACKEND=""

log() {
    printf '[%s] %s\n' "$(date -Iseconds)" "$*" | tee -a "$WORKFLOW_LOG"
}

format_duration() {
    local seconds="$1"
    printf '%02d:%02d:%02d' "$((seconds / 3600))" "$(((seconds % 3600) / 60))" "$((seconds % 60))"
}

detect_mail_backend() {
    if [[ "$DISABLE_EMAIL" == "1" ]]; then
        MAIL_BACKEND=""
    elif [[ "$DRY_RUN" == "1" ]]; then
        MAIL_BACKEND="dry-run"
    elif [[ -n "$SMTP_USER" && -n "$SMTP_PASSWORD" ]]; then
        MAIL_BACKEND="smtp"
    elif command -v mailx >/dev/null 2>&1; then
        MAIL_BACKEND="mailx"
    elif command -v mail >/dev/null 2>&1; then
        MAIL_BACKEND="mail"
    elif command -v sendmail >/dev/null 2>&1; then
        MAIL_BACKEND="sendmail"
    else
        MAIL_BACKEND=""
    fi

    if [[ -z "$MAIL_BACKEND" && "$REQUIRE_EMAIL" == "1" ]]; then
        cat >&2 <<EOF
[error] no email backend is configured.
For 126 SMTP, export:
  SMTP_USER='whzsxdt@126.com'
  SMTP_PASSWORD='<126 client authorization code>'
Or set REQUIRE_EMAIL=0 to run without notifications.
EOF
        exit 2
    fi

    if [[ "$MAIL_BACKEND" == "smtp" ]]; then
        if [[ "$SMTP_USER" == *[[:space:][:cntrl:]]* ]]; then
            echo "[error] SMTP_USER contains whitespace or control characters; re-enter it." >&2
            exit 2
        fi
        if [[ "$SMTP_PASSWORD" == *[[:space:][:cntrl:]]* ]]; then
            cat >&2 <<'EOF'
[error] SMTP_PASSWORD contains whitespace or control characters.
Clear and re-enter the 126 client authorization code:
  unset SMTP_PASSWORD
  IFS= read -r -s -p '126客户端授权码: ' SMTP_PASSWORD
  echo
  export SMTP_PASSWORD
Then verify it without running benchmarks:
  EMAIL_TEST_ONLY=1 bash testcase/performance-test/run_all_comparison.sh
EOF
            exit 2
        fi
    fi
}

send_email() {
    local subject="$1"
    local body="$2"

    if [[ -z "$MAIL_BACKEND" ]]; then
        log "[mail] skipped: no backend configured"
        return 0
    fi
    if [[ "$MAIL_BACKEND" == "dry-run" ]]; then
        log "[mail dry-run] to=$EMAIL_TO subject=$subject"
        return 0
    fi

    case "$MAIL_BACKEND" in
        mailx)
            printf '%s\n' "$body" | mailx -s "$subject" "$EMAIL_TO"
            ;;
        mail)
            printf '%s\n' "$body" | mail -s "$subject" "$EMAIL_TO"
            ;;
        sendmail)
            {
                printf 'From: %s\n' "$SMTP_FROM"
                printf 'To: %s\n' "$EMAIL_TO"
                printf 'Subject: %s\n' "$subject"
                printf 'Content-Type: text/plain; charset=UTF-8\n\n'
                printf '%s\n' "$body"
            } | sendmail -t
            ;;
        smtp)
            local mail_file netrc_file
            mail_file=$(mktemp /tmp/pixels_mail_XXXXXX.txt)
            netrc_file=$(mktemp /tmp/pixels_netrc_XXXXXX)
            chmod 600 "$mail_file" "$netrc_file"
            {
                printf 'From: %s\n' "$SMTP_FROM"
                printf 'To: %s\n' "$EMAIL_TO"
                printf 'Subject: %s\n' "$subject"
                printf 'Date: %s\n' "$(LC_ALL=C date -R)"
                printf 'Content-Type: text/plain; charset=UTF-8\n'
                printf 'Content-Transfer-Encoding: 8bit\n\n'
                printf '%s\n' "$body"
            } >"$mail_file"
            printf 'machine smtp.126.com login %s password %s\n' \
                "$SMTP_USER" "$SMTP_PASSWORD" >"$netrc_file"
            if ! curl --silent --show-error --fail \
                --url "$SMTP_URL" --ssl-reqd \
                --netrc-file "$netrc_file" \
                --mail-from "$SMTP_FROM" --mail-rcpt "$EMAIL_TO" \
                --upload-file "$mail_file"; then
                rm -f "$mail_file" "$netrc_file"
                log "[mail] SMTP delivery failed"
                return 1
            fi
            rm -f "$mail_file" "$netrc_file"
            ;;
    esac
    log "[mail] sent to $EMAIL_TO: $subject"
}

run_cmd() {
    if [[ "$DRY_RUN" == "1" ]]; then
        printf '[dry-run]'
        printf ' %q' "$@"
        printf '\n'
        return 0
    fi
    "$@"
}

run_phase() {
    local phase="$1"
    shift
    CURRENT_PHASE="$phase"
    local phase_start phase_end rc
    phase_start=$(date +%s)
    log "===== phase start: $phase ====="
    set +e
    "$@" 2>&1 | tee -a "$WORKFLOW_LOG"
    rc=${PIPESTATUS[0]}
    set -e
    phase_end=$(date +%s)
    if [[ "$rc" -eq 0 ]]; then
        log "===== phase success: $phase ($(format_duration "$((phase_end - phase_start))")) ====="
        send_email \
            "[Pixels benchmark] SUCCESS: $phase" \
            "Phase: $phase
Status: SUCCESS
Host: $(hostname)
Finished: $(date -Iseconds)
Duration: $(format_duration "$((phase_end - phase_start))")
Run stamp: $RUN_STAMP
Log: $WORKFLOW_LOG"
    else
        log "===== phase failed: $phase rc=$rc ($(format_duration "$((phase_end - phase_start))")) ====="
        send_email \
            "[Pixels benchmark] FAILED: $phase" \
            "Phase: $phase
Status: FAILED
Exit code: $rc
Host: $(hostname)
Finished: $(date -Iseconds)
Duration: $(format_duration "$((phase_end - phase_start))")
Run stamp: $RUN_STAMP
Log: $WORKFLOW_LOG" || true
        return "$rc"
    fi
}

preflight() {
    cd "$REPO_ROOT"
    [[ -f testcase/benchmark.json ]]

    if [[ "$SKIP_SPDK" != "1" ]]; then
        [[ -x build/spdk-release/duckdb ]] || {
            log "[error] SPDK binary missing: build/spdk-release/duckdb"
            return 1
        }
    fi
    if [[ "$SKIP_PIXELS" != "1" || "$SKIP_PARQUET" != "1" ]]; then
        [[ -x build/release/duckdb.bin ]] || {
            log "[error] io_uring/pread binary missing: build/release/duckdb.bin"
            return 1
        }
    fi

    local mounted=0
    local i mountpoint_path
    for i in $(seq -w 1 24); do
        mountpoint_path="/data/9a3-$i"
        if mountpoint -q "$mountpoint_path"; then
            mounted=$((mounted + 1))
        fi
    done
    [[ "$mounted" -eq 24 ]] || {
        log "[error] expected 24 mounted data disks, found $mounted"
        return 1
    }

    if [[ "$SKIP_SPDK" != "1" ]]; then
        local current_hp
        current_hp=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
        if [[ "$current_hp" -lt "$HUGEPAGES" ]]; then
            log "[hugepages] increasing $current_hp -> $HUGEPAGES"
            run_cmd sudo sh -c "echo '$HUGEPAGES' > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
        fi
    else
        log "[preflight] SPDK skipped: no hugepage setup, VFIO binding, or LBA map required"
    fi
}

generate_lba_map() {
    cd "$REPO_ROOT"
    if [[ "$SKIP_LBA_MAP" == "1" ]]; then
        [[ -s "$LBA_MAP" ]]
        log "[lba-map] reusing $LBA_MAP"
        return
    fi
    run_cmd python3 testcase/spdk/gen_lba_map.py \
        --output "$LBA_MAP" \
        /data/9a3-{01..24}/clickbench/pixels-e0-fb || return
    [[ "$DRY_RUN" == "1" || -s "$LBA_MAP" ]]
}

run_spdk() {
    cd "$REPO_ROOT"
    run_cmd sudo -E env \
        "PIXELS_SRC=$PIXELS_SRC" \
        "PIXELS_HOME=$PIXELS_HOME" \
        "PROPERTIES_PATH=$PROPERTIES_PATH" \
        "QUERIES=$COMPARISON_QUERIES" \
        "SSD_MODES=$SSD_MODES" \
        "BUFFER_MODES=$SPDK_BUFFER_MODES" \
        "THREAD_LIST=$THREAD_LIST" \
        "RESUME=$RESUME" \
        "RUN_PERF_ONCPU=$RUN_PERF_ONCPU" \
        "RUN_PERF_STAT=$RUN_PERF_STAT" \
        "RUN_OFFCPU=$RUN_OFFCPU" \
        "KEEP_PERF_DATA=$KEEP_PERF_DATA" \
        "PERF_RECORD_FREQ=$PERF_RECORD_FREQ" \
        "PERF_CALLGRAPH=$PERF_CALLGRAPH" \
        "PERF_STAT_REPEAT=$PERF_STAT_REPEAT" \
        "OFFCPU_STACK_SIZE=$OFFCPU_STACK_SIZE" \
        "MIN_BLOCK_US=$MIN_BLOCK_US" \
        "ENABLE_PIXELS_PROFILER=$ENABLE_PIXELS_PROFILER" \
        "DROP_CACHES=$DROP_CACHES" \
        "LBA_MAP=$LBA_MAP" \
        "RESULT_ROOT=$RUN_ROOT" \
        "SUITE_TAG=$SPDK_TAG" \
        bash testcase/performance-test/run_suite_spdk.sh
}

verify_mounts_after_spdk() {
    local i
    for i in $(seq -w 1 24); do
        mountpoint -q "/data/9a3-$i" || {
            log "[error] /data/9a3-$i was not restored after SPDK"
            return 1
        }
    done
}

run_pixels() {
    cd "$REPO_ROOT"
    run_cmd sudo -E env \
        "PIXELS_SRC=$PIXELS_SRC" \
        "PIXELS_HOME=$PIXELS_HOME" \
        "PROPERTIES_PATH=$PROPERTIES_PATH" \
        "QUERIES=$COMPARISON_QUERIES" \
        "SSD_MODES=$SSD_MODES" \
        "BUFFER_MODES=$PIXELS_BUFFER_MODES" \
        "THREAD_LIST=$THREAD_LIST" \
        "RESUME=$RESUME" \
        "RUN_PERF_ONCPU=$RUN_PERF_ONCPU" \
        "RUN_PERF_STAT=$RUN_PERF_STAT" \
        "RUN_OFFCPU=$RUN_OFFCPU" "RUN_IOSTAT=0" \
        "KEEP_PERF_DATA=$KEEP_PERF_DATA" \
        "PERF_RECORD_FREQ=$PERF_RECORD_FREQ" \
        "PERF_CALLGRAPH=$PERF_CALLGRAPH" \
        "PERF_STAT_REPEAT=$PERF_STAT_REPEAT" \
        "OFFCPU_STACK_SIZE=$OFFCPU_STACK_SIZE" \
        "MIN_BLOCK_US=$MIN_BLOCK_US" \
        "ENABLE_PIXELS_PROFILER=$ENABLE_PIXELS_PROFILER" \
        "DROP_CACHES=$DROP_CACHES" \
        "RESULT_ROOT=$RUN_ROOT" \
        "SUITE_TAG=$PIXELS_TAG" \
        bash testcase/performance-test/run_suite.sh
}

run_parquet() {
    cd "$REPO_ROOT"
    run_cmd sudo -E env \
        "PIXELS_SRC=$PIXELS_SRC" \
        "PIXELS_HOME=$PIXELS_HOME" \
        "PROPERTIES_PATH=$PROPERTIES_PATH" \
        "QUERIES=$COMPARISON_QUERIES" \
        "SSD_MODES=$SSD_MODES" \
        "BUFFER_MODES=$PARQUET_BUFFER_MODES" \
        "THREAD_LIST=$THREAD_LIST" \
        "RESUME=$RESUME" \
        "RUN_PERF_ONCPU=$RUN_PERF_ONCPU" \
        "RUN_PERF_STAT=$RUN_PERF_STAT" \
        "RUN_OFFCPU=$RUN_OFFCPU" "RUN_IOSTAT=0" \
        "KEEP_PERF_DATA=$KEEP_PERF_DATA" \
        "PERF_RECORD_FREQ=$PERF_RECORD_FREQ" \
        "PERF_CALLGRAPH=$PERF_CALLGRAPH" \
        "PERF_STAT_REPEAT=$PERF_STAT_REPEAT" \
        "OFFCPU_STACK_SIZE=$OFFCPU_STACK_SIZE" \
        "MIN_BLOCK_US=$MIN_BLOCK_US" \
        "ENABLE_PIXELS_PROFILER=$ENABLE_PIXELS_PROFILER" \
        "DROP_CACHES=$DROP_CACHES" \
        "RESULT_ROOT=$RUN_ROOT" \
        "SUITE_TAG=$PARQUET_TAG" \
        bash testcase/performance-test/run_suite_parquet_uring.sh
}

run_analysis() {
    cd "$REPO_ROOT"
    if [[ "$SKIP_SPDK" != "1" && "$SKIP_PIXELS" != "1" ]]; then
        run_cmd env MPLCONFIGDIR=/tmp/mplconfig-buffer-analysis \
            python3 testcase/performance-test/comparison_spdk_20260715_vs_iouring_20260716/analyze.py \
            --spdk-run "$SPDK_RESULT_DIR" \
            --iouring-run "$PIXELS_RESULT_DIR" \
            --output-dir "$ANALYSIS_RESULT_DIR"
    else
        log "[analysis] SPDK/Pixels comparison skipped because an input phase was skipped"
    fi
    if [[ "$SKIP_PARQUET" != "1" ]]; then
        run_cmd python3 testcase/performance-test/parse_results.py \
            "$PARQUET_RESULT_DIR"
    else
        log "[analysis] Parquet summary skipped because the Parquet phase was skipped"
    fi
}

main() {
    detect_mail_backend
    log "workflow stamp=$RUN_STAMP mail_backend=${MAIL_BACKEND:-none}"
    log "SPDK_TAG=$SPDK_TAG PIXELS_TAG=$PIXELS_TAG PARQUET_TAG=$PARQUET_TAG"
    log "all results directory=$RUN_ROOT"
    log "profiling pixels=$ENABLE_PIXELS_PROFILER perf=$ENABLE_PERF_PROFILING oncpu=$RUN_PERF_ONCPU stat=$RUN_PERF_STAT offcpu=$RUN_OFFCPU keep_perf_data=$KEEP_PERF_DATA"
    log "modes spdk=[$SPDK_BUFFER_MODES] pixels=[$PIXELS_BUFFER_MODES] parquet=[$PARQUET_BUFFER_MODES]"

    if [[ "$EMAIL_TEST_ONLY" == "1" ]]; then
        send_email \
            "[Pixels benchmark] EMAIL TEST: $RUN_STAMP" \
            "Email notification test succeeded.
Host: $(hostname)
Time: $(date -Iseconds)
Recipient: $EMAIL_TO"
        log "email test completed; no benchmark was started"
        return 0
    fi

    run_phase "preflight" preflight

    if [[ "$SKIP_SPDK" != "1" ]]; then
        run_phase "LBA map generation" generate_lba_map
        run_phase "SPDK Pixels benchmark" run_spdk
        run_phase "post-SPDK mount verification" verify_mounts_after_spdk
    else
        log "[SPDK] skipped: LBA map generation, hugepage setup, VFIO workflow, and mount restoration are not needed"
    fi
    if [[ "$SKIP_PIXELS" != "1" ]]; then
        run_phase "io_uring and pread Pixels benchmark" run_pixels
    fi
    if [[ "$SKIP_PARQUET" != "1" ]]; then
        run_phase "Parquet benchmark" run_parquet
    fi
    if [[ "$SKIP_ANALYSIS" != "1" ]]; then
        run_phase "report generation" run_analysis
    fi

    local end_epoch
    end_epoch=$(date +%s)
    CURRENT_PHASE="complete"
    send_email \
        "[Pixels benchmark] ALL COMPLETE: $RUN_STAMP" \
        "Status: ALL PHASES COMPLETE
Host: $(hostname)
Finished: $(date -Iseconds)
Total duration: $(format_duration "$((end_epoch - START_EPOCH))")
All results: $RUN_ROOT
SPDK results: $SPDK_RESULT_DIR
Pixels results: $PIXELS_RESULT_DIR
Parquet results: $PARQUET_RESULT_DIR
Analysis results: $ANALYSIS_RESULT_DIR
Workflow log: $WORKFLOW_LOG"
    log "all requested phases completed in $(format_duration "$((end_epoch - START_EPOCH))")"
}

main "$@"
