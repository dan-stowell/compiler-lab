"""Fit log2(visits) = alpha*log2(N) + c per key-space size, from results/join.txt."""
import math, re, sys
rows = {}
for line in open(sys.argv[1]):
    kv = dict(re.findall(r"(\w+)=(\S+)", line))
    n, w = int(kv["N"]), int(kv["W"])
    kind = "full" if w == 2**64 else ("4N" if w == 4 * n else ("1024N" if w == 1024 * n else "?"))
    v = float(kv.get("visits") or kv["visits_est"])
    src = "measured" if line.startswith("join") else "estimated"
    rows.setdefault(kind, []).append((n, v, src, kv.get("count") or kv.get("join_count_est")))
def fit(pts):
    xs = [math.log2(n) for n, _ in pts]; ys = [math.log2(v) for _, v in pts]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    a = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    return a
for kind in ("4N", "1024N", "full"):
    r = sorted(rows[kind], key=lambda t: (t[0], t[2]))
    print(f"W={kind}")
    for n, v, src, c in r:
        print(f"  N=2^{int(math.log2(n)):2d} {src:9s} visits={v:.4e} visits/N={v/n:.4f} join_count={c}")
    m = [(n, v) for n, v, s, _ in r if s == "measured"]
    e = [(n, v) for n, v, s, _ in r if s == "estimated"]
    print(f"  exponent: measured 2^16..2^30 = {fit(m):.4f}; measured+estimated 2^16..2^40 = {fit(m + [p for p in e if p[0] > 2**30]):.4f}")
