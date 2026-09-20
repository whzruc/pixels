#!/usr/bin/env python3
"""Isolated, whole-process measurements; never edits installed configuration."""
import argparse
from contextlib import contextmanager
import csv
import glob
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
QUERIES = "q25 q28 q26 q27 q43 q37 q30 q39 q40 q07 q38 q42 q02 q08 q22 q41 q23 q21 q01 q31 q24".split()
FULL_QUERIES = [f'q{i:02d}' for i in range(44)]
V2_MODES = {
    'selective-base': {'queue': '4'},
    'selective-gate': {'queue': '4', 'gate': 'true'},
    'selective-cost': {'queue': '4', 'cost': 'true'},
    'selective-adaptive': {'queue': '4', 'adaptive': 'true'},
    'selective-gate-cost': {'queue': '4', 'gate': 'true', 'cost': 'true'},
    'selective-all': {'queue': '4', 'gate': 'true', 'cost': 'true', 'adaptive': 'true'},
}
SUPPORTED_MODES = {
    'legacy', 'dynamic', 'static', 'static-locked', 'static-lockfree',
    'selective-0', 'selective-1', 'selective-2', 'selective-4', *V2_MODES,
}
SMOKE = {
    "count": "SELECT count(*) FROM hits",
    "numeric": "SELECT count(*), sum(CounterID), min(EventTime), max(EventTime) FROM hits",
    "strings": "SELECT count(*), sum(length(URL)), sum(length(Referer)), sum(length(SearchPhrase)) FROM hits",
    "filtered": "SELECT CounterID, count(*), sum(length(URL)) FROM hits WHERE URL LIKE '%google%' GROUP BY CounterID ORDER BY CounterID",
}
SUMMARY_FIELDS = ['query', 'mode', 'repeat', 'process_seconds', 'query_seconds',
                  'rss_kib', 'result_sha256', 'selective_stats']


