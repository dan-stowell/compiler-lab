#!/usr/bin/env python3
"""E13 slice statistics from build/split/*/stats.csv: targets per file and
E8-style slice size (target body + bodies of the statics it reaches) as a share
of the file's preprocessed tokens."""
import csv
import glob
import os
import statistics
import sys

rows = []
for p in sorted(glob.glob(os.path.join(sys.argv[1], "*", "stats.csv"))):
    rows += list(csv.DictReader(open(p)))
per = {}
for r in rows:
    per.setdefault(r["file"], []).append(r)
print(f"targets: {len(rows)} in {len(per)} files")
print("targets per file: " + ", ".join(f"{f} {len(v)}" for f, v in sorted(per.items(), key=lambda x: -len(x[1]))))
pct = sorted(100 * int(r["slice_tokens"]) / int(r["tu_tokens"]) for r in rows)
ab = sorted(int(r["slice_tokens"]) for r in rows)
st = sorted(int(r["static_funcs_reached"]) for r in rows)
q = lambda xs, f: xs[min(len(xs) - 1, int(f * len(xs)))]
print(f"slice tokens / TU tokens: median {statistics.median(pct):.2f}%, p90 {q(pct, .9):.2f}%, max {pct[-1]:.2f}%")
print(f"slice tokens: median {statistics.median(ab):.0f}, p90 {q(ab, .9)}, max {ab[-1]}")
print(f"static functions reached per target: median {statistics.median(st):.0f}, max {st[-1]}")
