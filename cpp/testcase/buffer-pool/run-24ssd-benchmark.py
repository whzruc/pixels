#!/usr/bin/env python3

import argparse
import csv
import datetime
import hashlib
import json
import os
import platform
import random
import re
import shutil
import subprocess
import time
from pathlib import Path

MODES = ("legacy", "dynamic", "static", "static-hugepage")
EVENTS = (
    "cycles,instructions,cache-references,cache-misses,"
    "dTLB-loads,dTLB-load-misses,page-faults,minor-faults,major-faults,"
    "context-switches,cpu-migrations,task-clock"
)
BUFFER_STATS_FIELDS = (
    "current_allocated_bytes", "peak_allocated_bytes", "current_registered_bytes",
    "peak_registered_bytes", "allocations", "frees", "registrations",
    "registration_updates", "reuses", "growths"
)


def replace_property(text, key, value):
    pattern = rf"(?m)^{re.escape(key)}=.*$"
    replacement = f"{key}={value}"
    if re.search(pattern, text):
        return re.sub(pattern, replacement, text)
    return text.rstrip() + "\n" + replacement + "\n"


def prepare_home(root, base_config, mode, threads, column_sizes, enable_buffer_stats=False):
    home = root / "homes" / f"{mode}-t{threads}"
    config_path = home / "cpp" / "etc" / "pixels-cpp.properties"
    config_path.parent.mkdir(parents=True, exist_ok=True)
    text = base_config.read_text()
    pool_mode = "static" if mode == "static-hugepage" else mode
    text = replace_property(text, "pixel.bufferpool.mode", pool_mode)
    text = replace_property(text, "pixel.static.buffer.threads", str(threads))
    text = replace_property(text, "pixel.static.buffer.hugepage", str(mode == "static-hugepage").lower())
    text = replace_property(text, "pixel.column.size.path", str(column_sizes))
    text = replace_property(text, "pixel.enable.profiler", "false")
    text = replace_property(text, "pixel.bufferpool.stats.enabled", str(enable_buffer_stats).lower())
    config_path.write_text(text)
    return home


def build_sql(view_sql, query_dir, query_ids, threads):
    statements = [".timer on", f"PRAGMA threads={threads};", ".print __INIT_init__", view_sql]
    statements.extend((".print __WARMUP_q00__", (query_dir / "q00.sql").read_text()))
    for query_id in query_ids:
        statements.extend((f".print __QUERY_{query_id}__", (query_dir / f"{query_id}.sql").read_text()))
    statements.append(".quit")
    return "\n".join(statements) + "\n"


def build_isolated_query_sql(view_sql, query_dir, query_id, threads):
    return "\n".join((
        ".timer on",
        f"PRAGMA threads={threads};",
        ".print __INIT_init__",
        view_sql,
        f".print __QUERY_{query_id}__",
        (query_dir / f"{query_id}.sql").read_text(),
        ".quit",
    )) + "\n"


def parse_query_results(output, query_ids, include_warmup=True):
    marker = re.compile(r"^__(?:INIT|WARMUP|QUERY)_([a-z0-9]+)__$", re.MULTILINE)
    positions = list(marker.finditer(output))
    rows = []
    for index, match in enumerate(positions):
        end = positions[index + 1].start() if index + 1 < len(positions) else len(output)
        section = output[match.end():end]
        times = re.findall(r"Run Time \(s\): real ([0-9.]+)", section)
        if not times:
            raise RuntimeError(f"no timer result found for {match.group(1)}")
        normalized = re.sub(r"Run Time \(s\):.*", "", section)
        normalized = "\n".join(line.rstrip() for line in normalized.splitlines() if line.strip())
        rows.append((match.group(1), float(times[-1]), hashlib.sha256(normalized.encode()).hexdigest()))
    expected = ["init"] + (["q00"] if include_warmup else []) + list(query_ids)
    if [row[0] for row in rows] != expected:
        raise RuntimeError(f"query marker mismatch: got {[row[0] for row in rows]}")
    return rows