def nonempty_path(value):
    if not value or not value.strip():
        raise argparse.ArgumentTypeError('path must not be empty')
    return Path(value)


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as file:
        for block in iter(lambda: file.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def properties(source, mode, threads, column_sizes, buffer_hugepages='config'):
    values = {}
    for line in source.splitlines():
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    selective = mode.startswith('selective-')
    v2 = V2_MODES.get(mode, {})
    if mode in V2_MODES:
        queue_limit = v2['queue']
    elif selective:
        queue_limit = mode.split('-')[1]
    else:
        queue_limit = '0'
    static = mode in ('static', 'static-locked', 'static-lockfree')
    if buffer_hugepages != 'config':
        values['pixel.bufferpool.hugepage'] = 'true' if buffer_hugepages == 'on' else 'false'
    values.update({
        'pixels.dynamic.selective.enabled': str(selective).lower(),
        'pixels.dynamic.selective.queue.limit': queue_limit,
        'pixels.dynamic.selective.growth_gate.enabled': v2.get('gate', 'false'),
        'pixels.dynamic.selective.growth_gate.min_bytes': str(4 * 1024 * 1024),
        'pixels.dynamic.selective.growth_gate.require_history': 'true',
        'pixels.dynamic.selective.cost.enabled': v2.get('cost', 'false'),
        'pixels.dynamic.selective.cost.queue_weight': '1.0',
        'pixels.dynamic.selective.cost.queued_bytes_weight': '1.0',
        'pixels.dynamic.selective.cost.load_weight': '0.25',
        'pixels.dynamic.selective.cost.slack_weight': '0.01',
        'pixels.dynamic.selective.adaptive.enabled': v2.get('adaptive', 'false'),
        'pixels.dynamic.selective.adaptive.queue_min': '0',
        'pixels.dynamic.selective.adaptive.queue_max': '4',
        'pixels.dynamic.selective.adaptive.pressure_low': '0.10',
        'pixels.dynamic.selective.adaptive.pressure_high': '0.30',
        'pixels.dynamic.selective.adaptive.window': '64',
        'pixels.dynamic.selective.metrics.enabled': str(mode in V2_MODES).lower(),
        'pixels.enable.dynamic.buffer': str(mode == 'dynamic' or selective).lower(),
        'pixel.enable.globalStaticBytebuffer': str(static).lower(),
        'pixels.static.buffer.lock_free_lookup': str(mode == 'static-lockfree').lower(),
        'pixel.enable.globalBytebuffer': 'false',
        # BufferPool reads the historical lower-case spelling.  Keep the
        # camel-case alias for compatibility with older generated configs.
        'pixel.bufferpool.fixedsize': 'false',
        'pixel.bufferpool.fixedSize': 'false',
        'pixel.bufferpool.mode': 'dynamic' if selective else ('static' if static else mode),
        'localfs.enable.async.io': 'true', 'localfs.async.lib': 'iouring',
        'localfs.enable.spdk': 'false', 'localfs.enable.direct.io': 'true',
        'localfs.iouring.use.fixed.buffer': 'true', 'pixels.doublebuffer': 'true',
        'pixel.static.buffer.hugepage': 'false', 'pixels.static.buffer.hugepage': 'false',
        'pixel.threads': str(threads), 'pixel.static.buffer.threads': str(threads),
        'pixel.column.size.path': str(column_sizes),
        'pixel.globalStaticBytebuffer.columnSize': str(column_sizes),
        'pixel.enable.profiler': 'false',
    })
    return ''.join(f'{key}={value}\n' for key, value in sorted(values.items()))


def install_mode_properties(output, mode, content):
    """Install configuration for both current and historical ConfigFactory APIs."""
    explicit_path = output / f'{mode}.properties'
    explicit_path.write_text(content)

    # Older Pixels binaries do not understand PIXELS_PROPERTIES_PATH. The
    # 2026-08-25 code reads $PIXELS_HOME/etc/pixels-cpp.properties, while newer
    # PIXELS_HOME-based code reads $PIXELS_HOME/cpp/etc/pixels-cpp.properties.
    # Install both layouts so every historical build gets the intended config.
    legacy_home = output / 'config-homes' / mode
    for relative_path in ('etc/pixels-cpp.properties', 'cpp/etc/pixels-cpp.properties'):
        legacy_path = legacy_home / relative_path
        legacy_path.parent.mkdir(parents=True, exist_ok=True)
        legacy_path.write_text(content)
    return explicit_path, legacy_home


def run(command, env, sql, prefix, timeout):
    with prefix.with_suffix('.stdout').open('w') as out, prefix.with_suffix('.stderr').open('w') as err:
        try:
            subprocess.run(command, input=sql, text=True, env=env, stdout=out, stderr=err,
                           timeout=timeout, check=True)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
            err.flush()
            stderr = prefix.with_suffix('.stderr').read_text(errors='replace')
            tail = '\n'.join(stderr.splitlines()[-30:])
            raise RuntimeError(f'{prefix.name} failed; stderr tail:\n{tail}') from error


def run_perf_stat(command, env, sql, prefix, output, timeout):
    for attempt in range(2):
        run(command, env, sql, prefix, timeout)
        if output.is_file() and output.stat().st_size > 0:
            return
        if attempt == 0:
            print(f'[retry] {prefix.parent.name} perf stat produced an empty file', flush=True)
    raise RuntimeError(f'{prefix.parent.name} perf stat produced an empty file twice: {output}')


def perf_record_command(binary, output, frequency, call_graph, dwarf_stack_size):
    """Build the on-CPU command in one place so profiler settings are testable."""
    graph = call_graph if call_graph == 'fp' else f'dwarf,{dwarf_stack_size}'
    return ['perf', 'record', '-F', str(frequency), '-e', 'cpu-clock', '-g',
            f'--call-graph={graph}', '-o', str(output), '--', binary, '-bail', '-batch']


def folded_symbolization(path):
    """Return weighted coverage information for a folded stack file."""
    total = 0
    unknown = 0
    unknown_leaf = 0
    for line in path.read_text(errors='replace').splitlines():
        try:
            stack, weight = line.rsplit(' ', 1)
            weight = int(weight)
        except (ValueError, TypeError):
            continue
        total += weight
        frames = stack.split(';')
        if '[unknown]' in frames:
            unknown += weight
        if frames and frames[-1] == '[unknown]':
            unknown_leaf += weight
    return total, unknown, unknown_leaf


def write_symbolization_report(folded, output):
    total, unknown, unknown_leaf = folded_symbolization(folded)
    if total <= 0:
        raise RuntimeError(f'no weighted samples in {folded}')
    output.write_text(
        f'total_weight={total}\n'
        f'unknown_stack_weight={unknown}\n'
        f'unknown_stack_pct={unknown * 100.0 / total:.6f}\n'
        f'unknown_leaf_weight={unknown_leaf}\n'
        f'unknown_leaf_pct={unknown_leaf * 100.0 / total:.6f}\n')
    return unknown * 100.0 / total


def off_cpu_trace_seconds(query_seconds, requested):
    if requested > 0:
        return requested
    try:
        measured = float(query_seconds)
    except (TypeError, ValueError):
        measured = 10.0
    return max(5, int(measured / 2))


def resolve_off_cpu_tool(explicit):
    if explicit:
        candidate = explicit.resolve()
        if not candidate.is_file() or not os.access(candidate, os.X_OK):
            raise RuntimeError(f'off-CPU tool is not executable: {candidate}')
        return candidate
    patched = ROOT / 'testcase/offcputime_patched.py'
    if patched.is_file() and os.access(patched, os.X_OK):
        return patched
    system = shutil.which('offcputime-bpfcc')
    if system:
        return Path(system)
    raise RuntimeError('off-CPU tool not found; install bpfcc-tools or pass --off-cpu-tool')


@contextmanager
def relaxed_kptr_restrict():
    """Expose kernel symbols during profiling and restore the host setting."""
    path = Path('/proc/sys/kernel/kptr_restrict')
    previous = None
    if os.geteuid() == 0 and path.is_file():
        try:
            previous = path.read_text().strip()
            if previous != '0':
                path.write_text('0\n')
        except OSError as error:
            print(f'[warn] cannot relax kptr_restrict: {error}', flush=True)
            previous = None
    try:
        yield
    finally:
        if previous is not None and previous != '0':
            try:
                path.write_text(previous + '\n')
            except OSError as error:
                print(f'[warn] cannot restore kptr_restrict={previous}: {error}', flush=True)


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def run_off_cpu(command, env, sql, case, timeout, tool, flamegraph_dir,
                trace_seconds, stack_size, min_block_us):
    """Attach BCC before releasing DuckDB's stdin, then profile query waits."""
    folded = case / 'offcpu.folded'
    profiler_error = case / 'offcpu.stderr'
    query_stdout = case / 'offcpu-query.stdout'
    query_stderr = case / 'offcpu-query.stderr'
    duckdb = None
    profiler = None
    with query_stdout.open('w') as out, query_stderr.open('w') as err, \
            folded.open('w') as folded_out, profiler_error.open('w') as profiler_err:
        try:
            duckdb = subprocess.Popen(command, stdin=subprocess.PIPE, text=True, env=env,
                                      stdout=out, stderr=err)
            deadline = time.monotonic() + 5
            while not Path(f'/proc/{duckdb.pid}/maps').is_file() and time.monotonic() < deadline:
                if duckdb.poll() is not None:
                    break
                time.sleep(0.05)
            if duckdb.poll() is not None:
                raise RuntimeError('DuckDB exited before the off-CPU profiler attached')
            profiler = subprocess.Popen([
                str(tool), '--stack-storage-size', str(stack_size), '-p', str(duckdb.pid),
                '-f', '-m', str(min_block_us), str(trace_seconds),
            ], stdout=folded_out, stderr=profiler_err)
            # BCC compiles and attaches probes asynchronously. Do not release
            # the SQL until the profiler has had time to initialize.
            time.sleep(0.5)
            if profiler.poll() is not None:
                raise RuntimeError('off-CPU profiler exited before query execution')
            duckdb.stdin.write(sql)
            duckdb.stdin.flush()
            # Keep stdin open until BCC prints its stacks. If a short query
            # finishes first, DuckDB remains alive waiting for input and BCC
            # can still resolve its user-space mappings instead of [unknown].
            try:
                profiler.wait(timeout=trace_seconds + 30)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError('off-CPU profiler did not stop after its trace duration') from error
            duckdb.stdin.close()
            duckdb.stdin = None
            try:
                duckdb.wait(timeout=timeout)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(f'off-CPU DuckDB run timed out after {timeout}s') from error
        finally:
            stop_process(profiler)
            stop_process(duckdb)
    if duckdb.returncode:
        tail = '\n'.join(query_stderr.read_text(errors='replace').splitlines()[-30:])
        raise RuntimeError(f'off-CPU DuckDB run failed ({duckdb.returncode}); stderr tail:\n{tail}')
    if profiler.returncode:
        tail = '\n'.join(profiler_error.read_text(errors='replace').splitlines()[-30:])
        raise RuntimeError(f'off-CPU profiler failed ({profiler.returncode}); stderr tail:\n{tail}')
    if not folded.is_file() or folded.stat().st_size == 0:
        tail = '\n'.join(profiler_error.read_text(errors='replace').splitlines()[-30:])
        raise RuntimeError(f'off-CPU profiler produced no samples; stderr tail:\n{tail}')
    svg = case / 'offcpu.svg'
    with svg.open('w') as output:
        subprocess.run([
            'perl', str(flamegraph_dir / 'flamegraph.pl'),
            '--title', f'{case.name} off-CPU ({trace_seconds}s)',
            '--countname', 'microseconds', '--color', 'io', '--width', '1600', str(folded),
        ], stdout=output, check=True)
    return write_symbolization_report(folded, case / 'offcpu-symbolization.txt')


def completed_cases(summary_path):
    if not summary_path.exists():
        return set()
    with summary_path.open(newline='') as summary:
        reader = csv.DictReader(summary)
        if reader.fieldnames != SUMMARY_FIELDS:
            raise RuntimeError(f'cannot resume: unexpected summary header in {summary_path}')
        return {(row['query'], row['mode'], int(row['repeat'])) for row in reader}


def required_artifacts_exist(case, mode, repeat, perf, off_cpu=False):
    # stderr is legitimately empty on a quiet successful run; the remaining
    # artifacts must contain data.
    must_exist = [case / 'timed.stderr']
    required = [case / 'resource.txt', case / 'timed.stdout']
    if mode in V2_MODES:
        required.extend([case / 'selective-timed.events.csv', case / 'selective-timed.workers.csv'])
    if perf and repeat == 0:
        # perf.data is deliberately temporary: once perf script and the flame
        # graph have been produced it is deleted to avoid filling the result
        # volume. Resume therefore validates only the durable analysis files.
        required.extend([case / 'perf-stat.csv', case / 'stacks.txt', case / 'stacks.folded',
                         case / 'flamegraph.svg', case / 'symbolization.txt'])
        if mode in V2_MODES:
            required.extend([case / 'selective-stat.events.csv', case / 'selective-stat.workers.csv',
                             case / 'selective-record.events.csv', case / 'selective-record.workers.csv'])
    if off_cpu and repeat == 0:
        must_exist.extend([case / 'offcpu.stderr', case / 'offcpu-query.stderr'])
        required.extend([case / 'offcpu.folded', case / 'offcpu.svg',
                         case / 'offcpu-symbolization.txt', case / 'offcpu-query.stdout'])
    return (all(path.is_file() for path in must_exist) and
            all(path.is_file() and path.stat().st_size > 0 for path in required))


def repair_resume_summary(summary_path, output, perf, off_cpu=False):
    """Drop stale checkpoints whose timed/perf artifacts are incomplete."""
    with summary_path.open(newline='') as summary:
        reader = csv.DictReader(summary)
        if reader.fieldnames != SUMMARY_FIELDS:
            raise RuntimeError(f'cannot resume: unexpected summary header in {summary_path}')
        rows = list(reader)
    retained = []
    done = set()
    for row in rows:
        key = (row['query'], row['mode'], int(row['repeat']))
        case = output / f'{key[0]}-{key[1]}-r{key[2]}'
        if required_artifacts_exist(case, key[1], key[2], perf, off_cpu):
            retained.append(row)
            done.add(key)
        else:
            print(f'[redo] {case.name}: incomplete checkpoint artifacts', flush=True)
    if len(retained) != len(rows):
        temporary = summary_path.with_suffix('.csv.repairing')
        with temporary.open('w', newline='') as summary:
            writer = csv.DictWriter(summary, fieldnames=SUMMARY_FIELDS)
            writer.writeheader()
            writer.writerows(retained)
        temporary.replace(summary_path)
    return done


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, default=ROOT / 'build/release/duckdb')
    p.add_argument('--properties', type=Path, required=True)
    p.add_argument('--column-sizes', type=Path, default=Path('/home/whz/pixels/clickbench-size-e0.csv'))
    p.add_argument('--benchmark-json', type=Path, default=ROOT / 'testcase/benchmark.json')
    p.add_argument('--benchmark', default='clickbench-pixels-e0-24ssd')
    p.add_argument('--ssds', type=int, default=16)
    p.add_argument('--files-per-ssd', type=int, default=0, help='0 = all files')
    p.add_argument('--threads', type=int, default=48)
    p.add_argument('--repeat', type=int, default=3)
    p.add_argument('--modes', nargs='+', default=['legacy', 'dynamic', 'selective-0', 'selective-1', 'selective-2', 'selective-4', 'static'])
    p.add_argument('--query-set', choices=('regression', 'full'), default='regression')
    p.add_argument('--queries', nargs='+', help='explicit list; overrides --query-set')
    p.add_argument('--smoke', action='store_true', help='deterministic result equality checks, no EXPLAIN')
    p.add_argument('--perf', action='store_true', help='separate full-process stat + on-CPU record after timed repetitions')
    p.add_argument('--perf-call-graph', choices=('dwarf', 'fp'), default='dwarf',
                   help='on-CPU unwinder; DWARF avoids broken/unknown stacks in optimized builds')
    p.add_argument('--perf-frequency', type=int, default=99)
    p.add_argument('--perf-dwarf-stack-size', type=int, default=16384)
    p.add_argument('--off-cpu', action='store_true', help='also collect a BCC off-CPU flame graph for repeat 0')
    p.add_argument('--off-cpu-tool', type=Path,
                   help='offcputime executable; auto-detects testcase patch or offcputime-bpfcc')
    p.add_argument('--off-cpu-duration', type=int, default=0,
                   help='trace seconds; 0 uses half the timed query duration, minimum 5s')
    p.add_argument('--off-cpu-stack-size', type=int, default=32768)
    p.add_argument('--off-cpu-min-block-us', type=int, default=1)
    p.add_argument('--flamegraph-dir', type=Path)
    p.add_argument('--timeout', type=int, default=600)
    p.add_argument('--buffer-hugepages', choices=('config', 'on', 'off'), default='config',
                   help='override pixel.bufferpool.hugepage in generated mode configs')
    p.add_argument('--output', type=nonempty_path, required=True, help='new output, or existing output with --resume/--overwrite')
    p.add_argument('--resume', action='store_true', help='resume a compatible existing output directory')
    p.add_argument('--overwrite', action='store_true', help='delete an existing output directory before starting')
    args = p.parse_args()
    if args.resume and args.overwrite:
        p.error('--resume and --overwrite are mutually exclusive')
    if min(args.ssds, args.threads, args.repeat, args.timeout, args.perf_frequency,
           args.perf_dwarf_stack_size, args.off_cpu_stack_size,
           args.off_cpu_min_block_us) < 1 or args.files_per_ssd < 0:
        p.error('require positive SSDs/threads/repeat/timeout and nonnegative file limit')
    if args.off_cpu_duration < 0:
        p.error('--off-cpu-duration must be nonnegative')
    if any(m not in SUPPORTED_MODES for m in args.modes):
        p.error('unsupported mode')
    if args.smoke and len(args.modes) < 2:
        p.error('smoke requires at least two modes to compare results')
    if (args.perf or args.off_cpu) and not args.flamegraph_dir:
        p.error('--perf/--off-cpu requires --flamegraph-dir')
    if args.flamegraph_dir:
        for script in ('stackcollapse-perf.pl', 'flamegraph.pl'):
            if not (args.flamegraph_dir / script).is_file():
                p.error(f'missing FlameGraph script: {args.flamegraph_dir / script}')
    if args.off_cpu and os.geteuid() != 0:
        p.error('--off-cpu requires root; run the benchmark with sudo -E')
    try:
        off_cpu_tool = resolve_off_cpu_tool(args.off_cpu_tool) if args.off_cpu else None
    except RuntimeError as error:
        p.error(str(error))
    source = args.properties.read_text()
    if not args.column_sizes.is_file():
        p.error('column size CSV does not exist')
    benchmark = json.loads(args.benchmark_json.read_text())[args.benchmark]
    patterns = re.findall(r'"(/[^"]+)"', benchmark)
    if len(patterns) < args.ssds:
        p.error('benchmark has fewer data paths than requested')
    files = []
    selected = []
    for pattern in patterns[:args.ssds]:
        matches = sorted(f for f in glob.glob(pattern) if Path(f).is_file())
        if not matches:
            p.error(f'no files under {pattern}')
        matches = matches[:args.files_per_ssd] if args.files_per_ssd else matches
        files.extend(matches)
        selected.append({'pattern': pattern, 'files': len(matches)})
    if len(files) != len(set(files)):
        p.error('overlapping input paths would scan duplicate files')
    selected_queries = args.queries or (FULL_QUERIES if args.query_set == 'full' else QUERIES)
    queries = dict(SMOKE) if args.smoke and not args.queries else {}
    if not args.smoke or args.queries:
        query_root = ROOT / 'pixels-duckdb/duckdb/benchmark/clickbench'
        for q in selected_queries:
            if not re.fullmatch(r'q\d\d', q):
                p.error(f'invalid query name: {q}')
            candidates = [query_root / 'queries-test' / f'{q}.sql', query_root / 'queries' / f'{q}.sql']
            content = next(f for f in candidates if f.exists()).read_text().strip().rstrip(';')
            if args.smoke and q == 'q32':
                # q32's LIMIT has no tie-breaker; equal counts can return
                # different valid rows across execution modes.
                original_order = 'ORDER BY c DESC LIMIT 10'
                if original_order not in content:
                    p.error('q32 smoke tie-breaker no longer matches its SQL')
                content = content.replace(original_order,
                                          'ORDER BY c DESC, WatchID, ClientIP LIMIT 10')
            queries[q] = content
    args.output = args.output.resolve()
    manifest_path = args.output / 'manifest.json'
    summary_path = args.output / 'summary.csv'
    binary_hash = file_sha256(args.binary)
    if args.resume:
        if not args.output.is_dir() or not manifest_path.is_file() or not summary_path.is_file():
            p.error('--resume requires an existing benchmark output with manifest.json and summary.csv')
        manifest = json.loads(manifest_path.read_text())
        old_args = manifest.get('args', {})
        for key in ('benchmark', 'ssds', 'files_per_ssd', 'threads', 'repeat', 'modes',
                    'query_set', 'queries', 'smoke', 'perf', 'perf_call_graph',
                    'perf_frequency', 'perf_dwarf_stack_size', 'off_cpu',
                    'off_cpu_duration', 'off_cpu_stack_size', 'off_cpu_min_block_us'):
            current = getattr(args, key)
            if old_args.get(key) != current:
                p.error(f'cannot resume: argument {key} changed ({old_args.get(key)!r} != {current!r})')
        if manifest.get('files') != files or manifest.get('queries') != queries:
            p.error('cannot resume: selected files or query text changed')
        manifest.setdefault('resume_events', []).append({
            'time': time.strftime('%Y-%m-%dT%H:%M:%S%z'),
            'binary_sha256': binary_hash,
        })
        manifest_path.write_text(json.dumps(manifest, indent=2))
    else:
        if args.output.exists():
            if not args.overwrite:
                p.error(f'output already exists: {args.output}; use --resume or --overwrite')
            if not args.output.is_dir():
                p.error(f'output exists but is not a directory: {args.output}')
            shutil.rmtree(args.output)
        args.output.mkdir(parents=True, exist_ok=False)
        manifest_path.write_text(json.dumps({
            'args': {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
            'binary_sha256': binary_hash,
            'off_cpu_tool_resolved': str(off_cpu_tool) if off_cpu_tool else None,
            'paths': selected, 'files': files, 'queries': queries,
            'measurement': 'whole process; no cache drop; randomized mode order seed=20260909; separate on/off-CPU profilers',
        }, indent=2))
    quoted = ','.join("'" + f.replace("'", "''") + "'" for f in files)
    setup = f"SET threads={args.threads};\nCREATE VIEW hits AS SELECT * FROM pixels_scan([{quoted}]);\n"
    env = dict(os.environ, PIXELS_SRC=str(ROOT.parent))
    env.pop('PIXELS_PERF_READY_FILE', None)
    mode_configs = {}
    mode_homes = {}
    for mode in args.modes:
        mode_configs[mode], mode_homes[mode] = install_mode_properties(
            args.output, mode, properties(source, mode, args.threads,
                                          args.column_sizes.resolve(), args.buffer_hugepages))
    rng = random.Random(20260909)
    baseline = {}
    done = repair_resume_summary(summary_path, args.output, args.perf, args.off_cpu) if args.resume else set()
    with summary_path.open('a' if args.resume else 'w') as summary:
        writer = csv.DictWriter(summary, fieldnames=SUMMARY_FIELDS)
        if not args.resume:
            writer.writeheader()
        for query, content in queries.items():
            for repeat in range(args.repeat):
                modes = list(args.modes)
                rng.shuffle(modes)
                for mode in modes:
                    case = args.output / f'{query}-{mode}-r{repeat}'
                    key = (query, mode, repeat)
                    if key in done:
                        print(f'[skip] {case.name}', flush=True)
                        continue
                    case.mkdir(exist_ok=args.resume)
                    # Current ConfigFactory uses PIXELS_PROPERTIES_PATH. Older
                    # builds ignore it and use one of the PIXELS_HOME layouts
                    # installed by install_mode_properties().
                    env['PIXELS_PROPERTIES_PATH'] = str(mode_configs[mode])
                    env['PIXELS_HOME'] = str(mode_homes[mode])
                    env.pop('PROPERTIES_PATH', None)
                    env['PIXELS_SELECTIVE_METRICS_PREFIX'] = str(case / 'selective-timed')
                    result = case / 'result.csv'
                    statement = f"COPY ({content}) TO '{str(result).replace(chr(39), chr(39)*2)}' (FORMAT CSV, HEADER);" if args.smoke else f'EXPLAIN ANALYZE {content};'
                    sql = setup + statement + '\n'
                    (case / 'query.sql').write_text(sql)
                    print(f'[run] {case.name}', flush=True)
                    start = time.monotonic()
                    run(['/usr/bin/time', '-v', '-o', str(case / 'resource.txt'), str(args.binary.resolve()), '-bail', '-batch'], env, sql, case / 'timed', args.timeout)
                    elapsed = time.monotonic() - start
                    digest = ''
                    if args.smoke:
                        with result.open(newline='') as f:
                            rows = list(csv.reader(f))
                        digest = hashlib.sha256(json.dumps([rows[0], sorted(rows[1:])]).encode()).hexdigest()
                        if query in baseline and digest != baseline[query]:
                            raise RuntimeError(f'result mismatch: {case}; inspect CSVs')
                        baseline[query] = digest
                    log = (case / 'timed.stdout').read_text() + (case / 'timed.stderr').read_text()
                    timing = re.search(r'Total Time:\s*([\d.]+)s', log)
                    rss = re.search(r'Maximum resident set size \(kbytes\):\s*(\d+)', (case / 'resource.txt').read_text())
                    stats = ' | '.join(line for line in log.splitlines() if line.startswith('[SelectiveBuffer'))
                    if mode.startswith('selective-') and not stats:
                        raise RuntimeError('no selective metrics: stale binary or configuration not applied')
                    if (args.perf or args.off_cpu) and repeat == 0:
                        # Never wait for a static-only READY marker. All modes use
                        # exactly the same whole-process sampling boundary.
                        profile_sql = setup + f'EXPLAIN ANALYZE {content};\n'
                        binary = str(args.binary.resolve())
                    if args.perf and repeat == 0:
                        env['PIXELS_SELECTIVE_METRICS_PREFIX'] = str(case / 'selective-stat')
                        perf_stat = case / 'perf-stat.csv'
                        perf_data = case / 'perf.data'
                        with relaxed_kptr_restrict():
                            run_perf_stat(['perf', 'stat', '-x', ',', '-o', str(perf_stat), '-e',
                                           'task-clock,cycles,instructions,page-faults,minor-faults,major-faults,context-switches,cpu-migrations',
                                           '--', binary, '-bail', '-batch'], env, profile_sql,
                                          case / 'stat', perf_stat, args.timeout)
                            env['PIXELS_SELECTIVE_METRICS_PREFIX'] = str(case / 'selective-record')
                            try:
                                run(perf_record_command(binary, perf_data, args.perf_frequency,
                                                        args.perf_call_graph, args.perf_dwarf_stack_size),
                                    env, profile_sql, case / 'record', args.timeout)
                                with (case / 'stacks.txt').open('w') as out:
                                    subprocess.run(['perf', 'script', '-i', str(perf_data)],
                                                   stdout=out, check=True)
                                with (case / 'stacks.txt').open() as inp, (case / 'stacks.folded').open('w') as out:
                                    subprocess.run(['perl', str(args.flamegraph_dir / 'stackcollapse-perf.pl')],
                                                   stdin=inp, stdout=out, check=True)
                                if not (case / 'stacks.folded').stat().st_size:
                                    raise RuntimeError('perf produced no folded samples')
                                unknown_pct = write_symbolization_report(case / 'stacks.folded',
                                                                         case / 'symbolization.txt')
                                if unknown_pct > 5.0:
                                    print(f'[warn] {case.name}: {unknown_pct:.2f}% of on-CPU sample weight contains [unknown]',
                                          flush=True)
                                with (case / 'flamegraph.svg').open('w') as out:
                                    subprocess.run([
                                        'perl', str(args.flamegraph_dir / 'flamegraph.pl'),
                                        '--title', case.name + ' whole process on-CPU',
                                        '--countname', 'samples', '--color', 'hot', '--width', '1600',
                                        str(case / 'stacks.folded'),
                                    ], stdout=out, check=True)
                            finally:
                                perf_data.unlink(missing_ok=True)
                    if args.off_cpu and repeat == 0:
                        env['PIXELS_SELECTIVE_METRICS_PREFIX'] = str(case / 'selective-offcpu')
                        trace_seconds = off_cpu_trace_seconds(timing[1] if timing else None,
                                                             args.off_cpu_duration)
                        print(f'[off-cpu] {case.name}: tracing {trace_seconds}s with {off_cpu_tool}',
                              flush=True)
                        with relaxed_kptr_restrict():
                            off_cpu_unknown_pct = run_off_cpu(
                                [binary, '-bail', '-batch'], env, profile_sql, case,
                                args.timeout, off_cpu_tool, args.flamegraph_dir,
                                trace_seconds, args.off_cpu_stack_size,
                                args.off_cpu_min_block_us)
                        if off_cpu_unknown_pct > 5.0:
                            print(f'[warn] {case.name}: {off_cpu_unknown_pct:.2f}% of off-CPU weight contains [unknown]',
                                  flush=True)
                    if not required_artifacts_exist(case, mode, repeat, args.perf, args.off_cpu):
                        raise RuntimeError(f'{case.name} finished but one or more required artifacts are empty or missing')
                    # A summary row is the durable resume checkpoint. Write it
                    # only after all requested profiler artifacts are complete.
                    writer.writerow(dict(query=query, mode=mode, repeat=repeat, process_seconds=elapsed,
                                         query_seconds=timing[1] if timing else '', rss_kib=rss[1] if rss else '',
                                         result_sha256=digest, selective_stats=stats))
                    summary.flush()
                    (case / 'COMPLETE').write_text('timed measurement and requested profiler runs completed\n')
    (args.output / 'SUCCESS').write_text('All requested runs completed; smoke equality checked when enabled.\n')
    print(f'[done] {args.output}')


if __name__ == '__main__':
    main()
