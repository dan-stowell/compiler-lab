#!/usr/bin/env python3
"""E14: lifetime of TU-free function keys over lua/lua history (E10's data).

Subcommands:
  keys <e10work> <out.json> [jobs]
      For every distinct preprocessed TU in E10's CAS, and every extern function
      in it, compute three keys (the per-TU key is the TU's token hash itself):
        free  - E14 key: every non-function decl, the target's body, the bodies of
                the statics it reaches transitively, other externs as prototypes
                (unreached statics dropped), token-normalized. Compiler and flags
                are fixed over the history, so they do not vary the key.
        slice - E8 minimal slice: tu.build(target + reached statics), token hash.
        body  - token hash of the target's own definition.
  analyze <e10work> <keys.json>
      Per commit transition N -> N+1 and function present at both, count changes
      of each key. Change rate = changes / function-transitions; mean lifetime =
      1 / rate (commits).
"""
import hashlib
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "e10-hidden-deps"))
import e10  # noqa: E402

e8 = e10.e8
WORK = None


def H(s):
    return hashlib.sha1(s.encode()).hexdigest()[:16]


def toks(ts):
    return "\0".join(t[1] for t in ts)


def tu_keys(h):
    try:
        tu = e8.TU(e10.read_tu(WORK, h))
    except Exception:  # splitter failure on old code (same handling as E10)
        return h, None
    statics = tu.static_names
    out = {}
    for t in (f.name for f in tu.funcs if f.name not in statics):
        reach, fr = {t}, [t]
        while fr:
            for n in tu.fdef[fr.pop()].ref_ord:
                if n in statics and n not in reach:
                    reach.add(n)
                    fr.append(n)
        parts = []
        for d in tu.decls:
            if not d.is_func or d.name in reach:
                parts.append(toks(d.toks))
            elif d.name not in statics:
                parts.append(toks(d.header) + "\0;")
        out[t] = (H("\1".join(parts)),
                  e10.tokhash(tu.build(reach)[0]),
                  H(toks(tu.fdef[t].toks)))
    return h, out


def _init(work):
    global WORK
    WORK = work


def keys(work, out, jobs):
    _, per = e10.load_tok(work)
    hs = sorted({v[1] for f in per for v in per[f].values() if v[0] == "ok"})
    res = {}
    with ProcessPoolExecutor(jobs, initializer=_init, initargs=(work,)) as ex:
        for h, o in ex.map(tu_keys, hs, chunksize=8):
            res[h] = o
    json.dump(res, open(out, "w"))
    print(f"TUs keyed: {len(res)} ({sum(v is None for v in res.values())} splitter failures)")


def analyze(work, kfile):
    K = json.load(open(kfile))
    commits, per = e10.load_tok(work)
    names = ["per-TU (token hash)", "E14 TU-free key", "E8 minimal slice", "body hash"]
    ch, n, tu_ch, all_free = [0] * 4, 0, 0, 0
    for d in per.values():
        for i in range(1, len(commits)):
            a, b = d.get(i - 1), d.get(i)
            if not a or not b or a[0] != "ok" or b[0] != "ok":
                continue
            ka, kb = K[a[1]], K[b[1]]
            if ka is None or kb is None:
                continue
            tuc = a[1] != b[1]
            common = set(ka) & set(kb)
            if tuc and common:
                tu_ch += 1
                all_free += all(ka[t][0] != kb[t][0] for t in common)
            for t in common:
                n += 1
                c = [tuc] + [ka[t][j] != kb[t][j] for j in range(3)]
                for j in range(4):
                    ch[j] += c[j]
    print(f"commits {len(commits)}; function-transitions {n}")
    print(f"{'key':22s} {'changes':>8s} {'rate':>7s} {'1/rate':>8s} {'vs per-TU':>9s}")
    for j, k in enumerate(names):
        r = ch[j] / n
        print(f"{k:22s} {ch[j]:8d} {r:7.4f} {1 / r:8.1f} {ch[0] / ch[j]:8.2f}x")
    print(f"changed-TU transitions (TU-level, >=1 common extern) {tu_ch}; "
          f"every E14 key in the TU changed: {all_free} ({all_free / tu_ch:.1%})")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "keys":
        keys(sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else os.cpu_count())
    elif cmd == "analyze":
        analyze(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
