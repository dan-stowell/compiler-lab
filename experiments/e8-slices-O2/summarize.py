#!/usr/bin/env python3
"""Summarize E8's per-file CSVs: median/p90 smallest-matching slice fraction,
per-file medians, distribution of d, controls, and the add-callers variant."""
import os
import statistics
import sys
from collections import Counter


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]  # nearest-rank (lower)


allrows, meta = [], {}
for path in sys.argv[1:]:
    name = os.path.basename(path)[:-4]
    lines = open(path).read().splitlines()
    meta[name] = dict(kv.split("=") for kv in lines[0].lstrip("# ").split())
    for ln in lines[2:]:
        r = ln.split(",")
        allrows.append(dict(file=name, t=r[0], d=r[1], frac=float(r[2]), tok=int(r[3]),
                            rd=r[4], rb=r[5], cd=r[7], cfrac=float(r[8]), lev=r[9],
                            clev=r[10], inl=r[11]))

S = lambda k: sum(int(m[k]) for m in meta.values())
ctl_ok = all(m["control_recompile"].split("/")[0] == m["control_recompile"].split("/")[1]
             and m["control_rejoin"].split("/")[0] == m["control_rejoin"].split("/")[1]
             for m in meta.values())
fr = [r["frac"] for r in allrows]
print(f"files={len(meta)} targets={len(allrows)} tu_tokens_total={S('total_tokens')}")
print(f"static defs excluded={S('n_static_defs')} (named in the .c source: "
      f"{S('static_defs_named_in_source')}; emitted standalone in full asm: "
      f"{S('static_emitted_in_full_asm')})")
print(f"controls (recompile and rejoin reproduce every emitted function, all files): {ctl_ok}")
print(f"median slice frac={statistics.median(fr):.2%}  p90={pct(fr, 0.9):.2%}  "
      f"mean={statistics.mean(fr):.2%}  max={max(fr):.2%}")
print("d distribution:", dict(sorted(Counter(r["d"] for r in allrows).items(),
                                     key=lambda kv: (kv[0] == "never", int(kv[0]) if kv[0] != "never" else 0))))
cf = sum("compile_fail" in r["lev"] for r in allrows)
print(f"targets with any compile_fail level: {cf}")
print("per-file (targets, median, p90, max, file tokens, controls):")
for name in sorted(meta):
    rs = [r["frac"] for r in allrows if r["file"] == name]
    m = meta[name]
    if rs:
        print(f"  {name:10s} n={len(rs):3d} median={statistics.median(rs):6.2%} p90={pct(rs, 0.9):6.2%} "
              f"max={max(rs):6.2%} tokens={m['total_tokens']} ctl={m['control_recompile']},{m['control_rejoin']}")
    else:
        print(f"  {name:10s} n=  0 (no emitted extern functions)")
# add-callers variant
better = [r for r in allrows if r["d"] != "0" and
          (r["d"] == "never" or (r["cd"] != "never" and int(r["cd"]) < int(r["d"])))]
d0 = sum(r["d"] == "0" for r in allrows)
cfr = [r["cfrac"] for r in allrows]
print(f"add-callers variant: median frac={statistics.median(cfr):.2%} p90={pct(cfr, 0.9):.2%}; "
      f"d distribution: {dict(sorted(Counter(r['cd'] for r in allrows).items()))}")
print(f"  of {len(allrows) - d0} targets needing d>0, {len(better)} match at a smaller d with callers added; "
      f"{sum(r['cfrac'] < r['frac'] for r in allrows)} get a smaller slice")
print("largest slices:")
for r in sorted(allrows, key=lambda r: -r["frac"])[:8]:
    print(f"  {r['file']}:{r['t']} d={r['d']} (closure depth {r['rd']}, {r['rb']} bodies) "
          f"frac={r['frac']:.2%} callers-variant d={r['cd']} frac={r['cfrac']:.2%}; inlined: {r['inl'] or '-'}")
med = statistics.median(fr)
print("decision:", "median <= 10%: slice survives -O2" if med <= 0.10
      else "median > 10%: optimizer couples functions too tightly (-> ThinLTO-style summaries)")