def parse_time_file(path):
    text = path.read_text()
    patterns = {
        "elapsed_seconds": r"Elapsed \(wall clock\) time.*: ([0-9:.]+)",
        "user_seconds": r"User time \(seconds\): ([0-9.]+)",
        "system_seconds": r"System time \(seconds\): ([0-9.]+)",
        "max_rss_kb": r"Maximum resident set size \(kbytes\): (\d+)",
        "minor_faults": r"Minor \(reclaiming a frame\) page faults: (\d+)",
        "major_faults": r"Major \(requiring I/O\) page faults: (\d+)",
        "voluntary_context_switches": r"Voluntary context switches: (\d+)",
        "involuntary_context_switches": r"Involuntary context switches: (\d+)",
    }
    result = {}
    for key, pattern in patterns.items():
        match = re.search(pattern, text)
        result[key] = match.group(1) if match else ""
    return result


def parse_buffer_pool_stats(path, expected_mode):
    pattern = re.compile(r"^\[BufferPoolStats\]\s+(.+)$", re.MULTILINE)
    rows = []
    for match in pattern.finditer(path.read_text(errors="replace")):
        values = dict(re.findall(r"([a-z_]+)=([^\s]+)", match.group(1)))
        if values.get("mode") == expected_mode:
            rows.append(values)
    if len(rows) != 1:
        raise RuntimeError(f"expected one {expected_mode} BufferPoolStats row in {path}, got {len(rows)}")
    row = rows[0]
    missing = [field for field in BUFFER_STATS_FIELDS if field not in row]
    if missing:
        raise RuntimeError(f"missing BufferPoolStats fields in {path}: {missing}")
    return row


def append_buffer_stats(root, mode, threads, repeat, phase, query, stats):
    with (root / "buffer_pool_metrics.csv").open("a", newline="") as output:
        csv.writer(output).writerow((mode, threads, repeat, phase, query,
                                     *(stats[field] for field in BUFFER_STATS_FIELDS)))


def measured_command(args, perf_path, time_path):
    return [
        "perf", "stat", "-x", ",", "-o", str(perf_path), "-e", EVENTS,
        "/usr/bin/time", "-v", "-o", str(time_path), str(args.duckdb), "-batch"
    ]


def run_cold_case(args, root, mode, threads, repeat, view_sql):
    case = root / "raw" / f"{mode}-t{threads}-r{repeat}"
    case.mkdir(parents=True, exist_ok=True)
    home = prepare_home(root, args.base_config, mode, threads, args.column_sizes, args.buffer_pool_stats)
    sql_path = case / "cold-start.sql"
    output_path = case / "cold-start.out"
    error_path = case / "cold-start.err"
    perf_path = case / "cold-perf.csv"
    time_path = case / "cold-time.txt"
    sql_path.write_text(build_sql(view_sql, args.query_dir, [], threads))
    env = os.environ.copy()
    env["PIXELS_SRC"] = str(args.repo_root)
    env["PIXELS_HOME"] = str(home)
    with sql_path.open("rb") as sql_input, output_path.open("wb") as stdout, error_path.open("wb") as stderr:
        completed = subprocess.run(measured_command(args, perf_path, time_path), stdin=sql_input,
                                   stdout=stdout, stderr=stderr, env=env, timeout=args.timeout)
    if completed.returncode != 0:
        raise RuntimeError(f"cold start {mode} t{threads} r{repeat} failed; see {error_path}")
    parsed = {query: seconds for query, seconds, _ in
              parse_query_results(output_path.read_text(errors="replace"), [])}
    totals = parse_time_file(time_path)
    stats = None
    if args.buffer_pool_stats:
        stats_mode = "static" if mode == "static-hugepage" else mode
        stats = parse_buffer_pool_stats(error_path, stats_mode)
    with (root / "cold_metrics.csv").open("a", newline="") as output:
        csv.writer(output).writerow((mode, threads, repeat, parsed["init"], parsed["q00"],
                                     *totals.values(), str(perf_path.relative_to(root))))
    if args.buffer_pool_stats:
        append_buffer_stats(root, mode, threads, repeat, "cold", "q00", stats)


