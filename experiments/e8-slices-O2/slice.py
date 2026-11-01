#!/usr/bin/env python3
"""E8: smallest callee-body depth whose slice reproduces a target's clang -O2 asm.

Usage: slice.py <tu.i> <tu-with-linemarkers.i> <orig.c> <out.csv> <slices-dir> <cc> [cc args...]
       slice.py --emit <tu.i> <target> <d> [--add-callers]   (print one slice at fixed d)

Reuses E5's token splitter (../e5-dependency-slices/slice.py: tokenize, split,
analyze, header_refs). Targets are the functions the full-TU compile emits as
.globl @function. For each target and d = 0, 1, 2, ... the slice holds:
  - bodies of the target and of every TU function within d call levels of it
    (a "call" is any reference to a TU-defined function name in a body),
  - every other referenced TU function as a prototype (its header + ';', at its
    definition's position), plus any forward declarations of it,
  - the context declarations (types, globals, prototypes) reachable from all of
    the above, to a fixpoint, in original order.
The target's normalized asm (see norm_func) is compared with the full TU's.
"""
import os
import re
import subprocess
import sys
import tempfile

import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location("e5slice", os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "e5-dependency-slices", "slice.py"))
_e5 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_e5)
tokenize, split, header_refs = _e5.tokenize, _e5.split, _e5.header_refs

DEBUG_DIRS = (".loc", ".file", ".cfi_")
# private labels (.LBB, .Ltmp, .LJTI, .LCPI, .L.str, .Lfunc_end, ...) and the
# BBn_m form clang's loop comments use for the same blocks
PRIV_RE = re.compile(r"\.L[A-Za-z0-9_.$]*[A-Za-z0-9_$]|\bBB\d+_\d+\b")
# no leading `$`: in `movl $.L.str.3, %esi` the referenced label is `.L.str.3`
SYM_RE = re.compile(r"[A-Za-z_.][A-Za-z0-9_.$]*")
LABEL_RE = re.compile(r"^([A-Za-z_.$][A-Za-z0-9_.$]*):")
DATA_END = (".type", ".section", ".size", ".text", ".globl", ".data", ".bss",
            ".local", ".comm", ".ident", ".addrsig")


def parse_asm(asm):
    """-> (functions: name -> lines, data: label -> lines incl. section)."""
    lines = [ln for ln in asm.split("\n") if not ln.strip().startswith(DEBUG_DIRS)]
    funcs, data, ftype = {}, {}, set()
    for ln in lines:
        m = re.match(r"\s*\.type\s+([^,\s]+),\s*@function", ln)
        if m:
            ftype.add(m.group(1))
    section, j = ".text", 0
    while j < len(lines):
        s = lines[j].strip()
        if s.startswith((".section", ".text", ".data", ".bss")):
            section = s
        m = LABEL_RE.match(lines[j])
        if m and m.group(1) in ftype:
            name, k = m.group(1), j
            while not re.match(rf"\s*\.size\s+{re.escape(name)},", lines[k]):
                k += 1
            start = j
            while start > 0 and not LABEL_RE.match(lines[start - 1]) and \
                    lines[start - 1].strip().startswith((".globl", ".p2align", ".type")):
                start -= 1
            funcs[name] = lines[start:k + 1]
            j = k + 1
            continue
        if m and section != ".text":
            k = j + 1
            while k < len(lines) and not LABEL_RE.match(lines[k]) and \
                    not lines[k].strip().startswith(DATA_END) and \
                    not lines[k].strip().startswith("# -- End"):
                k += 1
            data[m.group(1)] = [section] + lines[j:k]
            j = k
            continue
        j += 1
    return funcs, data


def norm_func(parsed, name):
    """Function text + referenced data blocks (transitively, by label), with
    private labels renamed bijectively in order of first occurrence."""
    funcs, data = parsed
    if name not in funcs:
        return None
    out = list(funcs[name])
    seen, queue = set(), [out]
    order = []
    while queue:
        blk = queue.pop(0)
        for ln in blk:
            code = ln.split("#", 1)[0]
            for sym in SYM_RE.findall(code):
                if sym in data and sym not in seen:
                    seen.add(sym); order.append(sym); queue.append(data[sym])
    for sym in order:
        out += data[sym]
    ren = {}

    def sub(m):
        key = m.group(0)
        if key.startswith("BB"):
            key = ".L" + key
        if key not in ren:
            ren[key] = f".L#{len(ren)}"
        return ren[key] if m.group(0).startswith(".") else ren[key][2:]
    # renaming changes label lengths, which shifts clang's comment padding:
    # collapse whitespace runs (formatting only; no line or token is dropped)
    return "\n".join(collapse(PRIV_RE.sub(sub, ln)) for ln in out)


