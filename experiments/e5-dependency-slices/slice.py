#!/usr/bin/env python3
"""E5: verified closure slices for every function in a preprocessed TU.

Usage: slice.py <chibicc-binary> <tu.i> <out.csv> [--stubs] [<slices-dir>]

--stubs: referenced functions defined in the TU are emitted as header + '{}'
instead of a prototype (chibicc's asm depends on whether a callee is defined).

Splits the -E output into top-level declarations, computes each function's
closure slice (identifiers -> declarations, struct/union/enum tags in their own
namespace, other functions contribute only prototypes, fixpoint, original
order), compiles slice+function with chibicc -S and compares the function's
normalized asm against the same function compiled from the full TU.
"""
import os
import re
import subprocess
import sys
import tempfile

TOKEN_RE = re.compile(
    r"""
    (?P<ws>\s+)
  | (?P<str>"(?:\\.|[^"\\\n])*")
  | (?P<chr>'(?:\\.|[^'\\\n])*')
  | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<num>\.?[0-9](?:[eEpP][+-]|[A-Za-z0-9_.])*)
  | (?P<punct>\.\.\.|<<=|>>=|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|\#\#|.)
    """,
    re.S | re.X,
)

C_KEYWORDS = set("""
auto break case char const continue default do double else enum extern float for
goto if inline int long register restrict return short signed sizeof static struct
switch typedef union unsigned void volatile while _Alignas _Alignof _Atomic _Bool
_Complex _Generic _Imaginary _Noreturn _Static_assert _Thread_local __attribute__
__restrict __restrict__ __inline __inline__ __asm__ asm typeof __typeof__ __extension__
__volatile__ __const __signed__ __alignof__ noreturn
""".split())
TAGKW = {"struct", "union", "enum"}
DECL_NEXT = {";", ",", "=", "[", "(", ")", "__attribute__", "__asm__", "asm"}


def tokenize(src):
    """List of (kind, text, start, end); the -E output has no directives."""
    toks, pos = [], 0
    while pos < len(src):
        m = TOKEN_RE.match(src, pos)
        if m.lastgroup != "ws":
            toks.append(("id" if m.lastgroup == "id" else "p", m.group(0), m.start(), m.end()))
        pos = m.end()
    return toks


class Decl:
    def __init__(self, idx, toks):
        self.idx, self.toks = idx, toks
        self.is_func = False
        self.name = None          # function name if is_func
        self.header = None        # tokens before the body '{' if is_func
        self.decl_ord = set()     # ordinary-namespace names declared
        self.decl_tag = set()     # tags defined (with body) or forward-declared
        self.ref_ord = set()
        self.ref_tag = set()


def split(toks):
    """Top-level declarations: end at ';' at depth 0, or at the '}' closing a
    function body (a depth-0 '{' preceded by ')')."""
    decls, start, depth, i, fbody, eq = [], 0, 0, 0, False, False
    for i, (_, t, _, _) in enumerate(toks):
        if t == "=" and depth == 0:
            eq = True  # initializer: a later ') {' is a compound literal
        if t in "([{":
            if t == "{" and depth == 0 and i > 0 and toks[i - 1][1] == ")" and not eq:
                fbody = True
            depth += 1
        elif t in ")]}":
            depth -= 1
            if t == "}" and depth == 0 and fbody:
                decls.append(toks[start:i + 1]); start = i + 1; fbody = False; eq = False
        elif t == ";" and depth == 0:
            decls.append(toks[start:i + 1]); start = i + 1; eq = False
    assert start == len(toks), "trailing tokens"
    return [analyze(k, d) for k, d in enumerate(decls)]


