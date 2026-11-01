#!/usr/bin/env python3
"""Comparator self-test: in a target's full-TU asm, flip one byte of a string
literal it references, turn one space in such a string into two spaces and
into a tab, and separately change one of its instructions; the
normalized comparison (slice.norm_func) must reject both and accept the original.

Usage: selftest.py <full.s> <target>
"""
import importlib.util
import os
import re
import sys

spec = importlib.util.spec_from_file_location(
    "e8slice", os.path.join(os.path.dirname(os.path.abspath(__file__)), "slice.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

asm, t = open(sys.argv[1]).read(), sys.argv[2]
want = m.norm_func(m.parse_asm(asm), t)
assert want and m.norm_func(m.parse_asm(asm), t) == want

funcs, data = m.parse_asm(asm)
body = "\n".join(funcs[t])
strs = [lab for lab in re.findall(r"\$?(\.L\.str[\w.]*)", body) if lab in data]
assert strs, f"{t} references no .L.str label"
lab = strs[0]
# flip one byte inside that label's .asciz string
lines = asm.split("\n")
i = lines.index(f"{lab}:")
while '"' not in lines[i]:
    i += 1
q = lines[i].index('"') + 1
c = lines[i][q]
lines[i] = lines[i][:q] + ("X" if c != "X" else "Y") + lines[i][q + 1:]
mut_str = "\n".join(lines)
# change one instruction in the function: the first `movq` becomes `movl`
fstart = asm.index(f"\n{t}:")
k = asm.index("\tmovq\t", fstart)
mut_ins = asm[:k] + "\tmovl\t" + asm[k + 6:]
muts = [("string byte", mut_str), ("instruction", mut_ins)]
# whitespace inside a referenced string: one space -> two spaces, and -> a tab
for lab in strs:
    j = lines.index(f"{lab}:")
    while '"' not in lines[j]:
        j += 1
    orig = asm.split("\n")[j]
    q = orig.find(" ", orig.index('"'))
    if q >= 0:
        for name, rep in (("string space->two spaces", "  "), ("string space->tab", "\t")):
            ls = asm.split("\n")
            ls[j] = orig[:q] + rep + orig[q + 1:]
            muts.append((name, "\n".join(ls)))
        break
else:
    print(f"selftest {t}: no referenced string contains a space; whitespace cases skipped")
for name, a in muts:
    got = m.norm_func(m.parse_asm(a), t)
    assert got != want, f"comparator accepted a mutated {name} in {t}"
    print(f"selftest {t}: mutated {name} rejected")
