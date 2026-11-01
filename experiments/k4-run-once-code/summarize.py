"""Summarize K4 raw runs: medians per config, agreement check, derived costs, rule."""
import sys
from collections import defaultdict
from statistics import median

runs = defaultdict(list)  # (tag, mode, input, ring, chunk, N) -> [dict]
for line in open(sys.argv[1]):
    tok = line.split()
    d = {"tag": tok[0], "group": tok[1]}
    for t in tok[2:]:
        if "=" in t:
            k, v = t.split("=", 1)
            d[k.replace(":u", "")] = v
    key = (d["tag"], d["mode"], d.get("input", "-"), d.get("ring", "-"), d.get("chunk", "-"), d.get("N", "-"))
    runs[key].append(d)


def med(rs, field, group=None):
    xs = [float(r[field]) for r in rs if field in r and (group is None or r["group"] == group)]
    return median(xs) if xs else float("nan")


def spread(rs, field, group=None):
    xs = [float(r[field]) for r in rs if field in r and (group is None or r["group"] == group)]
    return (max(xs) - min(xs)) / median(xs) * 100 if xs else float("nan")


print("== noise control (fixed 2^28 splitmix chain, pinned) ==")
c = runs[("control", "control", "-", "-", "-", "-")]
print(f"runs={len(c)} median ns={med(c,'ns')/1e6:.1f} ms (range {spread(c,'ns'):.1f}% of median); "
      f"median cycles={med(c,'cycles')/1e6:.1f} M (range {spread(c,'cycles'):.1f}%); "
      f"load1 at runs: min {min(float(r['load1']) for r in c):.2f} max {max(float(r['load1']) for r in c):.2f}")
print(f"effective GHz (cycles/ns, median) = {med(c,'cycles')/med(c,'ns'):.2f}")

ok = True
for inp in ("seeded", "text:data/text.list"):
    main = {k: v for k, v in runs.items() if k[0] == "main" and k[2] == inp}
    if not main:
        continue
    N = float(next(iter(main))[5])
    tbl = next(v for k, v in main.items() if k[1] == "table")
    tcyc = med(tbl, "cycles", "A")
    results = {(r["state"], r["matches"]) for k, v in main.items() if k[1] != "gen" for r in v}
    agree = len(results) == 1
    ok &= agree
    print(f"\n== input {inp}, N={N:.0f} bytes; final (state, matches) over table/interp/jit = "
          f"{sorted(results)} -> {'AGREE' if agree else 'DISAGREE'} ==")
    print(f"{'mode':7}{'ring':>7}{'chunk':>6} {'ns/B':>7}{'ns rng%':>8}{'cyc/B':>7}{'ins/B':>7}{'IPC':>6}"
          f"{'icL2/KiB':>9}{'icSys/MiB':>10}{'DRAM B/B':>9}{'code B/B':>9}{'x table':>8}")
    order = sorted(main.items(), key=lambda kv: (["table", "interp", "jit", "gen"].index(kv[0][1]), int(kv[0][3])))
    for k, v in order:
        cyc, ins = med(v, "cycles", "A"), med(v, "instructions", "A")
        dram = 64 * (med(v, "ls_any_fills_from_sys.dram_io_all", "B") + med(v, "l2_pf_miss_l2_l3.all", "B"))
        print(f"{k[1]:7}{k[3]:>7}{k[4]:>6} {med(v,'ns')/N:7.3f}{spread(v,'ns'):8.1f}{cyc/N:7.2f}{ins/N:7.2f}{ins/cyc:6.2f}"
              f"{med(v,'ic_cache_fill_l2','A')/(N/1024):9.2f}{med(v,'ic_cache_fill_sys','A')/(N/2**20):10.2f}"
              f"{dram/N:9.3f}{med(v,'gen_bytes')/N:9.2f}{cyc/tcyc:8.2f}")
    print("generation cost (gen mode = emit only) and jit split (exec = jit - gen, ESTIMATED by subtraction):")
    for ring in ("4096", "16384", "65536"):
        g = [v for k, v in main.items() if k[1] == "gen" and k[3] == ring][0]
        j = [v for k, v in main.items() if k[1] == "jit" and k[3] == ring][0]
        gi = med(g, "gen_instr")
        print(f"  ring {ring:>5}: gen {med(g,'ns')/N:.3f} ns/input B, {med(g,'ns')/gi:.3f} ns/gen instr, "
              f"{med(g,'cycles','A')/gi:.3f} cyc/gen instr, {gi/N:.3f} gen instr/input B; "
              f"exec(est) {(med(j,'cycles','A')-med(g,'cycles','A'))/N:.2f} cyc/B vs table {tcyc/N:.2f} cyc/B; "
              f"jit total {med(j,'cycles','A')/tcyc:.2f}x table cycles, {med(j,'ns')/med(tbl,'ns'):.2f}x table ns")

print("\n== failure curve: 128 MiB seeded, 64 KiB ring, chunk = n input bytes (3n+5 instr/chunk), group A ==")
cv = {k: v for k, v in runs.items() if k[0] == "curve"}
if cv:
    N = float(next(iter(cv))[5])
    tbl = next(v for k, v in cv.items() if k[1] == "table")
    tcyc = med(tbl, "cycles")
    print(f"table: {tcyc/N:.2f} cyc/B, {med(tbl,'ns')/N:.3f} ns/B")
    print(f"{'n':>5}{'instr/chunk':>12}{'jit cyc/B':>10}{'x table':>8}{'jit ns/B':>9}{'gen ns/B':>9}{'gen ns/instr':>13}"
          f"{'jit ns/chunk':>13}{'icL2/KiB':>9}")
    ns_ = sorted({int(k[4]) for k in cv if k[1] == "jit"})
    for n in ns_:
        j = cv[("curve", "jit", "seeded", "65536", str(n), str(int(N)))]
        g = cv[("curve", "gen", "seeded", "65536", str(n), str(int(N)))]
        res = {(r["state"], r["matches"]) for r in j} | {(r["state"], r["matches"]) for r in tbl}
        ok &= len(res) == 1
        print(f"{n:5d}{3*n+5:12d}{med(j,'cycles')/N:10.2f}{med(j,'cycles')/tcyc:8.2f}{med(j,'ns')/N:9.3f}"
              f"{med(g,'ns')/N:9.3f}{med(g,'ns')/med(g,'gen_instr'):13.3f}{med(j,'ns')/med(j,'chunks'):13.1f}"
              f"{med(j,'ic_cache_fill_l2')/(N/1024):9.2f}{'' if len(res)==1 else '  DISAGREE'}")
print("\nALL RESULTS AGREE" if ok else "\nDISAGREEMENT FOUND")