def collapse(ln):
    """Collapse whitespace runs outside quoted strings (from the first '"' on,
    the line is kept verbatim, so .asciz/.ascii contents are compared exactly)."""
    q = ln.find('"')
    if q < 0:
        return " ".join(ln.split())
    return " ".join(ln[:q].split()) + " " + ln[q:]


def sys_offsets(src, lines_tu):
    """Start offsets (in the -P TU src) of tokens that come from system headers,
    using the same TU preprocessed with line markers. Asserts identical tokens."""
    toks, sysflag, cur = [], [], ""
    for ln in lines_tu.split("\n"):
        m = re.match(r'#\s*(?:line\s+)?\d+\s+"([^"]*)"', ln)
        if m:
            cur = m.group(1)
            continue
        for t in tokenize(ln):
            toks.append(t[1]); sysflag.append(cur.startswith("/"))
    ptoks = tokenize(src)
    assert [t[1] for t in ptoks] == toks, "-P and line-marker TUs differ"
    return {t[2] for t, f in zip(ptoks, sysflag) if f}


def func_name(h):
    """Identifier before the first depth-0 '(' of a header, skipping
    __attribute__((...)) groups."""
    j = 0
    while j < len(h):
        if h[j] == "__attribute__":
            depth, j = 0, j + 1
            while True:
                depth += {"(": 1, ")": -1}.get(h[j], 0)
                j += 1
                if depth == 0:
                    break
            continue
        if h[j] == "(":
            return h[j - 1]
        j += 1
    raise ValueError(" ".join(h))


def compile_s(cc, src_text, tmpdir, tag):
    c = os.path.join(tmpdir, tag + ".c")
    s = os.path.join(tmpdir, tag + ".s")
    with open(c, "w") as f:
        f.write(src_text)
    r = subprocess.run(cc + ["-S", "-o", s, c], capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stderr.strip().splitlines() or ["?"])[-1]
    with open(s) as f:
        return f.read(), None


