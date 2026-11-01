#!/usr/bin/env python3
"""E10: hidden dependencies of learned slices over lua/lua history.

Subcommands:
  tokpass <repo> <commits.txt> <outdir> <jobs>
      For every commit (oldest first) export the root .c/.h files, preprocess
      every core TU (-E -P, pinned flags) and store it by token hash in
      <outdir>/cas/. Writes <outdir>/tok.tsv: idx commit file status tokhash.
  estimate <outdir>
      Token-diff-only pass: upper bound on counted hits per window (extern
      function definitions at N in TUs whose token stream changed N -> N+1).
  chain <outdir> <file>
      The experiment for one TU over the history: learn each target's smallest
      d (E8's search), and at every commit where the TU's token stream changed,
      re-extract each target's slice with its frozen d. Same tokens = hit, then
      compare the full-TU normalized asm at N+1 with N. Misses (and mismatches)
      re-learn d. Writes <outdir>/events/<file>.jsonl and mismatch dumps.
  summarize <outdir>
"""
import gzip
import hashlib
import importlib.util
import json
import math
import os
import random
import re
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "e8slice", os.path.join(HERE, "..", "e8-slices-O2", "slice.py"))
e8 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(e8)

CC = os.environ.get("CC", "clang-18")
# Pinned from lua/lua's developer makefile at the pinned HEAD (linux):
#   CFLAGS= -Wall -O2 $(MYCFLAGS) -fno-stack-protector -fno-common
#   MYCFLAGS= $(LOCAL) -std=c99 -DLUA_USE_LINUX     (warnings and TESTS dropped)
# -O2 is passed to -E as well so __OPTIMIZE__ matches the compile.
PPFLAGS = ["-std=c99", "-O2", "-DLUA_USE_LINUX", "-fno-stack-protector", "-fno-common", "-E", "-P"]
CCFLAGS = ["-std=c99", "-O2", "-fno-stack-protector", "-fno-common", "-fno-pic", "-w"]
EXCLUDE = {"onelua.c", "ltests.c", "lua.c", "luac.c"}
LEARN_JOBS = int(os.environ.get("LEARN_JOBS", "1"))  # processes for re-learning d within a chain
DET_EVERY = 25  # determinism spot-check: every 25th unchanged TU per file


def tokhash(src):
    return hashlib.sha1("\0".join(t[1] for t in e8.tokenize(src)).encode()).hexdigest()


# ---------------------------------------------------------------- tokpass
def _pp_commit(args):
    repo, idx, commit, outdir = args
    names = subprocess.run(["git", "-C", repo, "ls-tree", "--name-only", commit],
                           capture_output=True, text=True, check=True).stdout.split()
    srcs = [n for n in names if n.endswith((".c", ".h"))]
    cs = sorted(n for n in srcs if n.endswith(".c") and n not in EXCLUDE)
    rows = []
    with tempfile.TemporaryDirectory() as d:
        tar = subprocess.run(["git", "-C", repo, "archive", commit, "--"] + srcs,
                             capture_output=True, check=True).stdout
        subprocess.run(["tar", "-x", "-C", d], input=tar, check=True)
        for c in cs:
            r = subprocess.run([CC] + PPFLAGS + [os.path.join(d, c)], capture_output=True, text=True)
            if r.returncode != 0:
                rows.append((idx, commit, c[:-2], "ppfail", "-"))
                continue
            h = tokhash(r.stdout)
            p = os.path.join(outdir, "cas", h + ".i.gz")
            if not os.path.exists(p):
                tmp = p + f".{os.getpid()}"
                with gzip.open(tmp, "wt") as f:
                    f.write(r.stdout)
                os.replace(tmp, p)
            rows.append((idx, commit, c[:-2], "ok", h))
    return rows


def tokpass(repo, commits_txt, outdir, jobs):
    os.makedirs(os.path.join(outdir, "cas"), exist_ok=True)
    commits = open(commits_txt).read().split()
    with ProcessPoolExecutor(int(jobs)) as ex, open(os.path.join(outdir, "tok.tsv"), "w") as out:
        for rows in ex.map(_pp_commit, [(repo, i, c, outdir) for i, c in enumerate(commits)], chunksize=4):
            for r in rows:
                out.write("\t".join(map(str, r)) + "\n")


def load_tok(outdir):
    """-> commits (list), per file: {idx: (status, hash)}"""
    commits, per = {}, defaultdict(dict)
    for ln in open(os.path.join(outdir, "tok.tsv")):
        i, c, f, st, h = ln.rstrip("\n").split("\t")
        commits[int(i)] = c
        per[f][int(i)] = (st, h)
    return [commits[i] for i in range(len(commits))], per


def read_tu(outdir, h):
    with gzip.open(os.path.join(outdir, "cas", h + ".i.gz"), "rt") as f:
        return f.read()


