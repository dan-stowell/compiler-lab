#!/usr/bin/env python3
"""E15: output churn of E13 units over lua/lua history (journal/cycle7-plan.md).

Fixed version of the ideator's cut2.py/attr2.py (kept as ideator_*.py):
whole-unit output hash, compiles memoized by slice hash, all transitions.

Unit (E13's extraction rule): one extern target of a core TU, compiled with
every other extern body replaced by its prototype; unused statics are dropped
by clang. What the compiler sees of it is exactly the target body, the bodies
of the statics it reaches transitively (call/reference edges), prototypes of
everything else it references, and the context decls those need. That is
E8's tu.build(target + reached statics) ("slice"), which is what we compile.
`check` verifies on a sample that the real E13 unit text gives the same
normalized output for everything reachable from the target.

Subcommands:
  units   <e10work> <out> [jobs]   per distinct TU: per extern target the slice
                                   key, closure-bodies key and target-body key;
                                   slices stored in <out>/slices/<key>.c.gz
  compile <e10work> <out> [jobs]   compile each slice needed by a slice-changed
                                   transition once (memo by slice key); store the
                                   whole-unit normalized output hash
  check   <e10work> <out> <n>      E13-unit vs slice compile on n sampled units
  analyze <e10work> <out>          per-transition rates
  attr    <e10work> <out>          which decls changed in asm_changed_ctx_only cases
"""
import collections
import gzip
import hashlib
import json
import os
import random
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "e10-hidden-deps"))
import e10  # noqa: E402
sys.path.insert(0, os.path.join(HERE, "..", "e13-tu-free-build"))
import tufree  # noqa: E402

e8 = e10.e8
CC = [e10.CC] + e10.CCFLAGS
WORK = OUT = None


def H(s):
    return hashlib.sha1(s.encode()).hexdigest()[:20]


def toks(ts):
    return "\0".join(t[1] for t in ts)


def closure(tu, t):
    """Target plus the non-target function bodies it reaches (E13's rule: only
    other targets' bodies are removed, so statics and glibc extern-inline
    definitions such as tolower stay):
    references from reached bodies to static function names, and through the
    initializers of file-scope variables (luaL_Reg tables of static functions)."""
    reach, seen, fr = {t}, {t}, [t]
    while fr:
        n = fr.pop()
        ds = [tu.fdef[n]] if n in tu.fdef else [d for d in tu.ord_prov.get(n, []) if not d.is_func]
        for d in ds:
            for r in d.ref_ord:
                if r in seen:
                    continue
                if r in tu.fdef and not is_target(tu, tu.fdef[r]):  # statics, inline defs
                    seen.add(r); reach.add(r); fr.append(r)
                elif r in tu.ord_prov and r not in tu.fdef:
                    seen.add(r); fr.append(r)
    return reach


def is_target(tu, f):
    """E13's targets are the global T symbols of the file's -O2 object: extern
    definitions, minus C99/GNU inline definitions (glibc's extern inline atoi,
    getchar, ... in the preprocessed TU), which emit no symbol."""
    h = [t for _, t, _, _ in f.header]
    if not hasattr(tu, "static_protos"):  # `static T f(...);` then `T f(...) {...}`: internal
        tu.static_protos = {n for d in tu.decls if not d.is_func and "static" in [x for _, x, _, _ in d.toks]
                            for n in d.decl_ord}
    return f.name not in tu.static_names and f.name not in tu.static_protos and not ({"inline", "__inline", "__inline__"} & set(h)) \
        and "__gnu_inline__" not in h and "gnu_inline" not in h


def asm_name(tu, t):
    """Symbol name of target t (E8's splitter misnames `T *(name) (args)`;
    E13's func_name handles it)."""
    return tufree.func_name([x for _, x, _, _ in tu.fdef[t].header])


def unit_keys(tu, t):
    reach = closure(tu, t)
    src = tu.build(reach)[0]
    skey = e10.tokhash(src)[:20]
    bkey = H("\1".join(n + "\2" + toks(tu.fdef[n].toks) for n in sorted(reach)))
    tkey = H(toks(tu.fdef[t].toks))
    return reach, src, skey, bkey, tkey