def analyze(k, toks):
    d = Decl(k, toks)
    T = [t for _, t, _, _ in toks]
    K = [kd for kd, _, _, _ in toks]
    n = len(T)
    # function definition?
    if T[-1] == "}":
        pd = 0
        for j, t in enumerate(T):
            if t == "(":
                if pd == 0 and d.name is None:
                    cand = [T[x] for x in range(j) if K[x] == "id" and T[x] not in C_KEYWORDS]
                    d.name = cand[-1] if cand else "?"
                pd += 1
            elif t == ")":
                pd -= 1
            elif t == "{" and pd == 0 and T[j - 1] == ")":
                d.is_func, d.header = True, toks[:j]
                break
    # walk with a context stack: entries are '(' grouping/'(' other, '{' tagbody/enumbody/other, '['
    stack = []
    after_tag = None  # pending tag keyword for the next '{'
    for j in range(n):
        t, kd = T[j], K[j]
        prev = T[j - 1] if j else None
        nxt = T[j + 1] if j + 1 < n else None
        if t == "(":
            stack.append("(g" if nxt == "*" else "(")
            continue
        if t == "{":
            if d.is_func and not stack:
                stack.append("{body")
            elif after_tag:
                stack.append("{enum" if after_tag == "enum" else "{su")
            else:
                stack.append("{")
            after_tag = None
            continue
        if t == "[":
            stack.append("["); continue
        if t in ")]}":
            stack.pop(); continue
        if t in TAGKW:
            after_tag = t
            continue
        if kd != "id":
            continue
        if t in C_KEYWORDS:
            continue
        if prev in TAGKW:
            # tag name
            if nxt == "{" or (nxt == ";" and not stack and j == 1):
                d.decl_tag.add(t)
            else:
                d.ref_tag.add(t)
            after_tag = prev if nxt == "{" else None
            continue
        after_tag = None
        if prev in (".", "->"):
            continue  # member access / designator
        in_body = "{body" in stack
        if in_body:
            d.ref_ord.add(t)
            continue
        top = stack[-1] if stack else None
        # declared ordinary names
        if top == "{enum" and prev in ("{", ",") and nxt in (",", "=", "}"):
            d.decl_ord.add(t); continue
        if not d.is_func and all(s == "(g" for s in stack) and nxt in DECL_NEXT \
                and (not stack or prev == "*" or prev == "("):
            d.decl_ord.add(t); continue
        if d.is_func and t == d.name and not stack:
            d.decl_ord.add(t); continue
        # member / parameter declarator names (not references)
        if stack and nxt in (",", ")", ";", "[", ":") and prev not in ("(", ",", "{", ";", "[", "=") \
                and prev is not None:
            continue
        d.ref_ord.add(t)
    if d.is_func:
        d.decl_ord = {d.name}
    return d


def header_refs(d):
    """Refs of a function's prototype (header tokens only)."""
    h = analyze(d.idx, d.header + [("p", ";", 0, 0)])
    return h.ref_ord, h.ref_tag


def closure(decls, target, stubs):
    """Return (included decl indices, function defs to emit as prototypes, or
    as empty-bodied stubs when stubs=True)."""
    ord_prov, tag_prov, fdef = {}, {}, {}
    for d in decls:
        if d.is_func:
            fdef[d.name] = d
        else:
            for nme in d.decl_ord:
                ord_prov.setdefault(nme, []).append(d)
        for tg in d.decl_tag:
            tag_prov.setdefault(tg, []).append(d)
    hdr_cache = {}
    inc = set()          # indices of non-function decls included
    protos = set()       # indices of function defs included as synthesized prototypes
    work_ord = set(target.ref_ord) | {target.name}
    work_tag = set(target.ref_tag)
    seen_ord, seen_tag = set(), set()
    while work_ord or work_tag:
        new_o, new_t = set(), set()
        for nme in work_ord - seen_ord:
            seen_ord.add(nme)
            for d in ord_prov.get(nme, []):
                if d.idx not in inc:
                    inc.add(d.idx); new_o |= d.ref_ord; new_t |= d.ref_tag
            f = fdef.get(nme)
            if f is not None and f is not target:
                earlier = any(p.idx < target.idx for p in ord_prov.get(nme, []))
                if (stubs or (not earlier and f.idx < target.idx)) and f.idx not in protos:
                    protos.add(f.idx)
                    if f.idx not in hdr_cache:
                        hdr_cache[f.idx] = header_refs(f)
                    new_o |= hdr_cache[f.idx][0]; new_t |= hdr_cache[f.idx][1]
        for tg in work_tag - seen_tag:
            seen_tag.add(tg)
            for d in tag_prov.get(tg, []):
                if d.idx not in inc and not d.is_func:
                    inc.add(d.idx); new_o |= d.ref_ord; new_t |= d.ref_tag
        work_ord, work_tag = new_o - seen_ord, new_t - seen_tag
    return inc, protos


