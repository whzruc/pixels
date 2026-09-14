#!/usr/bin/env python3
"""Plot Selective V2 worker-buffer, queue-wait and load-balance telemetry."""
import argparse
import csv
import math
import re
import statistics
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


CASE_RE = re.compile(r'^([A-Za-z0-9_]+)-(.+)-r(\d+)$')
DEFAULT_QUERIES = ['q21', 'q22', 'q23', 'q24', 'q25', 'q27', 'q28',
                   'q30', 'q37', 'q39', 'q40', 'q42']
MODE_ORDER = ['legacy', 'dynamic', 'selective-4', 'selective-base',
              'selective-gate', 'selective-gate-cost', 'selective-all',
              'static-locked', 'static-lockfree']


def rows(path):
    with path.open(newline='') as source:
        return list(csv.DictReader(source))


def percentile(values, q):
    if not values:
        return 0.0
    return float(np.percentile(np.asarray(values, dtype=float), q))


def cv(values):
    values = [float(v) for v in values]
    mean = statistics.fmean(values) if values else 0.0
    return statistics.pstdev(values) / mean if mean else 0.0


def discover(result, prefix):
    cases = []
    for event_path in result.glob(f'*/{prefix}.events.csv'):
        match = CASE_RE.match(event_path.parent.name)
        if not match:
            continue
        query, mode, repeat = match.groups()
        worker_path = event_path.with_name(f'{prefix}.workers.csv')
        if worker_path.exists():
            cases.append((query, mode, int(repeat), event_path, worker_path))
    return sorted(cases)


def summarize(case):
    query, mode, repeat, event_path, worker_path = case
    events, workers = rows(event_path), rows(worker_path)
    waits = [int(r['queue_wait_ns']) for r in events
             if r['event'] == 'dequeue' and int(r['queue_wait_ns']) >= 0]
    enqueued = [int(r['demand_bytes']) for r in events if r['event'] == 'enqueue']
    executed = [int(r['executed_demand_bytes']) for r in workers]
    executed_files = [int(r['executed']) for r in workers]
    peak_buffers = [int(r['peak_buffer_bytes']) for r in workers]
    max_min = max(executed) / max(min((v for v in executed if v > 0), default=0), 1) if executed else 0
    return {
        'query': query, 'mode': mode, 'repeat': repeat, 'workers': len(workers),
        'transfers': len(enqueued),
        'executed_bytes_cv': cv(executed), 'executed_files_cv': cv(executed_files),
        'executed_bytes_max_min': max_min, 'peak_buffer_cv': cv(peak_buffers),
        'queue_wait_p50_ms': percentile(waits, 50) / 1e6,
        'queue_wait_p95_ms': percentile(waits, 95) / 1e6,
        'queue_wait_max_ms': max(waits, default=0) / 1e6,
        'enqueue_demand_p50_mib': percentile(enqueued, 50) / 2**20,
        'enqueue_demand_p95_mib': percentile(enqueued, 95) / 2**20,
        'max_total_queue_mib': max((int(r['total_queued_demand_bytes']) for r in events),
                                   default=0) / 2**20,
        'max_total_queue_depth': max((int(r['total_queue_depth']) for r in events), default=0),
        'adaptive_limit_changes': sum(r['event'] == 'queue_limit' for r in events),
    }


def step_matrix(events, workers, field, bins=240):
    max_ns = max((int(r['time_ns']) for r in events), default=1)
    edges = np.linspace(0, max_ns, bins)
    matrix = np.zeros((workers, bins), dtype=float)
    changes = [[] for _ in range(workers)]
    relevant = {'buffer_publish'} if field == 'worker_buffer_bytes' else {'enqueue', 'dequeue'}
    for row in events:
        worker = int(row['worker'])
        if row['event'] in relevant and 0 <= worker < workers:
            changes[worker].append((int(row['time_ns']), int(row[field])))
    for worker, series in enumerate(changes):
        series.sort()
        current = 0
        pos = 0
        for index, edge in enumerate(edges):
            while pos < len(series) and series[pos][0] <= edge:
                current = series[pos][1]
                pos += 1
            matrix[worker, index] = current / 2**20
    return edges / 1e9, matrix