def slice_path(skey):
    return os.path.join(OUT, "slices", skey[:2], skey + ".c.gz")


def _init(work, out):
    global WORK, OUT
    WORK, OUT = work, out


def _tu_units(h):
    try:
        tu = e8.TU(e10.read_tu(WORK, h))
    except Exception:  # splitter failure on old code (same handling as E10/E14)
        return h, None
    res = {}
    for t in (f.name for f in tu.funcs if is_target(tu, f)):
        reach, src, skey, bkey, tkey = unit_keys(tu, t)
        p = slice_path(skey)
        if not os.path.exists(p):
            os.makedirs(os.path.dirname(p), exist_ok=True)
            tmp = p + f".{os.getpid()}"
            with gzip.open(tmp, "wt") as f:
                f.write(src)
            os.replace(tmp, p)
        res[t] = [skey, bkey, tkey, sorted(reach)]
    return h, res


def units(work, out, jobs):
    _, per = e10.load_tok(work)
    hs = sorted({v[1] for f in per for v in per[f].values() if v[0] == "ok"})
    res = {}
    with ProcessPoolExecutor(jobs, initializer=_init, initargs=(work, out)) as ex:
        for h, o in ex.map(_tu_units, hs, chunksize=8):
            res[h] = o
    json.dump(res, open(os.path.join(out, "units.json"), "w"))
    print(f"TUs: {len(res)} ({sum(v is None for v in res.values())} splitter failures); "
          f"units: {sum(len(v) for v in res.values() if v)}; "
          f"distinct slices: {len({u[0] for v in res.values() if v for u in v.values()})}")


# ---------------------------------------------------------------- output normalization
DROP = (".file", ".loc", ".cfi_", ".ident", ".addrsig")


def norm_unit(asm):
    """Whole .s of the unit: every function and data block (incl. reached
    statics, .bss/.zero sizes, string literals), debug/ident lines dropped,
    private labels renamed bijectively by first occurrence, whitespace collapsed
    outside strings (E8's comparator rules)."""
    out = [ln for ln in asm.split("\n") if ln.strip() and not ln.strip().startswith(DROP)]
    ren = {}

    def sub(m):
        key = m.group(0)
        if key.startswith("BB"):
            key = ".L" + key
        if key not in ren:
            ren[key] = f".L#{len(ren)}"
        return ren[key] if m.group(0).startswith(".") else ren[key][2:]
    return "\n".join(e8.collapse(e8.PRIV_RE.sub(sub, ln)) for ln in out)


def reach_syms(asm, t):
    """(symbols reachable from t, all function/data symbols) in an .s"""
    funcs, data = e8.parse_asm(asm)
    blocks = {**data, **funcs}
    seen, q = {t}, [t]
    while q:
        for ln in blocks[q.pop()]:
            for s in e8.SYM_RE.findall(ln.split("#", 1)[0]):
                if s in blocks and s not in seen:
                    seen.add(s)
                    q.append(s)
    return seen, set(blocks)


def reach_norm(asm, t):
    """Functions and data blocks reachable from symbol t through symbol
    references, in emission order, normalized like norm_unit (for `check`)."""
    funcs, data = e8.parse_asm(asm)
    blocks = {**{k: v for k, v in data.items()}, **funcs}
    seen, q = {t}, [t]
    while q:
        for ln in blocks[q.pop()]:
            for s in e8.SYM_RE.findall(ln.split("#", 1)[0]):
                if s in blocks and s not in seen:
                    seen.add(s)
                    q.append(s)
    lines = asm.split("\n")
    order = sorted(seen, key=lambda s: next(i for i, ln in enumerate(lines) if ln.startswith(s + ":")))
    return norm_unit("\n".join(ln for s in order for ln in blocks[s]))


def compile_text(src, tag):
    with tempfile.TemporaryDirectory() as d:
        asm, err = e8.compile_s(CC, src, d, tag)
    return asm, err