def run_case(args, root, mode, threads, repeat, view_sql, query_ids):
    case = root / "raw" / f"{mode}-t{threads}-r{repeat}"
    case.mkdir(parents=True, exist_ok=True)
    home = prepare_home(root, args.base_config, mode, threads, args.column_sizes, args.buffer_pool_stats)
    sql_path = case / "workload.sql"
    output_path = case / "duckdb.out"
    error_path = case / "duckdb.err"
    perf_path = case / "perf.csv"
    time_path = case / "time.txt"
    sql_path.write_text(build_sql(view_sql, args.query_dir, query_ids, threads))

    command = measured_command(args, perf_path, time_path)
    env = os.environ.copy()
    env["PIXELS_SRC"] = str(args.repo_root)
    env["PIXELS_HOME"] = str(home)
    started = time.time()
    with sql_path.open("rb") as sql_input, output_path.open("wb") as stdout, error_path.open("wb") as stderr:
        completed = subprocess.run(command, stdin=sql_input, stdout=stdout, stderr=stderr,
                                   env=env, timeout=args.timeout)
    if completed.returncode != 0:
        raise RuntimeError(f"{mode} t{threads} r{repeat} failed; see {error_path}")

    parsed = parse_query_results(output_path.read_text(errors="replace"), query_ids)
    stats = None
    if args.buffer_pool_stats:
        stats_mode = "static" if mode == "static-hugepage" else mode
        stats = parse_buffer_pool_stats(error_path, stats_mode)
    with (root / "query_times.csv").open("a", newline="") as output:
        writer = csv.writer(output)
        for query_id, seconds, digest in parsed:
            writer.writerow((mode, threads, repeat, query_id, seconds, digest))
    totals = parse_time_file(time_path)
    with (root / "suite_metrics.csv").open("a", newline="") as output:
        writer = csv.writer(output)
        writer.writerow((mode, threads, repeat, time.time() - started, *totals.values(),
                         str(perf_path.relative_to(root))))
    if args.buffer_pool_stats:
        append_buffer_stats(root, mode, threads, repeat, "suite", "all", stats)