def _n_extern(args):
    outdir, h = args
    try:
        tu = e8.TU(read_tu(outdir, h))
    except Exception:  # splitter failure on old code
        return h, -1
    return h, sum(1 for f in tu.funcs if f.name not in tu.static_names)


def estimate(outdir, jobs=8):
    commits, per = load_tok(outdir)
    pairs = []  # (idx of N+1, file, hash at N)
    for f, m in per.items():
        for i in range(1, len(commits)):
            a, b = m.get(i - 1), m.get(i)
            if a and b and a[0] == b[0] == "ok" and a[1] != b[1]:
                pairs.append((i, f, a[1]))
    hs = sorted({h for _, _, h in pairs})
    with ProcessPoolExecutor(int(jobs)) as ex:
        nx = dict(ex.map(_n_extern, [(outdir, h) for h in hs], chunksize=8))
    # cumulative upper bound counting back from the newest commit
    by_commit = Counter()
    for i, f, h in pairs:
        by_commit[i] += max(nx[h], 0)
    print(f"commits={len(commits)} changed-TU pairs={len(pairs)} "
          f"upper bound on counted hits (extern defs at N in changed TUs)={sum(by_commit.values())}")
    acc = 0
    for i in range(len(commits) - 1, 0, -1):
        acc += by_commit[i]
        k = len(commits) - i
        if k in (50, 100, 200, 300, 400, 500, 750, 1000, 1500, 2000, 3000, 4000, 5000):
            print(f"  newest {k} transitions: upper bound {acc}")
    nf = sum(1 for f in per for i in per[f] if per[f][i][0] != "ok")
    print(f"preprocess failures (commit,file): {nf}")


# ---------------------------------------------------------------- chain
def call_targets(lines):
    out = set()
    for ln in lines:
        m = re.match(r"\s*(?:callq?|jmpq?)\s+([A-Za-z_][\w.$]*)", ln)
        if m and not m.group(1).startswith(".L"):
            out.add(m.group(1).split("@")[0])
    return out


def key_of(tu, t, d):
    """Cache key: tokens of t's depth-d slice (the whole TU if d is None)."""
    if d is None:  # never matched: the slice is the whole TU
        return "TU:" + tokhash(tu.src)
    bodies, _ = tu.bodies_at(t, d)
    return tokhash(tu.build(bodies)[0])


_TU = {}


def _learn(args):
    """E8's search for target t in TU version h -> (t, d, nfail, key, want)."""
    outdir, h, t, want = args
    if h not in _TU:
        _TU.clear()
        _TU[h] = (e8.TU(read_tu(outdir, h)), tempfile.mkdtemp(prefix="e10l-"))
    tu, tmp = _TU[h]
    res, levels = e8.search(tu, t, want, [CC] + CCFLAGS, tmp)
    d = res[0] if res else None
    return t, d, sum(1 for lv in levels if lv[3] == "compile_fail"), key_of(tu, t, d), want


