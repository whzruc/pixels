#!/usr/bin/env python3
"""Reproducible analysis and paper-style figures for the 44-query V2 run."""
import argparse
import collections
import csv
import json
import math
import re
import statistics
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np


MODES = ['legacy', 'dynamic', 'selective-4', 'selective-base', 'selective-gate',
         'selective-gate-cost', 'selective-all', 'static-locked', 'static-lockfree']
V2_MODES = ['selective-base', 'selective-gate', 'selective-gate-cost', 'selective-all']
COLORS = {
    'legacy': '#64748b', 'dynamic': '#dc5a5a', 'selective-4': '#80a7c7',
    'selective-base': '#4c78a8', 'selective-gate': '#2a9d8f',
    'selective-gate-cost': '#167d6d', 'selective-all': '#6a4c93',
    'static-locked': '#e9a23b', 'static-lockfree': '#bd6b2d',
}
LABELS = {
    'legacy': 'Legacy', 'dynamic': 'Dynamic', 'selective-4': 'Selective V1',
    'selective-base': 'V2 Base', 'selective-gate': '+ Gate',
    'selective-gate-cost': '+ Cost', 'selective-all': '+ Adaptive',
    'static-locked': 'Static locked', 'static-lockfree': 'Static lock-free',
}
STAT_RE = re.compile(r'(\w+)=(\d+)')


def geomean(values):
    values = [float(value) for value in values if float(value) > 0]
    return math.exp(statistics.fmean(math.log(value) for value in values))


def percentile(values, q):
    return float(np.percentile(np.asarray(values, dtype=float), q)) if values else 0.0


def bootstrap_geomean_ci(values, samples=10000):
    values = np.asarray(values, dtype=float)
    rng = np.random.default_rng(20260914)
    draws = rng.choice(values, size=(samples, len(values)), replace=True)
    estimates = np.exp(np.mean(np.log(draws), axis=1))
    return float(np.percentile(estimates, 2.5)), float(np.percentile(estimates, 97.5))


def write_csv(path, records, fields=None):
    if not records:
        return
    fields = fields or list(records[0])
    with path.open('w', newline='') as target:
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        writer.writerows(records)


def save(fig, output, name):
    fig.savefig(output / f'{name}.png', dpi=220, bbox_inches='tight')
    fig.savefig(output / f'{name}.svg', bbox_inches='tight')
    plt.close(fig)


def load_timed(result):
    groups = collections.defaultdict(list)
    with (result / 'summary.csv').open(newline='') as source:
        for row in csv.DictReader(source):
            groups[row['query'], row['mode']].append(row)
    medians = {}
    per_query = []
    for (query, mode), items in sorted(groups.items()):
        times = [float(row['query_seconds']) for row in items]
        process = [float(row['process_seconds']) for row in items]
        rss = [int(row['rss_kib']) / 2**20 for row in items]
        stats = []
        for row in items:
            parsed = {key: int(value) for key, value in STAT_RE.findall(row['selective_stats'])}
            if parsed:
                stats.append(parsed)
        item = {
            'query': query, 'mode': mode, 'repeats': len(items),
            'median_seconds': statistics.median(times),
            'min_seconds': min(times), 'max_seconds': max(times),
            'range_over_median': (max(times) - min(times)) / statistics.median(times),
            'median_process_seconds': statistics.median(process),
            'median_rss_gib': statistics.median(rss), 'max_rss_gib': max(rss),
        }
        for field in ('claimed', 'transferred', 'dequeued', 'local_fit', 'local_grow',
                      'queue_high_water', 'queued_demand_bytes_high_water', 'queue_wait_ns',
                      'max_queue_wait_ns', 'gate_rejected', 'queue_rejected', 'no_receiver',
                      'cost_evaluations', 'adaptive_limit_changes', 'executed',
                      'observed_buffer_bytes', 'allocations', 'growths', 'growth_bytes'):
            values = [entry[field] for entry in stats if field in entry]
            item[field] = statistics.median(values) if values else 0
        medians[query, mode] = item
    for key, item in sorted(medians.items()):
        query, mode = key
        row = dict(item)
        row['vs_dynamic'] = item['median_seconds'] / medians[query, 'dynamic']['median_seconds']
        row['vs_legacy'] = item['median_seconds'] / medians[query, 'legacy']['median_seconds']
        per_query.append(row)
    return medians, per_query


