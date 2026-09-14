#!/usr/bin/env python3
"""Isolated, whole-process measurements; never edits installed configuration."""
import argparse
import csv
import glob
import hashlib
import json
import os
from pathlib import Path
import random
import re
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


def properties(source, mode, threads, column_sizes):
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


def completed_cases(summary_path):
    if not summary_path.exists():
        return set()
    with summary_path.open(newline='') as summary:
        reader = csv.DictReader(summary)
        if reader.fieldnames != SUMMARY_FIELDS:
            raise RuntimeError(f'cannot resume: unexpected summary header in {summary_path}')
        return {(row['query'], row['mode'], int(row['repeat'])) for row in reader}


def required_artifacts_exist(case, mode, repeat, perf):
    # stderr is legitimately empty on a quiet successful run; the remaining
    # artifacts must contain data.
    must_exist = [case / 'timed.stderr']
    required = [case / 'resource.txt', case / 'timed.stdout']
    if mode in V2_MODES:
        required.extend([case / 'selective-timed.events.csv', case / 'selective-timed.workers.csv'])
    if perf and repeat == 0:
        required.extend([case / 'perf-stat.csv', case / 'perf.data',
                         case / 'stacks.folded', case / 'flamegraph.svg'])
        if mode in V2_MODES:
            required.extend([case / 'selective-stat.events.csv', case / 'selective-stat.workers.csv',
                             case / 'selective-record.events.csv', case / 'selective-record.workers.csv'])
    return (all(path.is_file() for path in must_exist) and
            all(path.is_file() and path.stat().st_size > 0 for path in required))


def repair_resume_summary(summary_path, output, perf):
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
        if required_artifacts_exist(case, key[1], key[2], perf):
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
    p.add_argument('--perf', action='store_true', help='separate full-process stat + record after timed repetitions')
    p.add_argument('--flamegraph-dir', type=Path)
    p.add_argument('--timeout', type=int, default=600)
    p.add_argument('--output', type=nonempty_path, required=True, help='new output, or existing output with --resume')
    p.add_argument('--resume', action='store_true', help='resume a compatible existing output directory')
    args = p.parse_args()
    if min(args.ssds, args.threads, args.repeat, args.timeout) < 1 or args.ssds > 16 or args.files_per_ssd < 0:
        p.error('require 1..16 SSDs, positive threads/repeat/timeout and nonnegative file limit')
    if any(m not in SUPPORTED_MODES for m in args.modes):
        p.error('unsupported mode')
    if args.smoke and len(args.modes) < 2:
        p.error('smoke requires at least two modes to compare results')
    if args.perf and not args.flamegraph_dir:
        p.error('--perf requires --flamegraph-dir')
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
    queries = dict(SMOKE) if args.smoke else {}
    if not args.smoke:
        query_root = ROOT / 'pixels-duckdb/duckdb/benchmark/clickbench'
        for q in selected_queries:
            if not re.fullmatch(r'q\d\d', q):
                p.error(f'invalid query name: {q}')
            candidates = [query_root / 'queries-test' / f'{q}.sql', query_root / 'queries' / f'{q}.sql']
            queries[q] = next(f for f in candidates if f.exists()).read_text().strip().rstrip(';')
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
                    'query_set', 'queries', 'smoke', 'perf'):
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
        args.output.mkdir(parents=True, exist_ok=False)
        manifest_path.write_text(json.dumps({
            'args': {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
            'binary_sha256': binary_hash,
            'paths': selected, 'files': files, 'queries': queries,
            'measurement': 'whole process; no cache drop; randomized mode order seed=20260909; separate profilers',
        }, indent=2))
    quoted = ','.join("'" + f.replace("'", "''") + "'" for f in files)
    setup = f"SET threads={args.threads};\nCREATE VIEW hits AS SELECT * FROM pixels_scan([{quoted}]);\n"
    env = dict(os.environ, PIXELS_SRC=str(ROOT.parent), PIXELS_HOME=str(args.output))
    env.pop('PIXELS_PERF_READY_FILE', None)
    for mode in args.modes:
        (args.output / f'{mode}.properties').write_text(properties(source, mode, args.threads, args.column_sizes.resolve()))
    rng = random.Random(20260909)
    baseline = {}
    done = repair_resume_summary(summary_path, args.output, args.perf) if args.resume else set()
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
                    env['PROPERTIES_PATH'] = str(args.output / f'{mode}.properties')
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
                    if args.perf and repeat == 0:
                        # Never wait for a static-only READY marker. All modes use
                        # exactly the same whole-process sampling boundary.
                        profile_sql = setup + f'EXPLAIN ANALYZE {content};\n'
                        binary = str(args.binary.resolve())
                        env['PIXELS_SELECTIVE_METRICS_PREFIX'] = str(case / 'selective-stat')
                        perf_stat = case / 'perf-stat.csv'
                        run_perf_stat(['perf', 'stat', '-x', ',', '-o', str(perf_stat), '-e',
                                       'task-clock,cycles,instructions,page-faults,minor-faults,major-faults,context-switches,cpu-migrations',
                                       '--', binary, '-bail', '-batch'], env, profile_sql,
                                      case / 'stat', perf_stat, args.timeout)
                        env['PIXELS_SELECTIVE_METRICS_PREFIX'] = str(case / 'selective-record')
                        run(['perf', 'record', '-F', '99', '--call-graph', 'fp', '-o', str(case / 'perf.data'),
                             '--', binary, '-bail', '-batch'], env, profile_sql, case / 'record', args.timeout)
                        with (case / 'stacks.txt').open('w') as out:
                            subprocess.run(['perf', 'script', '-i', str(case / 'perf.data')], stdout=out, check=True)
                        with (case / 'stacks.txt').open() as inp, (case / 'stacks.folded').open('w') as out:
                            subprocess.run(['perl', str(args.flamegraph_dir / 'stackcollapse-perf.pl')], stdin=inp, stdout=out, check=True)
                        if not (case / 'stacks.folded').stat().st_size:
                            raise RuntimeError('perf produced no folded samples')
                        with (case / 'stacks.folded').open() as inp, (case / 'flamegraph.svg').open('w') as out:
                            subprocess.run(['perl', str(args.flamegraph_dir / 'flamegraph.pl'), '--title', case.name + ' whole process'], stdin=inp, stdout=out, check=True)
                    if not required_artifacts_exist(case, mode, repeat, args.perf):
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