def chain(outdir, fname):
    commits, per = load_tok(outdir)
    m = per.get(fname, {})
    os.makedirs(os.path.join(outdir, "events"), exist_ok=True)
    mdir = os.path.join(outdir, "mismatch", fname)
    os.makedirs(mdir, exist_ok=True)
    out = open(os.path.join(outdir, "events", fname + ".jsonl"), "w")
    tmp = tempfile.mkdtemp(prefix="e10-")
    cc = [CC] + CCFLAGS
    ev = lambda **k: out.write(json.dumps(dict(file=fname, **k)) + "\n")

    pool = ProcessPoolExecutor(LEARN_JOBS) if LEARN_JOBS > 1 else None

    def learn_all(h, full, todo, idx):
        """Learn d for every target in todo (independent: pure in TU and t)."""
        args = [(outdir, h, t, e8.norm_func(full, t)) for t in todo]
        res = pool.map(_learn, args) if pool else map(_learn, args)
        got = {}
        for t, d, nfail, key, want in res:
            ev(c=idx, ev="learn", t=t, d=d if d is not None else "never", nfail=nfail)
            got[t] = dict(d=d, key=key, want=want)
        return got

    state, prev_h, prev_tu, prev_full, n_unch = None, None, None, None, 0
    for idx in range(len(commits)):
        st = m.get(idx)
        if st is None:
            if state is not None:
                ev(c=idx, ev="absent")
            state, prev_h = None, None
            continue
        if st[0] != "ok":
            ev(c=idx, ev="ppfail")
            state, prev_h = None, None
            continue
        h = st[1]
        if state is not None and h == prev_h:
            n_unch += 1
            if n_unch % DET_EVERY == 1:  # determinism spot-check
                asm, err = e8.compile_s(cc, prev_tu.src, tmp, "det")
                p = e8.parse_asm(asm)
                ok = sorted(p[0]) == sorted(prev_full[0]) and all(
                    e8.norm_func(p, n) == e8.norm_func(prev_full, n) for n in p[0])
                ev(c=idx, ev="det", ok=ok)
            continue
        src = read_tu(outdir, h)
        asm, err = e8.compile_s(cc, src, tmp, "full")
        if asm is None:
            ev(c=idx, ev="ccfail", err=err[:200])
            state, prev_h = None, None
            continue
        full = e8.parse_asm(asm)
        targets = [ln.split()[1] for ln in asm.split("\n")
                   if ln.strip().startswith(".globl") and ln.split()[1] in full[0]]
        try:
            tu = e8.TU(src)
        except Exception as ex:
            ev(c=idx, ev="splitfail", err=repr(ex)[:200])
            state, prev_h = None, None
            continue
        targets = [t for t in targets if t in tu.fdef]
        new, todo = {}, []
        if state is None:
            ev(c=idx, ev="start", n=len(targets))
            todo = list(targets)
        else:
            tset = set(targets)
            for t, s in state.items():
                if t not in tset:
                    ev(c=idx, ev="miss", t=t, why="gone")
                    continue
                k = key_of(tu, t, s["d"])
                if k != s["key"]:
                    ev(c=idx, ev="miss", t=t, why="never" if s["d"] is None else "key", d=s["d"])
                    todo.append(t)
                    continue
                got = e8.norm_func(full, t)
                match = got == s["want"]
                bodies, _ = tu.bodies_at(t, s["d"])
                rec = dict(c=idx, ev="hit", t=t, d=s["d"], match=match, nbodies=len(bodies))
                if match:
                    new[t] = s
                else:
                    # diagnostics: called symbols that appear/disappear, and
                    # for each, whether it is in the slice and its caller counts
                    oc = call_targets(s["want"].split("\n"))
                    nc = call_targets(got.split("\n"))
                    diag = {}
                    for f in sorted(oc ^ nc):
                        diag[f] = dict(
                            change="call_added" if f in nc else "call_removed",
                            in_slice=f in bodies, static=f in tu.static_names,
                            callers_N=len(prev_tu.callers.get(f, ())),
                            callers_N1=len(tu.callers.get(f, ())),
                            body_changed=(f in tu.fdef and f in prev_tu.fdef and
                                          tokhash(tu.txt(tu.fdef[f].toks)) !=
                                          tokhash(prev_tu.txt(prev_tu.fdef[f].toks))))
                    rec["diag"] = diag
                    with open(os.path.join(mdir, f"{idx}_{t}.N.s"), "w") as fh:
                        fh.write(s["want"])
                    with open(os.path.join(mdir, f"{idx}_{t}.N1.s"), "w") as fh:
                        fh.write(got)
                    todo.append(t)
                ev(**rec)
            for t in targets:
                if t not in state:
                    ev(c=idx, ev="new", t=t)
                    todo.append(t)
        new.update(learn_all(h, full, todo, idx))
        ev(c=idx, ev="tu", h=h, prev=prev_h, ntargets=len(targets))
        state, prev_h, prev_tu, prev_full, n_unch = new, h, tu, full, 0
        out.flush()
    out.close()


# ---------------------------------------------------------------- summarize
def cp_upper(k, n, a=0.05):
    """Clopper-Pearson two-sided 95% upper bound (bisection on the binomial cdf)."""
    if k >= n:
        return 1.0
    lo, hi = 0.0, 1.0
    for _ in range(60):
        p = (lo + hi) / 2
        cdf = sum(math.comb(n, j) * p ** j * (1 - p) ** (n - j) for j in range(k + 1))
        if cdf > a / 2:
            lo = p
        else:
            hi = p
    return hi


def cp_lower(k, n, a=0.05):
    if k == 0:
        return 0.0
    lo, hi = 0.0, 1.0
    for _ in range(60):
        p = (lo + hi) / 2
        sf = 1 - sum(math.comb(n, j) * p ** j * (1 - p) ** (n - j) for j in range(k))
        if sf < a / 2:
            lo = p
        else:
            hi = p
    return lo


