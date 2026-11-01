#!/usr/bin/env python3
"""summarize.py <ripple.csv> <work>: medians, p90s, exclusions, compile CPU, decisions."""
import csv
import glob
import statistics
import sys

E2_BODY, E2_CLOSURE = 0.0154, 0.2162


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]  # nearest-rank (same as E2)


rows = list(csv.DictReader(open(sys.argv[1])))
work = sys.argv[2]
ok = [r for r in rows if r["status"] == "ok"]
exc = [r for r in rows if r["status"].startswith("excluded")]
raw = [float(r["raw_frac"]) for r in ok]
norm = [float(r["norm_frac"]) for r in ok]
cpu, nfail, njobs = 0.0, 0, 0
for f in glob.glob(f"{work}/asm/*.cpu"):
    njobs += 1
    parts = open(f).read().split()
    try:
        cpu += float(parts[-2]) + float(parts[-1])
    except (IndexError, ValueError):
        pass
nfail = len(glob.glob(f"{work}/asm/*.fail"))
print(f"== {sys.argv[1]}: {len(rows)} commits, {len(rows) - 1} transitions")
print(f"included transitions: {len(ok)}; excluded: {len(exc)} "
      f"(self fails {sum(r['status'].startswith('excluded:self') for r in exc)}, "
      f"parent fails {sum(r['status'].startswith('excluded:parent') for r in exc)})")
print(f"distinct compiles: {njobs}, failed: {nfail}, compile CPU total (user+sys): {cpu:.1f}s")
if not ok:
    sys.exit("no included transitions")
mr, mn = statistics.median(raw), statistics.median(norm)
print(f"raw  asm change frac: median {mr:.4f}  p90 {pct(raw, .9):.4f}  mean {statistics.mean(raw):.4f}")
print(f"norm asm change frac: median {mn:.4f}  p90 {pct(norm, .9):.4f}  mean {statistics.mean(norm):.4f}")
nl = [float(r["noloc_frac"]) for r in ok]
print(f"diagnostic, strip .loc/.file only (no label renumbering): median {statistics.median(nl):.4f}  "
      f"p90 {pct(nl, .9):.4f}")
nlit = [float(r["normlit_frac"]) for r in ok]
print(f"normalized + referenced literal/static contents: median {statistics.median(nlit):.4f}  "
      f"p90 {pct(nlit, .9):.4f}  mean {statistics.mean(nlit):.4f}")
print(f"functions at last included commit: {ok[-1]['nfuncs']}; "
      f"added total {sum(int(r['added']) for r in ok)}, removed total {sum(int(r['removed']) for r in ok)}")
print(f"E2 reference (source-level): body median {E2_BODY:.4f}, closure median {E2_CLOSURE:.4f}")
print(f"raw - norm medians: {100 * (mr - mn):.1f} points")
print("DECISION 1:", "normalized median <= 3% -> output addressing removes closure ripple"
      if mn <= 0.03 else "normalized median > 3% -> output addressing does NOT remove the ripple")
print("DECISION 1b (with literal contents):", "<= 3%" if statistics.median(nlit) <= 0.03 else "> 3%")
print("DECISION 2:", "raw - normalized > 5 points -> hidden global counters cause ripple"
      if mr - mn > 0.05 else "raw - normalized <= 5 points -> global counters are not the main ripple source")