def aggregate_modes(medians, queries):
    output = []
    for mode in MODES:
        ratios = [medians[q, mode]['median_seconds'] / medians[q, 'dynamic']['median_seconds']
                  for q in queries]
        process_ratios = [medians[q, mode]['median_process_seconds'] /
                          medians[q, 'dynamic']['median_process_seconds'] for q in queries]
        legacy_ratios = [medians[q, mode]['median_seconds'] / medians[q, 'legacy']['median_seconds']
                         for q in queries]
        low, high = bootstrap_geomean_ci(ratios)
        rss = [medians[q, mode]['median_rss_gib'] for q in queries]
        output.append({
            'mode': mode,
            'sum_query_medians_seconds': sum(medians[q, mode]['median_seconds'] for q in queries),
            'sum_process_medians_seconds': sum(medians[q, mode]['median_process_seconds'] for q in queries),
            'geomean_vs_dynamic': geomean(ratios),
            'process_geomean_vs_dynamic': geomean(process_ratios),
            'geomean_vs_dynamic_ci_low': low, 'geomean_vs_dynamic_ci_high': high,
            'geomean_vs_legacy': geomean(legacy_ratios),
            'faster_than_dynamic_queries': sum(value < .98 for value in ratios),
            'within_2pct_dynamic_queries': sum(.98 <= value <= 1.02 for value in ratios),
            'slower_than_dynamic_queries': sum(value > 1.02 for value in ratios),
            'median_rss_gib': statistics.median(rss), 'p95_rss_gib': percentile(rss, 95),
            'max_rss_gib': max(rss),
            'median_repeat_range_fraction': statistics.median(
                medians[q, mode]['range_over_median'] for q in queries),
        })
    return output


def load_metrics(path):
    groups = collections.defaultdict(list)
    with path.open(newline='') as source:
        for row in csv.DictReader(source):
            groups[row['query'], row['mode']].append(row)
    records = []
    numeric = ['transfers', 'executed_bytes_cv', 'executed_files_cv',
               'executed_bytes_max_min', 'peak_buffer_cv', 'queue_wait_p50_ms',
               'queue_wait_p95_ms', 'queue_wait_max_ms', 'enqueue_demand_p50_mib',
               'enqueue_demand_p95_mib', 'max_total_queue_mib',
               'max_total_queue_depth', 'adaptive_limit_changes']
    for (query, mode), rows in sorted(groups.items()):
        record = {'query': query, 'mode': mode, 'repeats': len(rows)}
        for field in numeric:
            record[field] = statistics.median(float(row[field]) for row in rows)
        records.append(record)
    return records


def parse_perf(path):
    values = {}
    with path.open(newline='') as source:
        for row in csv.reader(source):
            if len(row) < 3 or row[0].startswith('#'):
                continue
            try:
                value = float(row[0].replace(',', ''))
            except ValueError:
                continue
            values[row[2]] = value
    if values.get('cycles'):
        values['ipc'] = values.get('instructions', 0) / values['cycles']
    return values


def load_perf(result, queries):
    per_case = {}
    for path in result.glob('q*-*-r0/perf-stat.csv'):
        name = path.parent.name
        per_case[name[:3], name[4:-3]] = parse_perf(path)
    fields = ['task-clock', 'cycles', 'instructions', 'page-faults', 'minor-faults',
              'major-faults', 'context-switches', 'cpu-migrations', 'ipc']
    output = []
    for mode in MODES:
        record = {'mode': mode}
        for field in fields:
            vals = [per_case[q, mode].get(field, 0) for q in queries]
            record[f'sum_{field}'] = sum(vals)
            record[f'median_{field}'] = statistics.median(vals)
            dynamic_ratios = [per_case[q, mode].get(field, 0) /
                              per_case[q, 'dynamic'].get(field, 1)
                              for q in queries if per_case[q, mode].get(field, 0) > 0 and
                              per_case[q, 'dynamic'].get(field, 0) > 0]
            record[f'geomean_{field}_vs_dynamic'] = geomean(dynamic_ratios) if dynamic_ratios else 0
        output.append(record)
    return output