class TU:
    """A preprocessed TU split into top-level declarations, with its call graph
    and slice builder. sysoff: token start offsets from system headers (only for
    the system-token counts; may be empty)."""

    def __init__(self, src, sysoff=frozenset()):
        self.src = src
        decls = self.decls = split(tokenize(src))
        self.total = sum(len(d.toks) for d in decls)
        self.sysoff = sysoff
        self.total_sys = self.nsys([t for d in decls for t in d.toks])
        funcs = self.funcs = [d for d in decls if d.is_func]
        for f in funcs:  # E5 names `void __attribute__((noreturn)) f(...)` "?"; fix
            f.ref_ord.discard(f.name)
            f.name = func_name([t for _, t, _, _ in f.header])
            f.ref_ord.discard(f.name)
        assert len({f.name for f in funcs}) == len(funcs), "function names not unique"
        for d in decls:  # E5 misses parenthesized declarators: `extern void (lua_xxx) (...);`
            if not d.is_func:
                T = [t for _, t, _, _ in d.toks]
                for j in range(1, len(T) - 2):
                    if T[j - 1] == "(" and T[j + 1] == ")" and T[j + 2] == "(" and \
                            re.fullmatch(r"[A-Za-z_]\w*", T[j]) and j >= 2 and T[j - 2] not in ("(", ","):
                        d.decl_ord.add(T[j])
                        if T[j - 2] in d.decl_ord:  # the return type was taken as the name
                            d.decl_ord.discard(T[j - 2]); d.ref_ord.add(T[j - 2])
        self.fdef = fdef = {d.name: d for d in funcs}
        self.statics = [d for d in funcs if "static" in [t for _, t, _, _ in d.header]]
        self.ord_prov, self.tag_prov = {}, {}
        for d in decls:
            if not d.is_func:
                for n in d.decl_ord:
                    self.ord_prov.setdefault(n, []).append(d)
            for tg in d.decl_tag:
                if not d.is_func:
                    self.tag_prov.setdefault(tg, []).append(d)
        self.calls = {f.name: {n for n in f.ref_ord if n in fdef and n != f.name} for f in funcs}
        self.hdr = {f.name: header_refs(f) for f in funcs}
        self.callers = {f.name: set() for f in funcs}
        for f, cs in self.calls.items():
            for c in cs:
                self.callers[c].add(f)
        self.static_names = {f.name for f in self.statics}

    def nsys(self, ts):
        return sum(1 for t in ts if t[2] in self.sysoff)

    def txt(self, ts):
        return self.src[ts[0][2]:ts[-1][3]]

    def build(self, bodies):
        """Slice source, token count and system-header token count for a set of
        function bodies."""
        fdef, hdr, decls = self.fdef, self.hdr, self.decls
        inc, protos = set(), set()
        wo = set().union(*(fdef[b].ref_ord | {b} for b in bodies))
        wt = set().union(*(fdef[b].ref_tag for b in bodies))
        so, st = set(), set()
        while wo or wt:
            no, nt = set(), set()
            for n in wo - so:
                so.add(n)
                for d in self.ord_prov.get(n, []):
                    if d.idx not in inc:
                        inc.add(d.idx); no |= d.ref_ord; nt |= d.ref_tag
                if n in fdef and n not in bodies and n not in protos:
                    protos.add(n); no |= hdr[n][0]; nt |= hdr[n][1]
            for tg in wt - st:
                st.add(tg)
                for d in self.tag_prov.get(tg, []):
                    if d.idx not in inc:
                        inc.add(d.idx); no |= d.ref_ord; nt |= d.ref_tag
            wo, wt = no - so, nt - st
        parts, ntok = [], 0
        last_sys = 0
        for d in decls:
            if d.idx in inc:
                parts.append(self.txt(d.toks)); ntok += len(d.toks); last_sys += self.nsys(d.toks)
            elif d.is_func and d.name in bodies:
                parts.append(self.txt(d.toks)); ntok += len(d.toks); last_sys += self.nsys(d.toks)
            elif d.is_func and d.name in protos:
                parts.append(self.txt(d.header) + ";"); ntok += len(d.header) + 1
                last_sys += self.nsys(d.header)
        return "\n".join(parts) + "\n", ntok, last_sys

    def bodies_at(self, t, d, add_callers=False):
        """Bodies of the depth-d slice of t (callees within d levels; with
        add_callers, also all callers of every static callee in it). Returns
        (bodies, exhausted) where exhausted means level d added nothing new
        (d is beyond t's callee depth)."""
        body, frontier = {t}, {t}
        exhausted = False
        for _ in range(d):
            frontier = set().union(*(self.calls[f] for f in frontier)) - body
            if not frontier:
                exhausted = True
                break
            body |= frontier
        bodies = set(body)
        if add_callers:
            for c in body - {t}:
                if c in self.static_names:
                    bodies |= self.callers[c]
        return bodies, exhausted


def search(tu, t, want, cc, tmp, add_callers=False, keep_dir=None):
    """Smallest d whose slice reproduces t's normalized asm `want`. add_callers:
    also include the bodies of all callers of every static callee already in
    the slice. Returns ((d, ntok, nsys) or None, levels)."""
    levels, body, frontier, d = [], {t}, {t}, 0
    while True:
        bodies = set(body)
        if add_callers:
            for c in body - {t}:
                if c in tu.static_names:
                    bodies |= tu.callers[c]
        s, ntok, ns = tu.build(bodies)
        if keep_dir:
            with open(os.path.join(keep_dir, f"{t}.d{d}.c"), "w") as fh:
                fh.write(s)
        asm, err = compile_s(cc, s, tmp, "slice")
        ok = asm is not None and norm_func(parse_asm(asm), t) == want
        levels.append((d, len(bodies), ntok, "ok" if ok else ("compile_fail" if asm is None else "mismatch")))
        if ok:
            return (d, ntok, ns), levels
        frontier = set().union(*(tu.calls[f] for f in frontier)) - body
        if not frontier:
            return None, levels
        body |= frontier
        d += 1


