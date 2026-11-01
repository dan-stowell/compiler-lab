#!/usr/bin/env python3
"""Summarize E5 CSVs. Usage: summarize.py <tu.i> <strict.csv> <stubs.csv>"""
import statistics
import sys
from collections import Counter

import slice as sl


def load(path):
    rows = [ln.rstrip("\n").split(",") for ln in open(path) if not ln.startswith("#")]
    hdr = rows[0]
    return [dict(zip(hdr, r)) for r in rows[1:]]


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]  # nearest-rank, lower


def report(name, rows):
    eff = [float(r["effective_frac"]) for r in rows]
    raw = [float(r["slice_frac"]) for r in rows]
    ok = [r for r in rows if r["status"] == "ok"]
    print(f"== {name}: {len(rows)} functions, {len(ok)} verified, {len(rows) - len(ok)} failed")
    for c, k in Counter(r["cause"] for r in rows if r["status"] != "ok").most_common():
        print(f"   failed x{k}: {c}")
    print(f"   effective (failed=100%): median {statistics.median(eff):.2%}  p90 {pct(eff, 0.9):.2%}")
    print(f"   raw slice size:          median {statistics.median(raw):.2%}  p90 {pct(raw, 0.9):.2%}"
          f"  max {max(raw):.2%}")
    ctx = [float(r["context_frac"]) for r in rows]
    print(f"   context only (excl. target body): median {statistics.median(ctx):.2%}")
    return max(rows, key=lambda r: float(r["slice_frac"]))


def main():
    tu, strict, stubs = sys.argv[1:4]
    report("strict (prototypes only, as specified)", load(strict))
    big = report("stubs (callees as header+{})", load(stubs))
    src = open(tu).read()
    decls = sl.split(sl.tokenize(src))
    total = sum(len(d.toks) for d in decls)
    tgt = next(d for d in decls if d.is_func and d.name == big["func"])
    inc, protos = sl.closure(decls, tgt, True)
    print(f"== largest slice: {big['func']} {float(big['slice_frac']):.2%} "
          f"({big['slice_tokens']}/{total} tokens; own body {big['func_tokens']}; "
          f"{len(inc)} decls + {len(protos)} stubs)")
    parts = [(len(decls[i].toks), " ".join(t for _, t, _, _ in decls[i].toks[:6])) for i in inc]
    parts += [(len(decls[i].header) + 2, "stub " + decls[i].name) for i in protos]
    for n, head in sorted(parts, reverse=True)[:6]:
        print(f"   {n:5d} tok  {head} ...")
    sizes = sorted(((len(d.toks), d.name if d.is_func else
                     " ".join(t for _, t, _, _ in d.toks[:4])) for d in decls), reverse=True)
    nonfn = total - sum(len(d.toks) for d in decls if d.is_func)
    ctx = [int(r["slice_tokens"]) - int(r["func_tokens"]) for r in load(stubs)]
    print(f"== TU {total} tokens, {nonfn} outside function definitions; stubs-mode context "
          f"median {statistics.median(ctx):.0f} tok = {statistics.median(ctx) / nonfn:.2%} of those")
    print("== largest top-level decls in TU:", "; ".join(f"{n} {s}" for n, s in sizes[:5]))


if __name__ == "__main__":
    main()