def _compile(skey):
    with gzip.open(slice_path(skey), "rt") as f:
        src = f.read()
    asm, err = compile_text(src, "u")
    if asm is None:
        return skey, None
    return skey, H(norm_unit(asm))


def transitions(work, U):
    """Yield (file, i, tu_a, tu_b, target) for every commit transition i-1 -> i of
    a core TU that preprocessed and split at both ends, per extern target present
    at both (E14's population)."""
    commits, per = e10.load_tok(work)
    for f in sorted(per):
        d = per[f]
        for i in range(1, len(commits)):
            a, b = d.get(i - 1), d.get(i)
            if not a or not b or a[0] != "ok" or b[0] != "ok":
                continue
            ka, kb = U[a[1]], U[b[1]]
            if ka is None or kb is None:
                continue
            for t in sorted(set(ka) & set(kb)):
                yield f, i, a[1], b[1], t


def compile_all(work, out, jobs):
    U = json.load(open(os.path.join(out, "units.json")))
    need = set()
    for f, i, ha, hb, t in transitions(work, U):
        if U[ha][t][0] != U[hb][t][0]:
            need |= {U[ha][t][0], U[hb][t][0]}
    of = os.path.join(out, "outputs.json")
    O = json.load(open(of)) if os.path.exists(of) else {}
    todo = sorted(need - set(O))
    print(f"slices needed {len(need)}, to compile {len(todo)}", flush=True)
    with ProcessPoolExecutor(jobs, initializer=_init, initargs=(work, out)) as ex:
        for n, (k, h) in enumerate(ex.map(_compile, todo, chunksize=16)):
            O[k] = h
            if n % 5000 == 4999:
                json.dump(O, open(of, "w"))
                print(f"  compiled {n + 1}", flush=True)
    json.dump(O, open(of, "w"))
    print(f"compile failures: {sum(v is None for k, v in O.items() if k in need)}")


# ---------------------------------------------------------------- check
def e13_unit_text(tu, t):
    externs = {f.name for f in tu.funcs if is_target(tu, f)}
    return "\n".join(tu.txt(d.header) + ";" if d.is_func and d.name in externs and d.name != t
                     else tu.txt(d.toks) for d in tu.decls) + "\n"


def _check(args):
    h, t = args
    tu = e8.TU(e10.read_tu(WORK, h))
    _, src, skey, _, _ = unit_keys(tu, t)
    a_s, _ = compile_text(src, "s")
    a_u, err = compile_text(e13_unit_text(tu, t), "u")
    if a_s is None or a_u is None:
        return h, t, "compile_fail"
    sym = asm_name(tu, t)
    r, allb = reach_syms(a_s, sym)
    same = reach_norm(a_s, sym) == reach_norm(a_u, sym)
    return h, t, f"{'same' if same else 'DIFF'}; slice .s blocks not reachable from target: {len(allb - r)}"


def check(work, out, n):
    U = json.load(open(os.path.join(out, "units.json")))
    pop = sorted({(ha, t) for f, i, ha, hb, t in transitions(work, U) if ha != hb})
    random.seed(15)
    sample = random.sample(pop, n)
    c = collections.Counter()
    with ProcessPoolExecutor(os.cpu_count(), initializer=_init, initargs=(work, out)) as ex:
        for h, t, r in ex.map(_check, sample):
            c[r] += 1
            if not r.startswith("same"):
                print("  ", h, t, r)
    print(f"E13-unit (reachable part) vs slice, {n} sampled (TU, target) on changed-TU transitions: {dict(c)}")


