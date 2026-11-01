#!/usr/bin/env python3
"""E2: Merkle ripple of closure keys over chibicc's first-parent history.

Usage: ripple.py <repo> <rev> <out.csv> [--include-tests]

Per commit (vs its first parent): #functions, fraction whose body hash changed
(incl. added), fraction whose closure key changed (incl. added), #removed,
largest SCC size of the call graph.
"""
import hashlib
import re
import subprocess
import sys
from collections import defaultdict

TOKEN_RE = re.compile(
    r"""
    (?P<ws>[ \t\r\f\v]+)
  | (?P<nl>\n)
  | (?P<lcom>//[^\n]*)
  | (?P<bcom>/\*.*?\*/)
  | (?P<str>"(?:\\.|[^"\\\n])*")
  | (?P<chr>'(?:\\.|[^'\\\n])*')
  | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<num>\.?[0-9](?:[eEpP][+-]|[A-Za-z0-9_.])*)
  | (?P<punct>\.\.\.|<<=|>>=|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|\#\#|.)
    """,
    re.S | re.X,
)

KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "do", "else",
            "_Alignof", "_Generic", "__attribute__", "typeof", "__typeof__"}


def tokenize(src):
    """Return list of (kind, text). Whitespace/comments dropped. A preprocessor
    directive line (with continuations) becomes one 'pp' token whose braces are
    ignored for depth tracking but which still contributes to body hashes."""
    toks = []
    pos, n = 0, len(src)
    bol = True  # at beginning of line (only whitespace seen)
    while pos < n:
        if bol:
            m = re.compile(r"[ \t]*#").match(src, pos)
            if m:
                # consume directive incl. backslash continuations and comments
                end = pos
                buf = []
                while True:
                    nl = src.find("\n", end)
                    if nl == -1:
                        nl = n
                    line = src[end:nl]
                    buf.append(line)
                    end = nl + 1
                    if not line.endswith("\\") or end >= n:
                        break
                text = "\n".join(buf)
                text = re.sub(r"/\*.*?\*/|//[^\n]*", " ", text, flags=re.S)
                text = " ".join(text.replace("\\\n", " ").split())
                toks.append(("pp", text))
                pos = end
                continue
        m = TOKEN_RE.match(src, pos)
        kind = m.lastgroup
        text = m.group(0)
        pos = m.end()
        if kind == "nl":
            bol = True
            continue
        if kind == "ws":
            continue
        if kind == "bcom":
            if "\n" in text:
                bol = True
            continue
        if kind == "lcom":
            continue
        bol = False
        toks.append(("id" if kind == "id" else "p", text))
    return toks


def split_functions(toks):
    """Yield (name, body_tokens) for top-level definitions: identifier ( ... ) {
    at brace depth 0. Body tokens span from after the previous top-level
    terminator (';', '}' or directive) through the closing brace, so return type
    and signature are included."""
    out = []
    depth = 0
    start = 0  # start of current top-level declaration
    i, n = 0, len(toks)
    while i < n:
        kind, t = toks[i]
        if kind == "pp":
            if depth == 0:
                start = i + 1
            i += 1
            continue
        if depth == 0:
            if t == ";":
                start = i + 1
            elif kind == "id" and t not in KEYWORDS and i + 1 < n and toks[i + 1][1] == "(":
                # match parens
                j, pd = i + 1, 0
                while j < n:
                    if toks[j][0] != "pp":
                        if toks[j][1] == "(":
                            pd += 1
                        elif toks[j][1] == ")":
                            pd -= 1
                            if pd == 0:
                                break
                    j += 1
                if j + 1 < n and toks[j + 1][1] == "{":
                    # find matching close brace
                    k, bd = j + 1, 0
                    while k < n:
                        if toks[k][0] != "pp":
                            if toks[k][1] == "{":
                                bd += 1
                            elif toks[k][1] == "}":
                                bd -= 1
                                if bd == 0:
                                    break
                        k += 1
                    out.append((t, toks[start:k + 1]))
                    i = k + 1
                    start = i
                    continue
            if t == "{":
                depth += 1
            elif t == "}":
                pass
        else:
            if t == "{":
                depth += 1
            elif t == "}":
                depth -= 1
                if depth == 0:
                    # end of a top-level struct/enum/initializer body; decl ends at ';'
                    pass
        i += 1
    return out


def h(*parts):
    m = hashlib.sha256()
    for p in parts:
        m.update(p.encode())
        m.update(b"\0")
    return m.hexdigest()