def load_hotspots(result):
    terms = {
        'growbuffer': 'DynamicBufferPool::GrowBuffer',
        'register_update': 'io_uring_register_buffers_update',
        'selective_claim': 'SelectiveBufferScheduler::claim',
        'metrics_trace': 'SelectiveBufferScheduler::trace',
        'static_getbuffer': 'GlobalStaticBufferPool::GetBuffer',
        'mutex': 'pthread_mutex',
        'unknown': '[unknown]',
    }
    records = []
    for path in sorted(result.glob('q*-*-r0/stacks.folded')):
        total = 0
        hits = collections.Counter()
        with path.open(errors='replace') as source:
            for line in source:
                try:
                    stack, raw_weight = line.rsplit(' ', 1)
                    weight = int(raw_weight)
                except ValueError:
                    continue
                total += weight
                for field, term in terms.items():
                    if term in stack:
                        hits[field] += weight
        name = path.parent.name
        record = {'query': name[:3], 'mode': name[4:-3], 'total_weight': total}
        for field in terms:
            record[f'{field}_stack_percent'] = 100 * hits[field] / total if total else 0
        records.append(record)
    return records


def top_deltas(medians, queries):
    comparisons = [
        ('metrics_overhead', 'selective-base', 'selective-4'),
        ('growth_gate', 'selective-gate', 'selective-base'),
        ('receiver_cost', 'selective-gate-cost', 'selective-gate'),
        ('adaptive_queue', 'selective-all', 'selective-gate-cost'),
        ('static_lockfree', 'static-lockfree', 'static-locked'),
    ]
    records = []
    for stage, after, before in comparisons:
        for query in queries:
            ratio = medians[query, after]['median_seconds'] / medians[query, before]['median_seconds']
            records.append({'stage': stage, 'query': query, 'before': before, 'after': after,
                            'before_seconds': medians[query, before]['median_seconds'],
                            'after_seconds': medians[query, after]['median_seconds'],
                            'ratio': ratio, 'change_percent': 100 * (ratio - 1)})
    return records


