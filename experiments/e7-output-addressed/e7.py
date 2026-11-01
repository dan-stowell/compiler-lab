#!/usr/bin/env python3
"""E7: per-function asm ripple over chibicc history, compiled by a FIXED compiler.

  e7.py plan <repo> <rev> <work>     write work/commits.tsv, materialize sources for
                                     uncompiled keys under work/src/<key>/, list
                                     those keys in work/jobs.txt
  e7.py analyze <repo> <rev> <work> <out.csv>
                                     split asm per function, hash raw/normalized,
                                     write one row per first-parent transition

Compile key = sha256(path, .c blob, every non-test .h (path, blob) in the tree),
so a file is recompiled only when it or a header it could include changes.
"""
import hashlib
import os
import re
import subprocess
import sys


def git(repo, *args):
    return subprocess.run(["git", "-C", repo, *args], capture_output=True,
                          check=True).stdout


def commits(repo, rev):
    return git(repo, "rev-list", "--first-parent", "--reverse", rev).decode().split()


def tree(repo, rev):
    """[(path, blob)] for non-test .c and .h files."""
    out = []
    for line in git(repo, "ls-tree", "-r", rev).decode().splitlines():
        meta, path = line.split("\t", 1)
        if path.startswith("test/") or not path.endswith((".c", ".h")):
            continue
        out.append((path, meta.split()[2]))
    return out


def keys_of(t):
    hdrs = sorted(f"{p}={b}" for p, b in t if p.endswith(".h"))
    res = []
    for p, b in t:
        if p.endswith(".c"):
            k = hashlib.sha256("\n".join([p, b, *hdrs]).encode()).hexdigest()[:24]
            res.append((p, k))
    return res


def plan(repo, rev, work):
    os.makedirs(f"{work}/src", exist_ok=True)
    os.makedirs(f"{work}/asm", exist_ok=True)
    jobs, seen = [], set()
    with open(f"{work}/commits.tsv", "w") as f:
        for c in commits(repo, rev):
            t = tree(repo, c)
            ks = keys_of(t)
            f.write(c + "\t" + " ".join(f"{p}:{k}" for p, k in ks) + "\n")
            for p, k in ks:
                if k in seen:
                    continue
                seen.add(k)
                if os.path.exists(f"{work}/asm/{k}.done"):
                    continue  # cached from an earlier run
                d = f"{work}/src/{k}"
                for fp, b in t:
                    if fp.endswith(".h") or fp == p:
                        os.makedirs(os.path.dirname(f"{d}/{fp}"), exist_ok=True)
                        with open(f"{d}/{fp}", "wb") as o:
                            o.write(git(repo, "cat-file", "blob", b))
                jobs.append(f"{k} {p}")
    with open(f"{work}/jobs.txt", "w") as f:
        f.write("".join(j + "\n" for j in jobs))
    print(f"plan: {len(seen)} distinct compile keys, {len(jobs)} to compile")


BOUNDARY = re.compile(r"^\s*\.(globl|local|text|data|bss|section)\b")
FUNC = re.compile(r"^\s*\.type\s+([^,\s]+),\s*@function")
DBG = re.compile(r"^\s*\.(loc|file)\b")
UNIQ = re.compile(r"\.L\.\.(\d+)")              # string literals, case/goto labels
OBJ = re.compile(r"^\s*\.type\s+\.L\.\.(\d+),\s*@object")
LBL = re.compile(r"^\.L\.\.(\d+):$")
VIS = re.compile(r"^\s*\.(globl|local)\s+(\S+)")
CNT = re.compile(r"\.L\.([A-Za-z_]+)\.(\d+)\b")  # .L.else.N etc. (codegen counter)


def split_funcs(text):
    """({name: [lines]}, {N: [lines]}): functions from '.type name, @function', and
    .L..N data objects (string literals, static locals) from '.type .L..N, @object',
    each up to the next section/symbol boundary."""
    funcs, objs, cur, vis = {}, {}, None, {}
    for line in text.splitlines():
        m = VIS.match(line)
        if m:
            vis[m.group(2)] = line.strip()
        m = FUNC.match(line)
        if m:
            # the function's .globl/.local line is part of its hash (static<->extern)
            cur = funcs.setdefault(m.group(1), [vis.get(m.group(1), "<no visibility>")])
            continue
        m = OBJ.match(line)
        if m:
            cur = objs.setdefault(m.group(1), [])
            continue
        if BOUNDARY.match(line):
            cur = None
            continue
        if cur is None:
            m = LBL.match(line)  # .bss objects have no .type line
            if m:
                cur = objs.setdefault(m.group(1), [])
        if cur is not None:
            cur.append(line)
    return funcs, objs


