#!/usr/bin/env python3
"""Summarise results.tsv (one line per expression from `e3 solve`) into markdown on stdout."""
import statistics as st, sys
rows = []
for line in open(sys.argv[1] if len(sys.argv) > 1 else "results.tsv"):
    f = line.rstrip("\n").split("\t")
    rows.append(dict(idx=int(f[0]), expr=f[1], dlen=int(f[2]), slen=int(f[3]), status=f[4], cpu=float(f[5]),
                     rss=int(f[6]), states=int(f[7]), children=int(f[8]), dprog=f[9], fprog=f[10]))
rows.sort(key=lambda r: r["idx"])
ok = [r for r in rows if r["status"] != "timeout"]
ratios = [r["slen"] / r["dlen"] for r in ok]
shorter = [r for r in ok if r["slen"] < r["dlen"]]
stackops = [r for r in shorter if any(op in r["fprog"].split("; ") for op in ("DUP", "SWAP", "OVER"))]
cpus = [r["cpu"] for r in rows]
mean = st.mean(ratios)
print(f"- expressions: {len(rows)}; timeouts (failures): {len(rows) - len(ok)}")
print(f"- mean length ratio (search/derived): {mean:.4f}")
print(f"- median length ratio: {st.median(ratios):.4f}")
print(f"- search strictly shorter: {len(shorter)}/{len(ok)} (of which {len(stackops)} use DUP/SWAP/OVER)")
print(f"- mean derived length {st.mean(r['dlen'] for r in ok):.2f}, mean search length {st.mean(r['slen'] for r in ok):.2f}")
print(f"- search CPU: total {sum(cpus):.1f} s; per expression mean {st.mean(cpus):.3f} s, median {st.median(cpus):.3f} s, max {max(cpus):.2f} s")
print(f"- peak RSS (max over per-expression processes): {max(r['rss'] for r in rows) / 1024:.1f} MiB")
print(f"- distinct states stored: total {sum(r['states'] for r in rows):,}, max per expression {max(r['states'] for r in rows):,};"
      f" child states examined: total {sum(r['children'] for r in rows):,}")
print("\nBy derived length:\n\n| derived len | n | mean search len | mean ratio | strictly shorter | mean CPU s |\n|---|---|---|---|---|---|")
for L in sorted({r["dlen"] for r in ok}):
    g = [r for r in ok if r["dlen"] == L]
    print(f"| {L} | {len(g)} | {st.mean(r['slen'] for r in g):.2f} | {st.mean(r['slen']/L for r in g):.3f} | "
          f"{sum(r['slen'] < L for r in g)} | {st.mean(r['cpu'] for r in g):.3f} |")
print("\nSearch length histogram: " + ", ".join(f"{k}: {sum(r['slen']==k for r in ok)}" for k in sorted({r['slen'] for r in ok})))
print(f"\nDecision rule (mean ratio <= 0.75): {'MET' if mean <= 0.75 else 'NOT MET'}")
# striking: biggest absolute saving, prefer non-trivial (longer) found programs, then ones using stack ops
cand = sorted(shorter, key=lambda r: (r["dlen"] - r["slen"], r in stackops, r["slen"]), reverse=True)
print("\nCandidate striking examples (largest saving first):\n")
for r in cand[:10]:
    print(f"- #{r['idx']} `{r['expr']}`: derived ({r['dlen']}) `{r['dprog']}` -> found ({r['slen']}) `{r['fprog']}`")
print("\nShorter programs using stack ops:\n")
for r in stackops:
    print(f"- #{r['idx']} `{r['expr']}`: derived ({r['dlen']}) -> found ({r['slen']}) `{r['fprog']}`")