class Repo:
    def __init__(self, path):
        self.path = path
        self.cache = {}  # blob sha -> list of (name, body_hash, ident_set)
        self.cat = subprocess.Popen(["git", "-C", path, "cat-file", "--batch"],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def blob(self, sha):
        self.cat.stdin.write((sha + "\n").encode())
        self.cat.stdin.flush()
        hdr = self.cat.stdout.readline().split()
        size = int(hdr[2])
        data = self.cat.stdout.read(size)
        self.cat.stdout.read(1)
        return data.decode("utf-8", "replace")

    def ls(self, rev, include_tests):
        out = subprocess.run(["git", "-C", self.path, "ls-tree", "-r", rev],
                             capture_output=True, text=True, check=True).stdout
        res = []
        for line in out.splitlines():
            meta, path = line.split("\t", 1)
            if not path.endswith(".c"):
                continue
            if not include_tests and path.startswith("test/"):
                continue
            res.append((path, meta.split()[2]))
        return res

    def funcs(self, sha):
        if sha not in self.cache:
            fs = []
            for name, body in split_functions(tokenize(self.blob(sha))):
                bh = h(*(t for _, t in body))
                ids = {t for k, t in body if k == "id"}
                fs.append((name, bh, ids))
            self.cache[sha] = fs
        return self.cache[sha]


def analyze(repo, rev, include_tests):
    """Return dict ident -> (body_hash, closure_key), and largest SCC size."""
    per_file = []  # (path, name, bh, ids)
    for path, sha in repo.ls(rev, include_tests):
        for name, bh, ids in repo.funcs(sha):
            per_file.append((path, name, bh, ids))
    files_of = defaultdict(set)
    for path, name, _, _ in per_file:
        files_of[name].add(path)
    # identity: name, or path:name when the name is defined in >1 file.
    # Same name defined twice in one file (e.g. #if branches) -> merged unit.
    def ident(path, name):
        return name if len(files_of[name]) == 1 else f"{path}:{name}"
    bodies = defaultdict(list)
    ids_of = defaultdict(set)
    path_of = {}
    for path, name, bh, ids in per_file:
        u = ident(path, name)
        bodies[u].append(bh)
        ids_of[u] |= ids
        path_of[u] = path
    body = {u: h(*sorted(v)) for u, v in bodies.items()}
    # call edges
    edges = {}
    for u in body:
        tgt = set()
        for name in ids_of[u]:
            if name not in files_of:
                continue
            if len(files_of[name]) == 1:
                tgt.add(name)
            elif path_of[u] in files_of[name]:
                tgt.add(f"{path_of[u]}:{name}")
            else:  # ambiguous static from another file: depend on all copies
                tgt |= {f"{p}:{name}" for p in files_of[name]}
        tgt.discard(u)  # self-reference is covered by the body itself
        edges[u] = sorted(tgt)
    # Tarjan (iterative); SCCs emitted callees-first
    index, low, onstack, stack, sccs = {}, {}, set(), [], []
    counter = 0
    for root in sorted(body):
        if root in index:
            continue
        work = [(root, 0)]
        while work:
            v, pi = work.pop()
            if pi == 0:
                index[v] = low[v] = counter
                counter += 1
                stack.append(v)
                onstack.add(v)
            recurse = False
            es = edges[v]
            while pi < len(es):
                w = es[pi]
                pi += 1
                if w not in index:
                    work.append((v, pi))
                    work.append((w, 0))
                    recurse = True
                    break
                elif w in onstack:
                    low[v] = min(low[v], index[w])
            if recurse:
                continue
            if low[v] == index[v]:
                comp = []
                while True:
                    w = stack.pop()
                    onstack.discard(w)
                    comp.append(w)
                    if w == v:
                        break
                sccs.append(comp)
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[v])
    key = {}
    for comp in sccs:
        members = set(comp)
        ext = sorted({key[w] for v in comp for w in edges[v] if w not in members})
        k = h("SCC", *sorted(f"{v}={body[v]}" for v in comp), "EXT", *ext)
        for v in comp:
            key[v] = k
    return {u: (body[u], key[u]) for u in body}, max((len(c) for c in sccs), default=0), sccs


def main():
    repo_path, rev, out_csv = sys.argv[1:4]
    include_tests = "--include-tests" in sys.argv
    repo = Repo(repo_path)
    revs = subprocess.run(["git", "-C", repo_path, "rev-list", "--first-parent",
                           "--reverse", rev], capture_output=True, text=True,
                          check=True).stdout.split()
    prev = None
    with open(out_csv, "w") as f:
        f.write("idx,commit,n_funcs,n_added,n_removed,body_changed,closure_changed,"
                "body_frac,closure_frac,largest_scc\n")
        for i, r in enumerate(revs):
            cur, big, _ = analyze(repo, r, include_tests)
            if prev is not None:
                added = [u for u in cur if u not in prev]
                removed = [u for u in prev if u not in cur]
                bch = sum(1 for u in cur if u not in prev or prev[u][0] != cur[u][0])
                cch = sum(1 for u in cur if u not in prev or prev[u][1] != cur[u][1])
                n = len(cur)
                bf = bch / n if n else 0.0
                cf = cch / n if n else 0.0
                f.write(f"{i},{r[:7]},{n},{len(added)},{len(removed)},{bch},{cch},"
                        f"{bf:.4f},{cf:.4f},{big}\n")
            prev = cur


if __name__ == "__main__":
    main()
