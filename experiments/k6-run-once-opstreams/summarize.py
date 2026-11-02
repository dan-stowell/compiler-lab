#!/usr/bin/env python3
"""Summarize K6 results/raw.txt: per-stream cycles/op (medians over repetitions), the Step 0
gate, and the crossover of the predictability sweep."""
import math
import statistics as st
import sys
from collections import defaultdict

runs = defaultdict(lambda: defaultdict(list))  # (stream, mode) -> counter -> [per-op values]
loads = []
for line in open(sys.argv[1]):
    if '|' not in line:
        continue
    left, right = line.split('|', 1)
    f = left.split()
    group = f[1]
    kv = dict(x.split('=', 1) for x in f[2:] if '=' in x)
    loads.append(float(kv['load1']))
    if 'Q' in kv:
        stream, n = 'syn Q=%s' % kv['Q'], int(kv['nops'])
    else:
        stream, n = kv['prog'].split('/')[-1].replace('.k6p', ''), int(kv['recs'])
    if kv.get('state', 'OK') != 'OK':
        sys.exit('state FAIL in ' + line)
    for c in right.split():
        k, v = c.split('=')
        k = k.replace(':u', '')
        if k == 'cycles':
            k = 'cycles' + group
        runs[(stream, kv['mode'])][k].append(int(v) / n)

def med(stream, mode, k):
    v = runs.get((stream, mode), {}).get(k)
    return st.median(v) if v else float('nan')

def spread(stream, mode, k):
    v = runs[(stream, mode)][k]
    return min(v), max(v), len(v)

streams = sorted({s for s, _ in runs}, key=lambda s: (not s.startswith('syn'), int(s.split('=')[1]) if s.startswith('syn') else 0, s))
print('load1 during runs: min %.2f median %.2f max %.2f' % (min(loads), st.median(loads), max(loads)))
print('\n== per stream (per op; cycles from group A, median; jit includes generation)')
print('%-14s %-7s %8s %15s %7s %8s %8s %8s' % ('stream', 'mode', 'cyc/op', '[min-max] n', 'ins/op', 'brmis/op', 'icL2/op', 'deredir/op'))
for s in streams:
    for m in ('cgoto', 'pred', 'hybrid', 'jit', 'gen'):
        if (s, m) not in runs:
            continue
        lo, hi, n = spread(s, m, 'cyclesA')
        print('%-14s %-7s %8.2f %6.2f-%-6.2f %2d %7.2f %8.3f %8.3f %8.3f' % (
            s, m, med(s, m, 'cyclesA'), lo, hi, n, med(s, m, 'instructions'), med(s, m, 'branch-misses'),
            med(s, m, 'ic_cache_fill_l2'), med(s, m, 'bp_de_redirect')))

print('\n== Step 0 gate: est_interp = 3 + 20 x cgoto misses/op; kill if est < 2 x (gen + 1)')
print('%-14s %9s %10s %8s %9s %6s | %9s %8s %8s' % ('stream', 'mis/op', 'est_interp', 'gen', '2(gen+1)', 'gate', 'best_base', 'jit', 'best/jit'))
sweep = []
for s in streams:
    mis = med(s, 'cgoto', 'branch-misses')
    est = 3 + 20 * mis
    gen = med(s, 'gen', 'cyclesA')
    rhs = 2 * (gen + 1)
    bases = [(med(s, m, 'cyclesA'), m) for m in ('cgoto', 'pred', 'hybrid') if (s, m) in runs]
    best, bm = min(bases)
    jit = med(s, 'jit', 'cyclesA')
    print('%-14s %9.3f %10.2f %8.2f %9.2f %6s | %6.2f %-3s %7.2f %8.2f' % (
        s, mis, est, gen, rhs, 'pass' if est >= rhs else 'KILL', best, bm[:3], jit, best / jit))
    if s.startswith('syn'):
        q = int(s.split('=')[1]) / 1000
        p = [q + (1 - q) / 16] + [(1 - q) / 16] * 15
        H = -sum(x * math.log2(x) for x in p if x > 0)
        sweep.append((H, mis, best, jit, best / jit))

print('\n== predictability sweep (synthetic; entropy of the opcode distribution)')
print('%8s %9s %9s %8s %9s' % ('H bits', 'mis/op', 'best_base', 'jit', 'best/jit'))
for row in sweep:
    print('%8.3f %9.3f %9.2f %8.2f %9.2f' % row)
sweep.sort(key=lambda r: r[1])
for target in (1.0, 2.0):
    x = None
    for a, b in zip(sweep, sweep[1:]):
        if (a[4] - target) * (b[4] - target) <= 0 and a[4] != b[4]:
            t = (target - a[4]) / (b[4] - a[4])
            x = (a[1] + t * (b[1] - a[1]), a[0] + t * (b[0] - a[0]))
            break
    if x:
        print('best/jit = %.0f at cgoto misses/op ~ %.3f (entropy ~ %.2f bits), linear interpolation' % (target, x[0], x[1]))
    else:
        print('best/jit = %.0f not crossed in the sweep' % target)
