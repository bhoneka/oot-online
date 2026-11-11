#!/usr/bin/env python3
"""Frame-time figures of a test run from the host's per-frame rows (DM_NVN_TSV=<run>/frames.tsv): for a window of seconds since
the host started (@t_s), percentiles of the frame time and where it goes. The GPU span is @gpu_end_ms - @gpu_beg_ms (the real
time from the frame's first GPU work to its last; @gpu_ms adds up command buffers, which overlap).
    frames_stats.py <frames.tsv> [from_s] [to_s]        (default: the whole run, split in 30 s windows)"""
import math, sys
from collections import defaultdict

def pct(xs, p):
    if not xs: return float('nan')
    s = sorted(xs); return s[min(len(s) - 1, max(0, math.ceil(p / 100 * len(s)) - 1))]

rows = defaultdict(dict)
for ln in open(sys.argv[1], errors='replace'):
    c = ln.rstrip('\n').split('\t')
    if len(c) != 3 or not c[1].startswith('@'): continue
    try: rows[int(c[0])][c[1]] = float(c[2])
    except ValueError: pass
fr = [r for _, r in sorted(rows.items()) if '@t_s' in r]
if not fr: sys.exit('no frames')
t_end = fr[-1]['@t_s']
wins = [(float(sys.argv[2]), float(sys.argv[3]) if len(sys.argv) > 3 else t_end + 1)] if len(sys.argv) > 2 else [(t, t + 30) for t in range(0, int(t_end) + 1, 30)]
keys = ['@frame_ms', '@work_ms', '@sync_ms', '@nvn_ms', '@sub_wall_ms', '@gpu_ms', 'span', '@draws', '@passes', '@pso_new']
print(f"{'window s':>11} {'frames':>6} {'fps':>5} " + ' '.join(f'{k.lstrip("@"):>16}' for k in keys) + '   (p50/p95)')
for a, b in wins:
    sel = [r for r in fr if a <= r['@t_s'] < b]
    if len(sel) < 10: continue
    for r in sel:
        if '@gpu_end_ms' in r and '@gpu_beg_ms' in r: r['span'] = r['@gpu_end_ms'] - r['@gpu_beg_ms']
    fps = len(sel) / max(1e-9, sel[-1]['@t_s'] - sel[0]['@t_s'])
    cells = []
    for k in keys:
        xs = [r[k] for r in sel if k in r]
        cells.append(f'{pct(xs,50):7.2f}/{pct(xs,95):<8.2f}' if xs else f'{"-":>16}')
    over = sum(1 for r in sel if r.get('@frame_ms', 0) > 25)
    print(f'{a:5.0f}-{b:<5.0f} {len(sel):6d} {fps:5.1f} ' + ' '.join(cells) + f'   >25ms: {over}')
    if len(wins) == 1:   # one window asked for: the tail of its frame times too
        ft = sorted(r['@frame_ms'] for r in sel if '@frame_ms' in r)
        print(f'frame time: mean {sum(ft) / len(ft):.3f} ms ({1000 * len(ft) / sum(ft):.2f} fps), p99 {pct(ft, 99):.2f}, p99.9 {pct(ft, 99.9):.2f}, max {ft[-1]:.1f} ms; '
              f'over 20 ms {sum(x > 20 for x in ft)}, over 25 ms {over}, over 33 ms {sum(x > 33.4 for x in ft)}')