def summarize(outdir):
    commits, per = load_tok(outdir)
    E = []
    for fn in sorted(os.listdir(os.path.join(outdir, "events"))):
        E += [json.loads(l) for l in open(os.path.join(outdir, "events", fn))]
    C = Counter(e["ev"] for e in E)
    hits = [e for e in E if e["ev"] == "hit"]
    misses = [e for e in E if e["ev"] == "miss"]
    mism = [e for e in hits if not e["match"]]
    n, M = len(hits), len(mism)
    print(f"window: {len(commits)} commits ({commits[0][:10]}..{commits[-1][:10]}), "
          f"{len(commits) - 1} transitions")
    print(f"events: {dict(C)}")
    tus_changed = sum(1 for e in E if e["ev"] == "tu" and e["prev"])
    print(f"changed-TU transitions evaluated: {tus_changed}")
    print(f"counted hits n = {n}; misses = {len(misses)} "
          f"(key {sum(e['why'] == 'key' for e in misses)}, never {sum(e['why'] == 'never' for e in misses)}, "
          f"gone {sum(e['why'] == 'gone' for e in misses)})")
    print(f"hit rate = {n}/{n + len(misses)} = {n / max(1, n + len(misses)):.4f}")
    print(f"mismatches among counted hits = {M} ({M / max(1, n):.4%}); "
          f"95% CP interval [{cp_lower(M, n):.4%}, {cp_upper(M, n):.4%}] (i.i.d.)")
    # per-commit
    hc, fc = Counter(), Counter()
    for e in hits:
        hc[e["c"]] += 1
        fc[e["c"]] += 0 if e["match"] else 1
    ncom = len(hc)
    nfailc = sum(1 for c in hc if fc[c])
    print(f"commits with >=1 counted hit: {ncom}; with >=1 mismatch: {nfailc} "
          f"({nfailc / max(1, ncom):.2%}; 95% CP [{cp_lower(nfailc, ncom):.2%}, {cp_upper(nfailc, ncom):.2%}])")
    dist = Counter(fc[c] for c in hc)
    print(f"per-commit mismatch counts (count: commits): {dict(sorted(dist.items()))}")
    print("commits with mismatches (idx commit: mismatches/hits):")
    for c in sorted(c for c in hc if fc[c]):
        print(f"  {c} {commits[c][:10]}: {fc[c]}/{hc[c]}")
    # commit-clustered bootstrap of the pooled rate
    rng = random.Random(1)
    cs = list(hc)
    reps = []
    for _ in range(4000):
        s = [rng.choice(cs) for _ in cs]
        reps.append(sum(fc[c] for c in s) / max(1, sum(hc[c] for c in s)))
    reps.sort()
    print(f"commit-clustered bootstrap 95% interval for the mismatch rate (4000 reps): "
          f"[{reps[int(0.025 * len(reps))]:.4%}, {reps[int(0.975 * len(reps)) - 1]:.4%}]")
    if M == 0:
        print(f"zero mismatches: rule-of-three style bounds: per hit {3 / n:.4%}, per commit {3 / ncom:.2%}")
    # failures recorded
    for k in ("ppfail", "ccfail", "splitfail"):
        ce = Counter(e["c"] for e in E if e["ev"] == k)
        cf = Counter(e["file"] for e in E if e["ev"] == k)
        print(f"{k}: {sum(ce.values())} (commit,file) in {len(ce)} commits; by file {dict(cf.most_common())}")
    anyf = {e["c"] for e in E if e["ev"] in ("ppfail", "ccfail", "splitfail")}
    print(f"commits with any compile failure: {len(anyf)}/{len(commits)} ({len(anyf) / len(commits):.2%})")
    nfl = sum(e["nfail"] > 0 for e in E if e["ev"] == "learn")
    print(f"learn events: {C['learn']} (with >=1 slice compile failure during search: {nfl}); "
          f"learned never: {sum(e['d'] == 'never' for e in E if e['ev'] == 'learn')}")
    det = [e for e in E if e["ev"] == "det"]
    print(f"determinism spot-checks on unchanged TUs: {sum(e['ok'] for e in det)}/{len(det)} identical")
    print(f"hits by frozen d: {dict(sorted(Counter(e['d'] for e in hits).items()))}")
    if mism:
        print(f"mismatches by frozen d: {dict(sorted(Counter(e['d'] for e in mism).items()))}")
        print(f"mismatches by file: {dict(Counter(e['file'] for e in mism).most_common())}")
        cls = Counter()
        for e in mism:
            dg = e["diag"]
            if not dg:
                cls["no call-set change"] += 1
            elif any(not v["in_slice"] and v["body_changed"] for v in dg.values()):
                cls["call-set change; a callee outside the slice changed its body"] += 1
            elif any(v["callers_N"] != v["callers_N1"] for v in dg.values()):
                cls["call-set change; a callee's caller count changed (body same)"] += 1
            else:
                cls["call-set change; other"] += 1
        print(f"automatic mismatch classes: {dict(cls.most_common())}")
        print("mismatch list (idx file target d diag):")
        for e in mism:
            print(f"  {e['c']} {e['file']} {e['t']} d={e['d']} {json.dumps(e['diag'])}")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "tokpass":
        tokpass(*sys.argv[2:6])
    elif cmd == "estimate":
        estimate(sys.argv[2])
    elif cmd == "chain":
        chain(sys.argv[2], sys.argv[3])
    elif cmd == "summarize":
        summarize(sys.argv[2])