def figures(output, medians, queries, aggregate, metrics, perf, deltas, hotspots):
    # Runtime heatmap.
    matrix = np.asarray([[medians[q, m]['median_seconds'] / medians[q, 'dynamic']['median_seconds']
                          for q in queries] for m in MODES])
    fig, ax = plt.subplots(figsize=(17, 5.4))
    image = ax.imshow(matrix, aspect='auto', cmap='RdYlGn_r', vmin=.65, vmax=1.35)
    ax.set(xticks=range(len(queries)), xticklabels=queries, yticks=range(len(MODES)),
           yticklabels=[LABELS[m] for m in MODES],
           title='Median query time normalized to Dynamic (lower is better)')
    ax.tick_params(axis='x', rotation=90)
    fig.colorbar(image, ax=ax, label='× Dynamic', fraction=.02, pad=.015)
    save(fig, output, 'runtime-heatmap-44q')

    # Aggregate runtime with query bootstrap confidence intervals.
    lookup = {row['mode']: row for row in aggregate}
    values = [lookup[m]['geomean_vs_dynamic'] for m in MODES]
    low = [values[i] - lookup[m]['geomean_vs_dynamic_ci_low'] for i, m in enumerate(MODES)]
    high = [lookup[m]['geomean_vs_dynamic_ci_high'] - values[i] for i, m in enumerate(MODES)]
    fig, ax = plt.subplots(figsize=(11.5, 5.2))
    x = np.arange(len(MODES))
    ax.bar(x, values, color=[COLORS[m] for m in MODES], yerr=[low, high], capsize=3)
    ax.axhline(1, color='#334155', ls='--', lw=1)
    for xi, value in zip(x, values): ax.text(xi, value + .018, f'{value:.3f}', ha='center', fontsize=8)
    ax.set(xticks=x, xticklabels=[LABELS[m] for m in MODES], ylabel='Geometric mean × Dynamic',
           title='Aggregate query performance (44 queries; 95% query-bootstrap CI)')
    ax.tick_params(axis='x', rotation=25)
    ax.grid(axis='y', alpha=.2)
    save(fig, output, 'aggregate-runtime')

    # Memory footprint.
    median_rss = [lookup[m]['median_rss_gib'] for m in MODES]
    max_rss = [lookup[m]['max_rss_gib'] for m in MODES]
    fig, ax = plt.subplots(figsize=(11.5, 5.2))
    x = np.arange(len(MODES)); width = .38
    ax.bar(x-width/2, median_rss, width, label='Median across queries', color='#4c78a8')
    ax.bar(x+width/2, max_rss, width, label='Maximum query', color='#e09f3e')
    ax.set(xticks=x, xticklabels=[LABELS[m] for m in MODES], ylabel='Peak process RSS (GiB)',
           title='Whole-process memory footprint')
    ax.tick_params(axis='x', rotation=25)
    ax.legend(frameon=False); ax.grid(axis='y', alpha=.2)
    save(fig, output, 'memory-footprint')

    # Ablation distributions.
    stages = ['metrics_overhead', 'growth_gate', 'receiver_cost', 'adaptive_queue']
    values = [[row['ratio'] for row in deltas if row['stage'] == stage] for stage in stages]
    fig, ax = plt.subplots(figsize=(9.5, 5.2))
    parts = ax.violinplot(values, showmedians=True, showextrema=False)
    for body, color in zip(parts['bodies'], ['#80a7c7', '#2a9d8f', '#167d6d', '#6a4c93']):
        body.set_facecolor(color); body.set_alpha(.78)
    ax.axhline(1, color='#334155', ls='--', lw=1)
    ax.set(xticks=range(1, 5), xticklabels=['Metrics/Base vs V1', 'Gate vs Base',
                                           'Cost vs Gate', 'Adaptive vs Cost'],
           ylabel='After / before query time', title='Per-query V2 ablation effects')
    ax.grid(axis='y', alpha=.2)
    save(fig, output, 'v2-ablation')

    # Static lock removal per query.
    ratios = [(q, medians[q, 'static-lockfree']['median_seconds'] /
               medians[q, 'static-locked']['median_seconds']) for q in queries]
    ratios.sort(key=lambda item: item[1])
    fig, ax = plt.subplots(figsize=(14, 4.8))
    ax.bar(range(len(ratios)), [r for _, r in ratios],
           color=['#2a9d8f' if r < 1 else '#dc5a5a' for _, r in ratios])
    ax.axhline(1, color='#334155', ls='--', lw=1)
    ax.set(xticks=range(len(ratios)), xticklabels=[q for q, _ in ratios], ylabel='Lock-free / locked',
           title='Static query-path mutex removal: per-query effect')
    ax.tick_params(axis='x', rotation=90); ax.grid(axis='y', alpha=.2)
    save(fig, output, 'static-lockfree-effect')

    # Selective scheduling diagnostics.
    by_key = {(row['query'], row['mode']): row for row in metrics}
    # Gate-active queries expose the routing/queue trade-off. Across all 44
    # queries the median is zero, which hides the behavior under investigation.
    active_queries = [q for q in queries if float(by_key[q, 'selective-gate']['transfers']) > 0]
    by_mode = collections.defaultdict(list)
    for row in metrics:
        if row['query'] in active_queries:
            by_mode[row['mode']].append(row)
    fig, axes = plt.subplots(2, 2, figsize=(10.8, 7.2), constrained_layout=True)
    fields = [('transfers', 'Median transfers/query'),
              ('queue_wait_p95_ms', 'Median queue-wait P95 (ms)'),
              ('executed_bytes_cv', 'Median worker demand CV'),
              ('max_total_queue_mib', 'Median peak queued demand (MiB)')]
    for ax, (field, title) in zip(axes.flat, fields):
        vals = [statistics.median(float(row[field]) for row in by_mode[m]) for m in V2_MODES]
        ax.bar(range(4), vals, color=[COLORS[m] for m in V2_MODES])
        ax.set(xticks=range(4), xticklabels=[LABELS[m] for m in V2_MODES], title=title)
        ax.tick_params(axis='x', rotation=20); ax.grid(axis='y', alpha=.2)
    fig.suptitle(f'Selective scheduling diagnostics on {len(active_queries)} gate-active queries')
    save(fig, output, 'selective-scheduling-diagnostics')

    # Whole-process perf ratios.
    perf_lookup = {row['mode']: row for row in perf}
    fields = [('task-clock', 'Task clock'), ('cycles', 'Cycles'),
              ('instructions', 'Instructions'), ('page-faults', 'Page faults')]
    fig, ax = plt.subplots(figsize=(12, 5.4))
    x = np.arange(len(MODES)); width = .19
    for index, (field, label) in enumerate(fields):
        vals = [perf_lookup[m][f'geomean_{field}_vs_dynamic'] for m in MODES]
        ax.bar(x + (index-1.5)*width, vals, width, label=label)
    ax.axhline(1, color='#334155', ls='--', lw=1)
    ax.set(xticks=x, xticklabels=[LABELS[m] for m in MODES], ylabel='Geometric mean × Dynamic',
           title='Whole-process perf stat (includes initialization)')
    ax.tick_params(axis='x', rotation=25); ax.legend(ncol=4, frameon=False); ax.grid(axis='y', alpha=.2)
    save(fig, output, 'perf-stat-summary')

    # Weighted presence in on-CPU folded stacks. A sample contributes to a
    # category when any frame in its stack matches the named component.
    hot_by_mode = collections.defaultdict(list)
    for row in hotspots: hot_by_mode[row['mode']].append(row)
    fields = [('growbuffer_stack_percent', 'GrowBuffer'),
              ('register_update_stack_percent', 'io_uring register update'),
              ('selective_claim_stack_percent', 'Selective claim'),
              ('metrics_trace_stack_percent', 'Metrics trace')]
    fig, axes = plt.subplots(2, 2, figsize=(11, 7.2), constrained_layout=True)
    for ax, (field, title) in zip(axes.flat, fields):
        vals = [statistics.median(float(row[field]) for row in hot_by_mode[m]) for m in MODES]
        ax.bar(range(len(MODES)), vals, color=[COLORS[m] for m in MODES])
        ax.set(xticks=range(len(MODES)), xticklabels=[LABELS[m] for m in MODES],
               ylabel='Median weighted stack presence (%)', title=title)
        ax.tick_params(axis='x', rotation=35, labelsize=7); ax.grid(axis='y', alpha=.2)
    save(fig, output, 'flamegraph-hotspot-summary')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--metrics-summary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not (args.results / 'SUCCESS').is_file():
        parser.error('results are incomplete: SUCCESS is missing')
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((args.results / 'manifest.json').read_text())
    queries = [f'q{i:02d}' for i in range(44)]
    medians, per_query = load_timed(args.results)
    expected = {(q, mode) for q in queries for mode in MODES}
    if set(medians) != expected:
        parser.error(f'incomplete query/mode matrix: {len(medians)} of {len(expected)}')
    aggregate = aggregate_modes(medians, queries)
    metrics_path = args.metrics_summary or args.output / 'selective-v2-metrics-summary.csv'
    if not metrics_path.is_file():
        parser.error(f'metrics summary is missing: {metrics_path}')
    metrics = load_metrics(metrics_path)
    perf = load_perf(args.results, queries)
    hotspots = load_hotspots(args.results)
    deltas = top_deltas(medians, queries)
    write_csv(args.output / 'per-query-mode-summary.csv', per_query)
    write_csv(args.output / 'aggregate-mode-summary.csv', aggregate)
    write_csv(args.output / 'selective-policy-summary.csv', metrics)
    write_csv(args.output / 'perf-mode-summary.csv', perf)
    write_csv(args.output / 'flamegraph-hotspots.csv', hotspots)
    write_csv(args.output / 'ablation-query-deltas.csv', deltas)
    figures(args.output, medians, queries, aggregate, metrics, perf, deltas, hotspots)
    metadata = {
        'results': str(args.results), 'binary_sha256': manifest.get('binary_sha256'),
        'queries': len(queries), 'modes': len(MODES), 'timed_runs': 1188,
        'perf_profiles': len(list(args.results.glob('q*-*-r0/perf-stat.csv'))),
        'metrics_case_prefixes': len(list(args.results.glob('q*-*-r*/selective-*.events.csv'))),
    }
    (args.output / 'analysis-metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    main()
