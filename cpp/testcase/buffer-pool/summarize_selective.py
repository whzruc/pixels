#!/usr/bin/env python3
"""Print medians without mixing query time, process RSS and buffer capacities."""
import csv
from collections import defaultdict
from pathlib import Path
import re
import statistics
import sys

groups = defaultdict(list)
for row in csv.DictReader((Path(sys.argv[1]) / 'summary.csv').open()):
    groups[row['query'], row['mode']].append(row)
print('| Query | Mode | N | Query median s | RSS median MiB | Observed buffer median MiB | Transfers total |')
print('|---|---|---:|---:|---:|---:|---:|')
for (query, mode), rows in sorted(groups.items()):
    times = [float(r['query_seconds']) for r in rows if r['query_seconds']]
    rss = [int(r['rss_kib']) / 1024 for r in rows if r['rss_kib']]
    buffers, transfers = [], []
    for row in rows:
        stats = dict(re.findall(r'(\w+)=(\d+)', row['selective_stats']))
        if 'observed_buffer_bytes' in stats:
            buffers.append(int(stats['observed_buffer_bytes']) / 1024**2)
        if 'transferred' in stats:
            transfers.append(int(stats['transferred']))
    def median(values):
        return f'{statistics.median(values):.4f}' if values else '—'
    print(f'| {query} | {mode} | {len(rows)} | {median(times)} | {median(rss)} | {median(buffers)} | {sum(transfers) if transfers else "—"} |')