def normalize(lines, objs=None, keep_dbg=False):
    """Drop .loc/.file; renumber .L..N and .L.kind.N labels by first appearance.
    With objs, also append the (renumbered) contents of every referenced .L..N
    data object, transitively, so a changed string literal counts as a change."""
    out, uniq, cnt = [], {}, {}

    def ren(line):
        line = UNIQ.sub(lambda m: ".L..u%d" % uniq.setdefault(m.group(1), len(uniq)), line)
        return CNT.sub(lambda m: ".L.%s.c%d" % (m.group(1), cnt.setdefault(m.group(2), len(cnt))), line)
    out = [ren(l) for l in lines if keep_dbg or not DBG.match(l)]
    if objs is not None:
        i = 0
        while i < len(uniq):
            n = list(uniq)[i]
            out.append("@obj u%d" % i)
            out += [ren(l) for l in objs.get(n, ["<code label, not a data object>"]) if not DBG.match(l)]
            i += 1
    return out


def h(lines):
    return hashlib.sha256("\n".join(lines).encode()).hexdigest()


def load(work, k, cache):
    if k not in cache:
        if os.path.exists(f"{work}/asm/{k}.fail") or not os.path.exists(f"{work}/asm/{k}.s"):
            cache[k] = None
        else:
            with open(f"{work}/asm/{k}.s", errors="replace") as f:
                fs, objs = split_funcs(f.read())
            cache[k] = {n: (h(ls), h(normalize(ls)), h(l for l in ls if not DBG.match(l)),
                            h(normalize(ls, objs)), h(normalize(ls, keep_dbg=True)))
                        for n, ls in fs.items()}
    return cache[k]


def analyze(repo, rev, work, out):
    cache, failed_files = {}, set()
    prev = None  # (rev, {(file, name): (raw, norm)}) or (rev, None) if failed
    with open(f"{work}/commits.tsv") as f:
        lines = [l.rstrip("\n").split("\t") for l in f]
    with open(out, "w") as o:
        o.write("commit,status,nfuncs,raw_changed,norm_changed,added,removed,raw_frac,norm_frac,"
                "noloc_frac,normlit_frac,labelsonly_frac\n")
        for c, spec in lines:
            fm, bad = {}, []
            for item in spec.split():
                p, k = item.rsplit(":", 1)
                r = load(work, k, cache)
                if r is None:
                    bad.append(p)
                    failed_files.add(k)
                    continue
                for n, hv in r.items():
                    fm[(p, n)] = hv
            cur = None if bad else fm
            if prev is None:
                status = "first"
            elif cur is None or prev[1] is None:
                status = "excluded:" + ("self" if cur is None else "parent") + \
                         (":" + "|".join(bad) if bad else "")
            else:
                status = "ok"
            if status == "ok":
                pm = prev[1]
                added = [f for f in cur if f not in pm]
                removed = [f for f in pm if f not in cur]
                raw = sum(1 for f in cur if f not in pm or pm[f][0] != cur[f][0])
                norm = sum(1 for f in cur if f not in pm or pm[f][1] != cur[f][1])
                noloc = sum(1 for f in cur if f not in pm or pm[f][2] != cur[f][2])
                lit = sum(1 for f in cur if f not in pm or pm[f][3] != cur[f][3])
                lab = sum(1 for f in cur if f not in pm or pm[f][4] != cur[f][4])
                n = len(cur)
                o.write(f"{c[:10]},ok,{n},{raw},{norm},{len(added)},{len(removed)},"
                        f"{raw / n if n else 0:.6f},{norm / n if n else 0:.6f},"
                        f"{noloc / n if n else 0:.6f},{lit / n if n else 0:.6f},"
                        f"{lab / n if n else 0:.6f}\n")
            else:
                o.write(f"{c[:10]},{status},{len(fm)},,,,,,,,,\n")
            prev = (c, cur)
    print(f"analyze: {len(lines)} commits, {len(failed_files)} failing compile keys")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "plan":
        plan(*sys.argv[2:5])
    elif cmd == "analyze":
        analyze(*sys.argv[2:6])
    else:
        sys.exit(__doc__)
