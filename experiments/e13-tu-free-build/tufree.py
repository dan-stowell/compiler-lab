#!/usr/bin/env python3
"""E13: TU-free split of one preprocessed Lua core file.

Usage:
  tufree.py check <tu.i> <targets.txt>            safety check (exit 1 on violation)
  tufree.py split <tu.i> <targets.txt> <outdir>   one <target>.c per target + stats.csv

<targets.txt>: the file's extern function names, one per line (the global T
symbols of its normal -O2 object).

split: for each target, the TU text with every *other* target's body replaced by
its header + ';'. Everything else (statics, data, types) is left as is; clang
drops unused statics.

check (the plan's rule): no address-taken static function and no non-const
file-scope or function-local static variable in the file. It also prints a
finer diagnostic: for each violating static, how many targets reach it through
static call/reference edges (a copy is only duplicated if >= 2 targets reach it).

Reuses E5's tokenizer/splitter via E8 (../e5-dependency-slices/slice.py).
"""
import csv
import importlib.util
import os
import re
import sys

_spec = importlib.util.spec_from_file_location("e5slice", os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "e5-dependency-slices", "slice.py"))
_e5 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_e5)
tokenize, split = _e5.tokenize, _e5.split
ID = re.compile(r"[A-Za-z_]\w*\Z")


def func_name(h):
    """Name in a function header: identifier before the first depth-0 '(' outside
    __attribute__((...)); `T (name) (args)` gives name."""
    j = 0
    while j < len(h):
        if h[j] in ("__attribute__", "__asm__", "asm"):
            depth, j = 0, j + 1
            while True:
                depth += {"(": 1, ")": -1}.get(h[j], 0)
                j += 1
                if depth == 0:
                    break
            continue
        if h[j] == "(":
            if j + 3 < len(h) and ID.match(h[j + 1]) and h[j + 2] == ")" and h[j + 3] == "(":
                return h[j + 1]
            return h[j - 1]
        j += 1
    raise ValueError(" ".join(h))


def load(tu):
    src = open(tu).read()
    decls = split(tokenize(src))
    for d in decls:
        if d.is_func:
            d.ref_ord.discard(d.name)
            d.name = func_name([t for _, t, _, _ in d.header])
            d.ref_ord.discard(d.name)
            d.static = "static" in [t for _, t, _, _ in d.header]
    return src, decls


def var_is_const(toks, name_idx):
    """toks: declaration tokens; name_idx: index of the declarator name.
    Pointer declarators need `const` right before the name (T *const p)."""
    before = [t for t in toks[:name_idx]]
    if "*" in before:
        return before[-1] == "const"
    return "const" in before


def static_vars(T, start, end):
    """Static variable declarations in T[start:end]: yields (name, const?)."""
    j = start
    while j < end:
        if T[j] == "static":
            k, depth = j, 0
            # declaration ends at ';' at depth 0 or at '=' (initializer)
            while k < end and not (depth == 0 and T[k] in (";", "=", ",")):
                depth += {"(": 1, "[": 1, "{": 1, ")": -1, "]": -1, "}": -1}.get(T[k], 0)
                k += 1
            seg = T[j:k]
            # strip array suffixes to find the name
            m = len(seg) - 1
            while m > 0 and seg[m] == "]":
                d = 0
                while True:
                    d += {"]": 1, "[": -1}.get(seg[m], 0)
                    m -= 1
                    if d == 0:
                        break
            if m > 0 and ID.match(seg[m]) and not (m + 1 < len(seg) and seg[m + 1] == "("):
                yield seg[m], var_is_const(seg, m)
            j = k
        j += 1


def analyze(decls, targets):
    funcs = {d.name: d for d in decls if d.is_func}
    statics = {n for n, d in funcs.items() if d.static}
    viol = []  # (kind, name, where)
    # address-taken static functions: name not followed by '(' outside its own header
    for d in decls:
        T = [t for _, t, _, _ in d.toks]
        hdr = len(d.header) if d.is_func else 0
        own = d.name if d.is_func else None
        # names redeclared as locals in this body (`int reglevel = ...`) are shadowed
        shadow = {T[j] for j in range(hdr, len(T) - 1)
                  if T[j] in statics and ID.match(T[j - 1]) and T[j - 1] != "return"}
        for j, t in enumerate(T):
            if t in shadow:
                continue
            if t in statics and not (j + 1 < len(T) and T[j + 1] == "("):
                if j < hdr and t == own:
                    continue
                prev = T[j - 1] if j else ""
                if prev in (".", "->") or (ID.match(prev) and prev != "return"):
                    continue  # member access, or a local declarator shadowing the name
                if not d.is_func and j > 0 and T[j - 1] not in ("=", ",", "{", "&", "(", "return"):
                    continue  # prototype's own name position etc.
                viol.append(("address-taken static function", t, own or "file scope"))
        if d.is_func:
            for name, const in static_vars(T, hdr, len(T)):
                if not const:
                    viol.append(("non-const function-local static", name, d.name))
        elif T and "static" in T and "typedef" not in T:
            # file-scope static: skip function prototypes
            for name, const in static_vars(T, 0, len(T)):
                if not const:
                    viol.append(("non-const file-scope static", name, "file scope"))
    # reachability through static bodies/variables from each target
    statvar_decl = {}
    for d in decls:
        if not d.is_func:
            for n in d.decl_ord:
                statvar_decl[n] = d
    reach = {}
    for t in targets:
        seen, work = set(), [t]
        while work:
            n = work.pop()
            d = funcs.get(n) or statvar_decl.get(n)
            if d is None or (n != t and n in funcs and n not in statics):
                continue
            for r in d.ref_ord:
                if r not in seen and (r in statics or r in statvar_decl):
                    seen.add(r); work.append(r)
        reach[t] = seen
    return viol, reach, funcs, statics


