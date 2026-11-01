"""E12 step 3-4: hot/cold frontier and decision.

usage: python3 frontier.py PROFILE_CSV CG_TOTAL_IR NATIVE_S NS_PER_INSTR TABLE_B DECODER_TEXT_B
                           PAYLOAD_B TEXT_B SYMMAP_B OUT_CSV

For k = 0..324, the k chibicc functions with the most self Ir stay native; the rest run from
E11's payload (the whole payload stays resident: conservative, it is not trimmed to the cold set).
  cold_Ir(k)      = sum of self Ir of the 324-k cold chibicc functions (measured, callgrind)
  hot_fraction(k) = 1 - cold_Ir(k) / total_Ir   (all Ir: chibicc + libc + ld.so; assumes native
                    time is proportional to Ir, so non-chibicc time stays native)
  ratio(k)        = hot_fraction(k) + cold_Ir(k) * c / T_native
                    (c = per-guest-instruction cost; one guest instruction = one x86 instruction
                    of the original stage2 code, so cold_Ir counts guest instructions)
  resident(k)     = payload + native bytes of the k hot functions + symbol map + decoder tables
                    + decoder .text
c_opt = measured C decode ns/instr (decode only); c_pess = 3 * c_opt.
"""
import csv, sys

prof, total, T, c, tab, dtext, pay, text, symmap, out = sys.argv[1:11]
total, T, c = int(total), float(T), float(c) * 1e-9
tab, dtext, pay, text, symmap = map(int, (tab, dtext, pay, text, symmap))
rows = [r for r in csv.DictReader(open(prof)) if r['kind'] == 'chibicc']
rows.sort(key=lambda r: int(r['rank']))
assert len(rows) == 324
fixed = pay + symmap + tab + dtext
cold = sum(int(r['self_ir']) for r in rows)
hotb = 0
res = []
with open(out, 'w', newline='') as o:
    w = csv.writer(o)
    w.writerow(['k', 'added_function', 'added_self_ir', 'hot_native_bytes', 'cold_ir', 'hot_fraction',
                'est_ratio_optimistic', 'est_ratio_pessimistic', 'resident_bytes', 'resident_fraction'])
    for k in range(325):
        if k:
            r = rows[k - 1]
            cold -= int(r['self_ir']); hotb += int(r['native_bytes'])
        hf = 1 - cold / total
        ro, rp = hf + cold * c / T, hf + cold * 3 * c / T
        rb = fixed + hotb
        res.append((k, ro, rp, rb, cold))
        w.writerow([k, rows[k - 1]['function'] if k else '', rows[k - 1]['self_ir'] if k else '',
                    hotb, cold, f'{hf:.6f}', f'{ro:.4f}', f'{rp:.4f}', rb, f'{rb / text:.4f}'])

print(f"fixed resident = payload {pay} + symbol map {symmap} + decoder tables {tab} + decoder .text {dtext} "
      f"= {fixed} B = {fixed / text:.1%} of native .text {text} B (the k=0 floor)")
print(f"k=0 (all chibicc interpreted): ratio optimistic {res[0][1]:.1f}x, pessimistic {res[0][2]:.1f}x")
allb = sum(int(r['native_bytes']) for r in rows)
best = {}
for name, i in (('optimistic', 1), ('pessimistic', 2)):
    ok = [x for x in res if x[i] <= 1.5]
    b = min(ok, key=lambda x: x[3])
    best[name] = b[3] / text
    print(f"{name} (c={c * 1e9 * (1 if i == 1 else 3):.2f} ns/instr): smallest resident at ratio<=1.5: k={b[0]} "
          f"hot functions, ratio {b[i]:.3f}, cold Ir {b[4]} ({b[4] / total:.3%} of total), resident {b[3]} B "
          f"= {b[3] / text:.1%} of native .text")
    hb = b[3] - fixed
    trim = pay * (1 - hb / allb)
    print(f"  sensitivity (ESTIMATE): payload + hot native only (no symbol map, decoder tables or code) = "
          f"{(pay + hb) / text:.1%}; payload trimmed to cold functions pro rata by native bytes "
          f"({trim:.0f} B) + hot native only = {(trim + hb) / text:.1%}")
if best['optimistic'] > 0.5:
    d = "optimistic needs >50% of native .text resident: executable compression buys little RAM here; stop"
elif best['pessimistic'] <= 0.3:
    d = "pessimistic reaches <=30%: build the real hot/cold interpreter next"
else:
    d = "in between: build the decode+dispatch core and remeasure the per-instruction cost"
print("DECISION:", d)
