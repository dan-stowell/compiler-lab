#!/usr/bin/env python3
"""E13 analysis of perf.csv rows: round,bench,variant,value,unit,event,stddev%,...
(perf stat -x, -r 10: each row is the mean of 10 runs).

Per benchmark the value is the mean over rounds. For each variant vs `base`:
- geomean ratio over the benchmarks;
- CI_bench: 95% t-interval of the mean log ratio across the 5 benchmarks (df=4);
- CI_meas: 95% normal interval from measurement noise only (spread of the round
  means, se = sd/sqrt(rounds), delta method on the log ratio).
Decision (plan rule, on instructions, CI_bench, plus test results given in argv[2]):
  upper <= 1.03 viable at no measurable cost; lower > 1.10 load-bearing;
  1.03 < lower and upper <= 1.10 viable at a measured cost; else inconclusive."""
import csv
import math
import statistics
import sys

data = {}
for row in csv.reader(open(sys.argv[1])):
    r, b, v, val, _, ev = row[:6]
    ev = ev.split(":")[0]
    data.setdefault((ev, b, v), []).append(float(val))
benches = sorted({k[1] for k in data})
variants = sorted({k[2] for k in data} - {"base"})
T4 = 2.776  # t_{0.975, 4}


def summary(ev, v):
    logs, var = [], 0.0
    for b in benches:
        xb, xv = data[(ev, b, "base")], data[(ev, b, v)]
        mb, mv = statistics.mean(xb), statistics.mean(xv)
        logs.append(math.log(mv / mb))
        for xs, m in ((xb, mb), (xv, mv)):
            if len(xs) > 1:
                var += (statistics.stdev(xs) / math.sqrt(len(xs)) / m) ** 2
    g = statistics.mean(logs)
    sb = statistics.stdev(logs) / math.sqrt(len(logs))
    sm = math.sqrt(var) / len(logs)
    return [math.exp(x) for x in logs], math.exp(g), \
        (math.exp(g - T4 * sb), math.exp(g + T4 * sb)), (math.exp(g - 1.96 * sm), math.exp(g + 1.96 * sm))


for ev in ("instructions", "cycles"):
    print(f"== {ev} (ratio variant/base; benchmarks: {' '.join(benches)})")
    for v in variants:
        per, g, cb, cm = summary(ev, v)
        print(f"{v:7s} per-bench {' '.join(f'{x:.4f}' for x in per)}  geomean {g:.4f}  "
              f"CI_bench [{cb[0]:.4f}, {cb[1]:.4f}]  CI_meas [{cm[0]:.4f}, {cm[1]:.4f}]")
    noise = [summary(ev, v)[1] for v in variants if v.startswith("shuf")]
    if noise:
        print(f"noise floor (shuffled link orders vs base): geomean ratios "
              f"{' '.join(f'{x:.4f}' for x in noise)}; max |dev| {max(abs(x - 1) for x in noise) * 100:.2f}%")
_, g, (lo, hi), _ = summary("instructions", "tufree")
tests_ok = len(sys.argv) < 3 or sys.argv[2] == "pass"
if not tests_ok:
    d = "cross-function knowledge load-bearing OR transformation bug (test failure)"
elif hi <= 1.03:
    d = "viable at no measurable cost"
elif lo > 1.10:
    d = "cross-function knowledge within the TU is load-bearing"
elif lo > 1.03 and hi <= 1.10:
    d = "viable at a measured cost"
else:
    d = "inconclusive (interval straddles a boundary)"
print(f"DECISION (instructions, CI_bench [{lo:.4f}, {hi:.4f}], tests {'pass' if tests_ok else 'FAIL'}): {d}")