def run_isolated_query_case(args, root, mode, threads, repeat, view_sql, query_id):
    case = root / "raw-query" / f"{mode}-t{threads}-r{repeat}" / query_id
    case.mkdir(parents=True, exist_ok=True)
    home = prepare_home(root, args.base_config, mode, threads, args.column_sizes, True)
    sql_path = case / "query.sql"
    output_path = case / "duckdb.out"
    error_path = case / "duckdb.err"
    perf_path = case / "perf.csv"
    time_path = case / "time.txt"
    sql_path.write_text(build_isolated_query_sql(view_sql, args.query_dir, query_id, threads))
    env = os.environ.copy()
    env["PIXELS_SRC"] = str(args.repo_root)
    env["PIXELS_HOME"] = str(home)
    with sql_path.open("rb") as sql_input, output_path.open("wb") as stdout, error_path.open("wb") as stderr:
        completed = subprocess.run(measured_command(args, perf_path, time_path), stdin=sql_input,
                                   stdout=stdout, stderr=stderr, env=env, timeout=args.timeout)
    if completed.returncode != 0:
        raise RuntimeError(f"isolated {query_id} {mode} t{threads} r{repeat} failed; see {error_path}")
    parsed = parse_query_results(output_path.read_text(errors="replace"), [query_id], include_warmup=False)
    values = {query: (seconds, digest) for query, seconds, digest in parsed}
    totals = parse_time_file(time_path)
    stats_mode = "static" if mode == "static-hugepage" else mode
    stats = parse_buffer_pool_stats(error_path, stats_mode)
    with (root / "query_process_metrics.csv").open("a", newline="") as output:
        csv.writer(output).writerow((mode, threads, repeat, query_id, values["init"][0],
                                     values[query_id][0], values[query_id][1], *totals.values(),
                                     str(perf_path.relative_to(root))))
    append_buffer_stats(root, mode, threads, repeat, "query", query_id, stats)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, default=Path("/home/whz/test/pixels"))
    parser.add_argument("--cpp-root", type=Path, default=Path("/home/whz/test/pixels/cpp"))
    parser.add_argument("--duckdb", type=Path, default=Path("/home/whz/test/pixels/cpp/build/release/duckdb"))
    parser.add_argument("--base-config", type=Path, default=Path("/home/whz/test/pixels/cpp/etc/pixels-cpp.properties"))
    parser.add_argument("--benchmark-json", type=Path, default=Path("/home/whz/test/pixels/cpp/testcase/benchmark.json"))
    parser.add_argument("--query-dir", type=Path, default=Path("/home/whz/test/pixels/cpp/pixels-duckdb/duckdb/benchmark/clickbench/queries"))
    parser.add_argument("--column-sizes", type=Path, default=Path("/home/whz/pixels/clickbench-size-e0.csv"))
    parser.add_argument("--threads", type=int, nargs="+", default=[12, 24, 48])
    parser.add_argument("--modes", nargs="+", choices=MODES, default=list(MODES))
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--queries", nargs="+", default=[f"q{i:02d}" for i in range(1, 44)])
    parser.add_argument("--timeout", type=int, default=14400)
    parser.add_argument("--seed", type=int, default=1392)
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument("--allow-concurrent", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--skip-cold-start", action="store_true")
    parser.add_argument("--buffer-pool-stats", action="store_true")
    parser.add_argument("--isolated-query-stats", action="store_true")
    parser.add_argument("--isolated-repeats", type=int, default=1)
    args = parser.parse_args()

    required_commands = ("perf", "/usr/bin/time")
    for command in required_commands:
        if shutil.which(command) is None:
            raise RuntimeError(f"required command not found: {command}")
    if not args.duckdb.is_file() or not os.access(args.duckdb, os.X_OK):
        raise RuntimeError(f"DuckDB executable not found: {args.duckdb}")
    if not args.column_sizes.is_file():
        raise RuntimeError(f"column-size file not found: {args.column_sizes}")
    missing_queries = [query for query in ["q00", *args.queries]
                       if not (args.query_dir / f"{query}.sql").is_file()]
    if missing_queries:
        raise RuntimeError(f"missing query files: {missing_queries}")
    if not args.allow_concurrent:
        existing = subprocess.run(["pgrep", "-x", "duckdb"], text=True,
                                  stdout=subprocess.PIPE, check=False).stdout.strip()
        if existing:
            raise RuntimeError(f"another DuckDB process is running (PID: {existing}); "
                               "stop it or pass --allow-concurrent")

    args.root.mkdir(parents=True, exist_ok=True)
    environment_path = args.root / "environment.json"
    if not environment_path.exists():
        def command_output(command):
            return subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, check=False).stdout

        environment = {
            "started_at": datetime.datetime.now().astimezone().isoformat(),
            "hostname": platform.node(),
            "platform": platform.platform(),
            "git_head": command_output(["git", "-C", str(args.cpp_root), "rev-parse", "HEAD"]).strip(),
            "git_status": command_output(["git", "-C", str(args.cpp_root), "status", "--short"]),
            "lscpu": command_output(["lscpu"]),
            "memory": Path("/proc/meminfo").read_text(),
            "transparent_hugepage": Path("/sys/kernel/mm/transparent_hugepage/enabled").read_text().strip(),
            "perf_event_paranoid": Path("/proc/sys/kernel/perf_event_paranoid").read_text().strip(),
            "threads": args.threads,
            "modes": args.modes,
            "repeats": args.repeats,
            "queries": args.queries,
            "column_sizes": str(args.column_sizes),
            "isolated_query_stats": args.isolated_query_stats,
            "isolated_repeats": args.isolated_repeats,
            "buffer_pool_stats": args.buffer_pool_stats,
        }
        environment_path.write_text(json.dumps(environment, indent=2) + "\n")
    benchmark = json.loads(args.benchmark_json.read_text())
    view_sql = benchmark["clickbench-pixels-e0-24ssd"]
    query_times = args.root / "query_times.csv"
    suite_metrics = args.root / "suite_metrics.csv"
    if not query_times.exists():
        query_times.write_text("mode,threads,repeat,query,seconds,result_sha256\n")
    if not suite_metrics.exists():
        suite_metrics.write_text(
            "mode,threads,repeat,runner_elapsed_seconds,elapsed,user_seconds,system_seconds,max_rss_kb,"
            "minor_faults,major_faults,voluntary_context_switches,involuntary_context_switches,perf_file\n"
        )
    failures = args.root / "failures.csv"
    if not failures.exists():
        failures.write_text("mode,threads,repeat,error\n")
    cold_metrics = args.root / "cold_metrics.csv"
    if not cold_metrics.exists():
        cold_metrics.write_text(
            "mode,threads,repeat,init_seconds,warmup_seconds,elapsed,user_seconds,system_seconds,max_rss_kb,"
            "minor_faults,major_faults,voluntary_context_switches,involuntary_context_switches,perf_file\n"
        )
    buffer_pool_metrics = args.root / "buffer_pool_metrics.csv"
    if not buffer_pool_metrics.exists():
        buffer_pool_metrics.write_text(
            "mode,threads,repeat,phase,query," + ",".join(BUFFER_STATS_FIELDS) + "\n"
        )
    query_process_metrics = args.root / "query_process_metrics.csv"
    if not query_process_metrics.exists():
        query_process_metrics.write_text(
            "mode,threads,repeat,query,init_seconds,query_seconds,result_sha256,elapsed,user_seconds,"
            "system_seconds,max_rss_kb,minor_faults,major_faults,voluntary_context_switches,"
            "involuntary_context_switches,perf_file\n"
        )

    cases = [(mode, threads, repeat) for repeat in range(1, args.repeats + 1)
             for threads in args.threads for mode in args.modes]
    random.Random(args.seed).shuffle(cases)
    if args.dry_run:
        print(f"Preflight passed: {len(args.queries)} measured queries, {len(cases)} cases")
        if args.isolated_query_stats:
            isolated_count = len(args.modes) * len(args.threads) * args.isolated_repeats * len(args.queries)
            print(f"Isolated query-stat cases: {isolated_count}")
        for mode, threads, repeat in cases:
            print(f"mode={mode} threads={threads} repeat={repeat}")
        return
    completed = set()
    with suite_metrics.open() as source:
        for row in csv.DictReader(source):
            completed.add((row["mode"], int(row["threads"]), int(row["repeat"])))
    completed_cold = set()
    with cold_metrics.open() as source:
        for row in csv.DictReader(source):
            completed_cold.add((row["mode"], int(row["threads"]), int(row["repeat"])))
    for mode, threads, repeat in cases:
        if (mode, threads, repeat) in completed:
            continue
        print(f"START mode={mode} threads={threads} repeat={repeat}", flush=True)
        try:
            if not args.skip_cold_start and (mode, threads, repeat) not in completed_cold:
                run_cold_case(args, args.root, mode, threads, repeat, view_sql)
            run_case(args, args.root, mode, threads, repeat, view_sql, args.queries)
            print(f"DONE  mode={mode} threads={threads} repeat={repeat}", flush=True)
        except Exception as error:
            print(f"FAIL  mode={mode} threads={threads} repeat={repeat}: {error}", flush=True)
            with failures.open("a", newline="") as output:
                csv.writer(output).writerow((mode, threads, repeat, str(error)))
            if args.fail_fast:
                raise

    if args.isolated_query_stats:
        completed_queries = set()
        with query_process_metrics.open() as source:
            for row in csv.DictReader(source):
                completed_queries.add((row["mode"], int(row["threads"]), int(row["repeat"]), row["query"]))
        query_cases = [(mode, threads, repeat, query) for repeat in range(1, args.isolated_repeats + 1)
                       for threads in args.threads for mode in args.modes for query in args.queries]
        random.Random(args.seed + 1).shuffle(query_cases)
        for mode, threads, repeat, query in query_cases:
            key = (mode, threads, repeat, query)
            if key in completed_queries:
                continue
            print(f"START isolated mode={mode} threads={threads} repeat={repeat} query={query}", flush=True)
            try:
                run_isolated_query_case(args, args.root, mode, threads, repeat, view_sql, query)
                print(f"DONE  isolated mode={mode} threads={threads} repeat={repeat} query={query}", flush=True)
            except Exception as error:
                print(f"FAIL  isolated mode={mode} threads={threads} repeat={repeat} query={query}: {error}",
                      flush=True)
                with failures.open("a", newline="") as output:
                    csv.writer(output).writerow((mode, threads, repeat, f"isolated {query}: {error}"))
                if args.fail_fast:
                    raise


if __name__ == "__main__":
    main()