# ---------------------------------------------------------------- analyze
def analyze(work, out):
    U = json.load(open(os.path.join(out, "units.json")))
    O = json.load(open(os.path.join(out, "outputs.json")))
    c = collections.Counter()
    commits_ctx = set()
    per_file = collections.defaultdict(collections.Counter)
    ctx_cases = []
    for f, i, ha, hb, t in transitions(work, U):
        ua, ub = U[ha][t], U[hb][t]
        c["fn_transitions"] += 1
        if ha == hb:
            continue
        c["tu_changed"] += 1
        sk, bk, tk = ua[0] != ub[0], ua[1] != ub[1], ua[2] != ub[2]
        c["slice_changed"] += sk
        c["closure_body_changed"] += bk
        c["target_body_changed"] += tk
        if not sk:
            continue
        oa, ob = O.get(ua[0]), O.get(ub[0])
        if oa is None or ob is None:
            c["excluded_compile_fail"] += 1
            continue
        if oa != ob:
            c["output_changed"] += 1
            per_file[f]["output_changed"] += 1
            if not bk:
                c["asm_changed_ctx_only"] += 1
                commits_ctx.add(i)
                ctx_cases.append((f, i, ha, hb, t))
        elif bk:
            c["body_changed_output_same"] += 1
    n, m = c["fn_transitions"], c["tu_changed"]
    print(f"commits {len(e10.load_tok(work)[0])}; function-transitions {n}; on changed TUs {m}")
    print(f"{'measure':34s} {'count':>8s} {'rate/all':>9s} {'rate/TUchg':>10s}")
    for k in ("slice_changed", "closure_body_changed", "target_body_changed", "output_changed",
              "asm_changed_ctx_only", "body_changed_output_same", "excluded_compile_fail"):
        print(f"{k:34s} {c[k]:8d} {c[k] / n:9.5f} {c[k] / m:10.5f}")
    so = c["slice_changed"] - c["excluded_compile_fail"]
    print(f"slice changes leaving whole-unit output identical: "
          f"{so - c['output_changed']} / {so} ({(so - c['output_changed']) / so:.1%})")
    print(f"output changes that are context-only: {c['asm_changed_ctx_only']} / {c['output_changed']} "
          f"({c['asm_changed_ctx_only'] / c['output_changed']:.1%}); in {len(commits_ctx)} commits")
    print(f"slice-key / output change-rate ratio: {c['slice_changed'] / c['output_changed']:.2f}x; "
          f"closure-body / output: {c['closure_body_changed'] / c['output_changed']:.2f}x")
    json.dump(ctx_cases, open(os.path.join(out, "ctx_only.json"), "w"))


# ---------------------------------------------------------------- attr (from attr2.py)
def decl_label(d):
    T = [t for _, t, _, _ in d.toks]
    names = sorted(d.decl_ord | d.decl_tag)
    if d.is_func:
        return "proto:" + d.name
    return " ".join(names)[:40] or " ".join(T[:4])


def attr(work, out):
    """For asm_changed_ctx_only cases: the context decls (by name) whose text
    differs between the two slices."""
    cases = json.load(open(os.path.join(out, "ctx_only.json")))
    names = collections.Counter()
    U =json.load(open(os.path.join(out, "units.json")))
    for f, i, ha, hb, t in cases:
        A = e8.TU(gzip.open(slice_path(U[ha][t][0]), "rt").read())
        B = e8.TU(gzip.open(slice_path(U[hb][t][0]), "rt").read())
        ta = {" ".join(A.txt(d.toks).split()) for d in A.decls}
        ch = set()
        for d in B.decls:
            if " ".join(B.txt(d.toks).split()) not in ta:
                ch.add(decl_label(d))
        tb = {" ".join(B.txt(d.toks).split()) for d in B.decls}
        for d in A.decls:
            if " ".join(A.txt(d.toks).split()) not in tb:
                ch.add(decl_label(d))
        for k in ch:
            names[k] += 1
    print(f"asm_changed_ctx_only cases: {len(cases)}; changed context decls (cases touching each):")
    for k, v in names.most_common(25):
        print(f"  {v:6d}  {k}")


if __name__ == "__main__":
    cmd, work, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    _init(work, out)
    jobs = int(sys.argv[4]) if len(sys.argv) > 4 else os.cpu_count()
    if cmd == "units":
        units(work, out, jobs)
    elif cmd == "compile":
        compile_all(work, out, jobs)
    elif cmd == "check":
        check(work, out, int(sys.argv[4]))
    elif cmd == "analyze":
        analyze(work, out)
    elif cmd == "attr":
        attr(work, out)
    else:
        sys.exit(__doc__)