HIDDEN = '__attribute__((visibility("hidden")))'


def classify(viol, reach, targets, fname):
    """Amended rule: an address-taken static reached by one target stays static;
    one reached by several is promoted to a single hidden global named
    __tuf_<file>_<name>, defined in the first such target's object.
    -> (hard violations, {static: (new name, owner target)}, report lines)."""
    hard, promote, lines = [], {}, []
    for kind, name, where in viol:
        users = [t for t in targets if name in reach[t]]
        if kind != "address-taken static function":
            hard.append((kind, name, where))
            lines.append(f"VIOLATION {fname}: {kind} '{name}' in {where}; reached by {len(users)} target(s)")
        elif len(users) >= 2:
            if name not in promote:
                promote[name] = (f"__tuf_{fname}_{name}", users[0])
                lines.append(f"PROMOTE {fname}: address-taken static '{name}' reached by {len(users)} targets"
                             f" -> hidden global __tuf_{fname}_{name}, defined with {users[0]}")
        else:
            lines.append(f"OK {fname}: address-taken static '{name}' (in {where}) reached by "
                         f"{len(users)} target(s); stays static")
    return hard, promote, lines


def main():
    mode, tu, tfile = sys.argv[1:4]
    targets = [ln.strip() for ln in open(tfile) if ln.strip()]
    src, decls = load(tu)
    funcs = {d.name: d for d in decls if d.is_func}
    missing = [t for t in targets if t not in funcs or funcs[t].static]
    assert not missing, f"targets without an extern definition: {missing}"
    targets.sort(key=lambda t: funcs[t].idx)  # source order
    viol, reach, funcs, statics = analyze(decls, targets)
    fname = os.path.basename(tu)[:-2]
    hard, promote, lines = classify(viol, reach, targets, fname)
    if mode == "check":
        for ln in dict.fromkeys(lines):
            print(ln)
        print(f"{fname}: {len(hard)} violation(s), {len(promote)} promoted, {len(targets)} targets, "
              f"{len(statics)} static functions")
        sys.exit(1 if hard else 0)
    assert not hard, "safety check fails"
    outdir = sys.argv[4]
    os.makedirs(outdir, exist_ok=True)
    total = sum(len(d.toks) for d in decls)

    def text(d, body):
        """Decl text, promoted statics renamed (not after . or ->) and their
        `static` replaced by hidden visibility; body=False gives header + ';'."""
        toks = d.toks if body else d.header
        declares = (d.is_func and d.name in promote) or (d.decl_ord & promote.keys())
        out, pos = [], toks[0][2]
        for j, (_, t, a, b) in enumerate(toks):
            rep = None
            if t in promote and (j == 0 or toks[j - 1][1] not in (".", "->")):
                rep = promote[t][0]
            elif t == "static" and declares:
                rep = HIDDEN
            if rep is not None:
                out.append(src[pos:a] + rep); pos = b
        out.append(src[pos:toks[-1][3]])
        return "".join(out) + ("" if body else ";")

    with open(os.path.join(outdir, "stats.csv"), "w", newline="") as f, \
            open(os.path.join(outdir, "keep.txt"), "w") as kf:
        w = csv.writer(f)
        w.writerow(["file", "target", "tu_tokens", "slice_tokens", "static_funcs_reached"])
        for t in targets:
            owned = [new for old, (new, own) in promote.items() if own == t]
            parts = []
            for d in decls:
                if d.is_func and d.name in targets and d.name != t:
                    parts.append(text(d, False))
                elif d.is_func and d.name in promote and promote[d.name][1] != t:
                    parts.append(text(d, False))
                else:
                    parts.append(text(d, True))
            open(os.path.join(outdir, t + ".c"), "w").write("\n".join(parts) + "\n")
            kf.write(" ".join([t] + owned) + "\n")  # globals this object keeps
            # E8-style size: target body + bodies of the statics it reaches
            body = len(funcs[t].toks) + sum(len(funcs[n].toks) for n in reach[t] if n in funcs)
            w.writerow([fname, t, total, body, sum(1 for n in reach[t] if n in funcs)])


if __name__ == "__main__":
    main()
