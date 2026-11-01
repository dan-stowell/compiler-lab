#!/usr/bin/env python3
"""Compare splitter function names vs a grep heuristic for each .c file at a rev.
grep heuristic: a line starting in column 0 with an identifier followed by '(' and
the definition's '{' at the end of that line or a later line starting '{'/ending ') {'."""
import os, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ripple import tokenize, split_functions
repo, rev = sys.argv[1:3]
files = [p for p in subprocess.run(["git","-C",repo,"ls-tree","-r","--name-only",rev],capture_output=True,text=True).stdout.split() if p.endswith(".c") and not p.startswith("test/")]
tot_s = tot_g = 0
for p in files:
    src = subprocess.run(["git","-C",repo,"show",f"{rev}:{p}"],capture_output=True,text=True).stdout
    s = sorted(n for n,_ in split_functions(tokenize(src)))
    # grep: column-0 non-preprocessor line with NAME( whose signature ends in ') {' (possibly after continuation lines)
    g = []
    lines = src.split("\n")
    for i,l in enumerate(lines):
        if not l or l[0] in " \t#/}*" : continue
        m = re.match(r"[^=;(]*?\b([A-Za-z_]\w*)\(", l)
        if not m: continue
        j = i
        while j < len(lines) and not re.search(r"[;{]\s*$", lines[j]): j += 1
        if j < len(lines) and re.search(r"\)\s*\{\s*$", lines[j]): g.append(m.group(1))
    g.sort()
    tot_s += len(s); tot_g += len(g)
    if s != g:
        print(f"  {p}: splitter={len(s)} grep={len(g)} only-splitter={sorted(set(s)-set(g))} only-grep={sorted(set(g)-set(s))}")
print(f"{rev}: splitter={tot_s} grep={tot_g} files={len(files)}")
