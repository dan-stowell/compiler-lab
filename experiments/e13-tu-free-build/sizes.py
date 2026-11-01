#!/usr/bin/env python3
"""Function sizes in two linked binaries (nm -S). Duplicate local copies of a
static are summed and counted. Prints .text totals and the 20 largest base
functions, plus every function whose size changed by >= 25% and >= 64 bytes."""
import subprocess
import sys
from collections import defaultdict


def sizes(binary):
    s, n = defaultdict(int), defaultdict(int)
    out = subprocess.run(["nm", "-S", "--defined-only", binary], capture_output=True, text=True).stdout
    for ln in out.split("\n"):
        p = ln.split()
        if len(p) == 4 and p[2] in "tT":
            s[p[3]] += int(p[1], 16); n[p[3]] += 1
    return s, n


(a, na), (b, nb) = sizes(sys.argv[1]), sizes(sys.argv[2])
print(f"total function bytes: base {sum(a.values())}, tufree {sum(b.values())} "
      f"({sum(b.values()) / sum(a.values()):.3f}x)")
print("largest base functions: name base tufree [copies]")
for f in sorted(a, key=lambda f: -a[f])[:20]:
    print(f"  {f:28s} {a[f]:6d} {b.get(f, 0):6d}" + (f" [{nb[f]} copies]" if nb.get(f, 0) > 1 else ""))
print("changed >= 25% and >= 64 bytes (absent = fully inlined everywhere / dropped):")
for f in sorted(set(a) | set(b), key=lambda f: -(abs(b.get(f, 0) - a.get(f, 0)))):
    x, y = a.get(f, 0), b.get(f, 0)
    if abs(y - x) >= 64 and abs(y - x) >= 0.25 * max(x, 1):
        print(f"  {f:28s} {x:6d} {y:6d}" + (f" [{nb[f]} copies]" if nb.get(f, 0) > 1 else ""))
