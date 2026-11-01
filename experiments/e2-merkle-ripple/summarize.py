#!/usr/bin/env python3
"""Summarize ripple.csv: medians, p90, closure-ripple histogram, decision."""
import csv, statistics, sys

def pct(xs, q):
    xs = sorted(xs)
    k = (len(xs) - 1) * q
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)

for path in sys.argv[1:]:
    rows = list(csv.DictReader(open(path)))
    bf = [float(r["body_frac"]) for r in rows]
    cf = [float(r["closure_frac"]) for r in rows]
    rm = [int(r["n_removed"]) for r in rows]
    scc = [int(r["largest_scc"]) for r in rows]
    print(f"== {path}: {len(rows)} commit transitions")
    print(f"functions at last commit: {rows[-1]['n_funcs']}; largest SCC at last commit: {scc[-1]} (max over history {max(scc)})")
    print(f"body-hash change frac:   median {statistics.median(bf):.4f}  p90 {pct(bf,.9):.4f}  mean {statistics.mean(bf):.4f}")
    print(f"closure-key change frac: median {statistics.median(cf):.4f}  p90 {pct(cf,.9):.4f}  mean {statistics.mean(cf):.4f}")
    print(f"removed functions: total {sum(rm)}, commits with removals {sum(1 for x in rm if x)}")
    print(f"amplification (sum closure changed / sum body changed): "
          f"{sum(int(r['closure_changed']) for r in rows) / max(1, sum(int(r['body_changed']) for r in rows)):.2f}")
    later = [r for r in rows if int(r["n_funcs"]) >= 100]
    if later:
        lc = [float(r["closure_frac"]) for r in later]
        lb = [float(r["body_frac"]) for r in later]
        print(f"  commits with >=100 functions ({len(later)}): body median {statistics.median(lb):.4f}, closure median {statistics.median(lc):.4f}, p90 {pct(lc,.9):.4f}")
    print("closure ripple histogram:")
    edges = [0, 1e-9, .05, .10, .20, .30, .40, .50, .75, 1.0001]
    labels = ["0", "(0,5%]", "(5,10%]", "(10,20%]", "(20,30%]", "(30,40%]", "(40,50%]", "(50,75%]", "(75,100%]"]
    for lo, hi, lab in zip(edges, edges[1:], labels):
        n = sum(1 for x in cf if (x == 0 if lab == "0" else lo - 1e-12 < x <= hi) and not (lab != "0" and x == 0))
        print(f"  {lab:>10} {n:4d} {'#' * n}")
    med = statistics.median(cf)
    print("DECISION:", "median closure ripple <= 10% -> whole-closure addressing viable"
          if med <= 0.10 else "median closure ripple > 10% -> closure keys over-invalidate")
    print()