def emit_main(argv):
    """--emit <tu.i> <target> <d> [--add-callers]: print t's depth-d slice."""
    tu = TU(open(argv[0]).read())
    bodies, _ = tu.bodies_at(argv[1], int(argv[2]), "--add-callers" in argv[3:])
    sys.stdout.write(tu.build(bodies)[0])


def main():
    if sys.argv[1] == "--emit":
        return emit_main(sys.argv[2:])
    tu_path, tu_lines, orig_c, out_csv, sdir = sys.argv[1:6]
    cc = sys.argv[6:]
    orig = open(orig_c, errors="replace").read()
    src = open(tu_path).read()
    tu = TU(src, sys_offsets(src, open(tu_lines).read()))
    decls, total, total_sys = tu.decls, tu.total, tu.total_sys
    txt = tu.txt
    tmp = tempfile.mkdtemp(prefix="e8-")
    os.makedirs(sdir, exist_ok=True)

    full_asm, err = compile_s(cc, src, tmp, "full")
    assert full_asm, err
    full = parse_asm(full_asm)
    targets = [ln.split()[1] for ln in full_asm.split("\n")
               if ln.strip().startswith(".globl") and ln.split()[1] in full[0]]

    funcs, fdef, statics, calls = tu.funcs, tu.fdef, tu.statics, tu.calls

    # Controls: the full TU recompiled, and re-joined from the splitter's
    # declarations, must reproduce every emitted function's normalized asm.
    again, _ = compile_s(cc, src, tmp, "again")
    rejoin, _ = compile_s(cc, "\n".join(txt(d.toks) for d in decls) + "\n", tmp, "rejoin")
    ctl = {}
    for tag, a in (("recompile", again), ("rejoin", rejoin)):
        p = parse_asm(a)
        ctl[tag] = sum(norm_func(p, n) == norm_func(full, n) for n in full[0])

    want = {t: norm_func(full, t) for t in targets}

    rows = []
    for t in targets:
        reach, fr, rd = {t}, {t}, 0  # full callee closure, for reference
        while True:
            fr = set().union(*(calls[f] for f in fr)) - reach
            if not fr:
                break
            reach |= fr; rd += 1
        res, levels = search(tu, t, want[t], cc, tmp, False, sdir)
        cres, clev = search(tu, t, want[t], cc, tmp, True)
        # callees the full TU inlined into t: referenced in t's body, not called/jumped/addressed in its asm
        tasm = "\n".join(full[0][t])
        inl = sorted(c for c in calls[t] if not re.search(rf"\b{re.escape(c)}\b", tasm))
        fmt = lambda r: (str(r[0]), f"{r[1] / total:.4f}", r[1]) if r else ("never", "1.0000", total)
        ssys = res[2] if res else total_sys
        lv = lambda L: " ".join(f"d{a}:{b}f/{c}t/{e}" for a, b, c, e in L)
        rows.append((t, *fmt(res), rd, len(reach), len(fdef[t].toks), *fmt(cres)[:2],
                     lv(levels), lv(clev), " ".join(inl), ssys))

    emitted_static = [n for n in full[0] if n not in targets]
    with open(out_csv, "w") as fh:
        fh.write(f"# total_tokens={total} sys_header_tokens={total_sys} n_decls={len(decls)} n_funcs={len(funcs)} "
                 f"n_static_defs={len(statics)} "
                 f"static_defs_named_in_source={sum(1 for f in statics if re.search(rf'\b{f.name}\s*\(', orig))} "
                 f"static_emitted_in_full_asm={len(emitted_static)} "
                 f"targets={len(targets)} control_recompile={ctl['recompile']}/{len(full[0])} "
                 f"control_rejoin={ctl['rejoin']}/{len(full[0])}\n")
        fh.write("target,smallest_d,slice_frac,slice_tokens,reach_max_d,reach_bodies,target_tokens,"
                 "callers_d,callers_frac,levels(d:bodies/tokens/status),callers_levels,inlined_in_full,"
                 "slice_sys_header_tokens\n")
        for r in rows:
            fh.write(",".join(map(str, r)) + "\n")


if __name__ == "__main__":
    main()