def detailed_figure(case, output):
    query, mode, repeat, event_path, worker_path = case
    events, workers_rows = rows(event_path), rows(worker_path)
    nworkers = len(workers_rows)
    if not events or not workers_rows:
        return
    time_s, buffers = step_matrix(events, nworkers, 'worker_buffer_bytes')
    _, queues = step_matrix(events, nworkers, 'worker_queued_demand_bytes')
    waits = sorted(int(r['queue_wait_ns']) / 1e6 for r in events
                   if r['event'] == 'dequeue')
    executed = np.asarray([int(r['executed_demand_bytes']) / 2**30 for r in workers_rows])
    files = np.asarray([int(r['executed']) for r in workers_rows])

    fig, axes = plt.subplots(2, 2, figsize=(12.2, 7.6), constrained_layout=True)
    im = axes[0, 0].imshow(buffers, aspect='auto', origin='lower', cmap='viridis',
                           extent=[time_s[0], time_s[-1], -0.5, nworkers - 0.5])
    axes[0, 0].set(title='Per-worker registered buffer capacity', xlabel='Query time (s)',
                   ylabel='Worker id')
    fig.colorbar(im, ax=axes[0, 0], label='Capacity (MiB)')

    im = axes[0, 1].imshow(queues, aspect='auto', origin='lower', cmap='magma',
                           extent=[time_s[0], time_s[-1], -0.5, nworkers - 0.5])
    enqueues = [r for r in events if r['event'] == 'enqueue']
    if enqueues:
        enqueue_mib = np.asarray([int(r['demand_bytes']) / 2**20 for r in enqueues])
        marker_size = 12 + 55 * enqueue_mib / max(float(enqueue_mib.max()), 1.0)
        axes[0, 1].scatter([int(r['time_ns']) / 1e9 for r in enqueues],
                           [int(r['worker']) for r in enqueues], s=marker_size,
                           facecolors='none', edgecolors='white', linewidths=.7,
                           label='Circle size = file demand')
        axes[0, 1].legend(loc='lower right', fontsize=7, framealpha=.75)
    axes[0, 1].set(title='Queued file demand by receiver', xlabel='Query time (s)',
                   ylabel='Worker id')
    fig.colorbar(im, ax=axes[0, 1], label='Queued demand (MiB)')
    limits = [(int(r['time_ns']) / 1e9, int(r['effective_queue_limit'])) for r in events]
    if limits:
        inset = axes[0, 1].inset_axes([0.58, 0.69, 0.38, 0.25])
        inset.step([x for x, _ in limits], [y for _, y in limits], where='post', lw=1)
        inset.set_ylabel('limit', fontsize=7)
        inset.tick_params(labelsize=7)

    if waits:
        y = np.arange(1, len(waits) + 1) / len(waits)
        axes[1, 0].plot(waits, y, color='#2a9d8f', lw=2)
    axes[1, 0].set(title='Transferred-file queue wait ECDF', xlabel='Queue wait (ms)',
                   ylabel='Cumulative probability', ylim=(0, 1.02))
    axes[1, 0].grid(alpha=.25)

    x = np.arange(nworkers)
    axes[1, 1].bar(x, executed, color='#4c78a8', label='Demand')
    axes[1, 1].set(title=f'Worker load: bytes CV={cv(executed):.3f}, files CV={cv(files):.3f}',
                   xlabel='Worker id', ylabel='Executed demand (GiB)')
    other = axes[1, 1].twinx()
    other.plot(x, files, color='#e76f51', lw=1.2, marker='.', label='Files')
    other.set_ylabel('Executed files')
    axes[1, 1].grid(axis='y', alpha=.25)

    fig.suptitle(f'{query} / {mode} / repeat {repeat}: Selective scheduling telemetry',
                 fontsize=14)
    stem = output / f'{query}-{mode}-r{repeat}-telemetry'
    fig.savefig(stem.with_suffix('.png'), dpi=180)
    fig.savefig(stem.with_suffix('.svg'))
    plt.close(fig)


def comparison_figure(summaries, output):
    # Compare repeat 0, matching the detailed perf/trace run convention.
    selected = [r for r in summaries if r['repeat'] == 0]
    queries = sorted({r['query'] for r in selected})
    modes = sorted({r['mode'] for r in selected})
    if not queries or not modes:
        return
    fields = [('executed_bytes_cv', 'Executed-byte load CV', 'viridis'),
              ('queue_wait_p95_ms', 'Queue wait P95 (ms)', 'magma'),
              ('max_total_queue_mib', 'Peak queued demand (MiB)', 'cividis')]
    fig, axes = plt.subplots(1, 3, figsize=(max(12, len(queries) * .48), 4.8),
                             constrained_layout=True)
    lookup = {(r['query'], r['mode']): r for r in selected}
    for ax, (field, title, cmap) in zip(axes, fields):
        matrix = np.full((len(modes), len(queries)), np.nan)
        for yi, mode in enumerate(modes):
            for xi, query in enumerate(queries):
                if (query, mode) in lookup:
                    matrix[yi, xi] = float(lookup[query, mode][field])
        image = ax.imshow(matrix, aspect='auto', cmap=cmap)
        ax.set(title=title, xticks=range(len(queries)), xticklabels=queries,
               yticks=range(len(modes)), yticklabels=modes)
        ax.tick_params(axis='x', rotation=90)
        fig.colorbar(image, ax=ax, shrink=.85)
    fig.suptitle('Selective V2: load imbalance and queueing diagnostics', fontsize=14)
    fig.savefig(output / 'selective-v2-comparison.png', dpi=180)
    fig.savefig(output / 'selective-v2-comparison.svg')
    plt.close(fig)