def func_asm(asm, name):
    """Text of one function from chibicc -S output, normalized."""
    lines = asm.split("\n")
    out, on = [], False
    for j, ln in enumerate(lines):
        s = ln.strip()
        if s.startswith((".globl ", ".local ")):
            on = (s.split()[1] == name and j + 2 < len(lines)
                  and lines[j + 2].strip() == f".type {name}, @function")
        if on:
            out.append(ln)
    if not out:
        return None
    out = [ln for ln in out if not ln.strip().startswith((".loc", ".file"))]
    text = "\n".join(out).rstrip()
    ren = {}
    def sub(m):
        key = m.group(0)
        if key not in ren:
            ren[key] = f".L.{m.group(1)}.#{len([k for k in ren if k.split('.')[2] == m.group(1)])}"
        return ren[key]
    return re.sub(r"\.L\.([A-Za-z_]*)\.(\d+)\b", sub, text)


def compile_s(cc, src_text, tmpdir, tag):
    c = os.path.join(tmpdir, tag + ".c")
    s = os.path.join(tmpdir, tag + ".s")
    with open(c, "w") as f:
        f.write(src_text)
    r = subprocess.run([cc, "-S", "-o", s, c], capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stderr.strip().splitlines() or ["?"])[-1]
    with open(s) as f:
        return f.read(), None


def main():
    cc, tu, out_csv = sys.argv[1:4]
    stubs = "--stubs" in sys.argv
    rest = [a for a in sys.argv[4:] if a != "--stubs"]
    sdir = rest[0] if rest else None
    src = open(tu).read()
    toks = tokenize(src)
    decls = split(toks)
    total = sum(len(d.toks) for d in decls)
    txt = lambda ts: src[ts[0][2]:ts[-1][3]]
    tmp = tempfile.mkdtemp(prefix="e5-")
    full_asm, err = compile_s(cc, src, tmp, "full")
    assert full_asm, err
    funcs = [d for d in decls if d.is_func]
    if sdir:
        os.makedirs(sdir, exist_ok=True)
    rows = []
    defined = {d.name for d in funcs}
    for f in funcs:
        inc, protos = closure(decls, f, stubs)
        parts, ntok = [], len(f.toks)
        for d in decls:
            if d.idx in inc:
                parts.append(txt(d.toks)); ntok += len(d.toks)
            elif d.idx in protos:
                if stubs:
                    parts.append(txt(d.header) + " {}"); ntok += len(d.header) + 2
                else:
                    parts.append(txt(d.header) + ";"); ntok += len(d.header) + 1
            elif d is f:
                parts.append(txt(f.toks))
        slice_src = "\n".join(parts) + "\n"
        if sdir:
            with open(os.path.join(sdir, f.name + ".c"), "w") as fh:
                fh.write(slice_src)
        asm, err = compile_s(cc, slice_src, tmp, "slice")
        want = func_asm(full_asm, f.name)
        if asm is None:
            status, cause = "compile_fail", err
        else:
            got = func_asm(asm, f.name)
            if got is None or want is None:
                status, cause = "missing", "function not emitted"
            elif got != want:
                status, cause = "mismatch", "asm differs"
                g = re.sub(r"mov (\S+)@GOTPCREL\(%rip\), %rax",
                           lambda m: f"lea {m.group(1)}(%rip), %rax" if m.group(1) in defined
                           else m.group(0), got)
                if g == want:
                    cause = "only callee address differs: lea (defined) vs GOTPCREL (declared)"
            else:
                status, cause = "ok", ""
        frac = ntok / total
        eff = frac if status == "ok" else 1.0
        ctx = (ntok - len(f.toks)) / total
        rows.append((f.name, len(f.toks), ntok, len(inc), len(protos), f"{frac:.4f}",
                     f"{ctx:.4f}", status, f"{eff:.4f}", cause.replace(",", ";")))
    with open(out_csv, "w") as fh:
        fh.write(f"# total_tokens={total} n_decls={len(decls)} n_funcs={len(funcs)}\n")
        fh.write("func,func_tokens,slice_tokens,n_decls,n_synth_protos,slice_frac,"
                 "context_frac,status,effective_frac,cause\n")
        for r in rows:
            fh.write(",".join(map(str, r)) + "\n")


if __name__ == "__main__":
    main()