def performance_outputs(result, output):
    summary_path = result / 'summary.csv'
    if not summary_path.exists():
        return
    grouped = {}
    for row in rows(summary_path):
        grouped.setdefault((row['query'], row['mode']), []).append(row)
    clean = []
    for (query, mode), items in sorted(grouped.items()):
        times = [float(r['query_seconds']) for r in items if r['query_seconds']]
        rss = [int(r['rss_kib']) / 2**20 for r in items if r['rss_kib']]
        if not times:
            continue
        clean.append({'query': query, 'mode': mode, 'repeats': len(times),
                      'median_seconds': statistics.median(times),
                      'min_seconds': min(times), 'max_seconds': max(times),
                      'median_rss_gib': statistics.median(rss) if rss else 0.0})
    if not clean:
        return
    lookup = {(r['query'], r['mode']): r for r in clean}
    queries = sorted({r['query'] for r in clean})
    present = {r['mode'] for r in clean}
    modes = [m for m in MODE_ORDER if m in present] + sorted(present - set(MODE_ORDER))
    for item in clean:
        baseline = lookup.get((item['query'], 'dynamic'))
        item['time_vs_dynamic'] = (item['median_seconds'] / baseline['median_seconds']
                                   if baseline else math.nan)
    with (output / 'selective-v2-performance-summary.csv').open('w', newline='') as target:
        writer = csv.DictWriter(target, fieldnames=list(clean[0]))
        writer.writeheader()
        writer.writerows(clean)

    runtime = np.full((len(modes), len(queries)), np.nan)
    rss = np.full_like(runtime, np.nan)
    for yi, mode in enumerate(modes):
        for xi, query in enumerate(queries):
            item = lookup.get((query, mode))
            if item:
                runtime[yi, xi] = item['time_vs_dynamic']
                rss[yi, xi] = item['median_rss_gib']
    fig, axes = plt.subplots(2, 1, figsize=(max(12, len(queries) * .42), 7.2),
                             constrained_layout=True)
    image = axes[0].imshow(runtime, aspect='auto', cmap='RdYlGn_r', vmin=.5, vmax=1.5)
    axes[0].set(title='Median query time / Dynamic (lower is better)',
                xticks=range(len(queries)), xticklabels=queries,
                yticks=range(len(modes)), yticklabels=modes)
    axes[0].tick_params(axis='x', rotation=90)
    fig.colorbar(image, ax=axes[0], label='Normalized time')
    image = axes[1].imshow(rss, aspect='auto', cmap='YlOrBr')
    axes[1].set(title='Median process peak RSS', xticks=range(len(queries)),
                xticklabels=queries, yticks=range(len(modes)), yticklabels=modes)
    axes[1].tick_params(axis='x', rotation=90)
    fig.colorbar(image, ax=axes[1], label='GiB')
    fig.suptitle('Selective V2 full-query performance and memory', fontsize=14)
    fig.savefig(output / 'selective-v2-performance.png', dpi=180)
    fig.savefig(output / 'selective-v2-performance.svg')
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--prefix', default='selective-timed',
                        help='metrics prefix: selective-timed/stat/record')
    parser.add_argument('--queries', nargs='*', default=DEFAULT_QUERIES)
    parser.add_argument('--modes', nargs='*')
    parser.add_argument('--repeat', type=int, default=0)
    args = parser.parse_args()
    if not args.results.is_dir():
        parser.error('results directory does not exist')
    args.output.mkdir(parents=True, exist_ok=True)
    cases = discover(args.results, args.prefix)
    if not cases:
        parser.error(f'no {args.prefix}.events.csv files found')
    summaries = [summarize(case) for case in cases]
    with (args.output / 'selective-v2-metrics-summary.csv').open('w', newline='') as target:
        writer = csv.DictWriter(target, fieldnames=list(summaries[0]))
        writer.writeheader()
        writer.writerows(summaries)
    comparison_figure(summaries, args.output)
    performance_outputs(args.results, args.output)
    wanted_queries = set(args.queries)
    wanted_modes = set(args.modes) if args.modes else None
    for case in cases:
        query, mode, repeat, _, _ = case
        if repeat == args.repeat and query in wanted_queries and (wanted_modes is None or mode in wanted_modes):
            detailed_figure(case, args.output)
    print(f'wrote telemetry summary and figures to {args.output}')


if __name__ == '__main__':
    main()
