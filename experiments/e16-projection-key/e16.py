#!/usr/bin/env python3
"""E16: layout-projection key for E13 units (journal/cycle7-plan.md).

The key of a unit (an E15 slice: target + reached statics, compiled alone) is a
hash over what its function bodies actually use, computed from clang's
-ast-dump=json and -fdump-record-layouts-complete (parse + Sema, no codegen):
  BODY  exact preprocessed body tokens of the target and every reached static
  N     every AST node in those bodies: kind, opcode, castKind, value, name,
        value category, and its resolved type (typedefs expanded recursively,
        anonymous records named by their path)
  M     each accessed field: record, field, bit offset, bit width, resolved
        field type, plus the containing record's size and alignment
  L     whole-record layout (every nested field line, resolved types) wherever a
        record is used by value (copied, initialized, passed, returned, local
        variable, anything but the base of '.' or the operand of '&') or is the
        pointee in pointer arithmetic / subscripting / ++ --
  SZ    sizeof/alignof: resolved argument type + size/align of the records in it
  E     value of each enum constant used; ET underlying type of enums used
  F     every declaration of each callee and of each unit function: resolved
        type, storage class, inline, all attributes (noreturn, pure, visibility..)
  G     every declaration of each global variable used: type, storage class,
        attributes; for const or defined-here globals the whole declaration text,
        and its initializer is walked like a body (deviation: the plan says const
        only; a global defined in the unit is emitted into the unit's .o)
  TY    sizeof, _Alignof, __alignof__ of every type mentioned, evaluated by clang
        in a syntax-only probe (typedef attributes, packed enums; this also gives
        every sizeof/alignof operand's evaluated value)
  CC    compiler and flags (pinned)
Fallback: any node kind outside KNOWN, an OffsetOfExpr, or an unresolvable
field/record/enum adds the whole declaration text (and layout) of the records
involved, or the whole enum; counted per unit.

Subcommands:
  key <file.c>                       print the key and items (debug)
  mutate                             mutation tests (synthetic + real Lua slice)
  keys <e15out> <out> <n|all> [jobs] projection keys for the slices of the first
                                     n slice-changed transitions (seeded order)
  analyze <e10work> <e15out> <out>   informative hits, false hits, ratio
"""
import bisect
import collections
import gzip
import hashlib
import json
import os
import random
import re
import resource
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "e15-output-churn"))
import e15  # noqa: E402

e10, e8 = e15.e10, e15.e8
CCV = e10.CC
FLAGS = ["-std=c99", "-O2", "-fno-stack-protector", "-fno-common", "-fno-pic", "-w"]
PIN = " ".join([CCV] + FLAGS + ["x86_64-linux-gnu"])

KNOWN = {
    "CompoundStmt", "DeclStmt", "VarDecl", "ParmVarDecl", "ReturnStmt", "IfStmt", "WhileStmt",
    "DoStmt", "ForStmt", "SwitchStmt", "CaseStmt", "DefaultStmt", "BreakStmt", "ContinueStmt",
    "GotoStmt", "LabelStmt", "NullStmt", "ImplicitCastExpr", "CStyleCastExpr", "DeclRefExpr",
    "IntegerLiteral", "FloatingLiteral", "CharacterLiteral", "StringLiteral", "ParenExpr",
    "BinaryOperator", "CompoundAssignOperator", "UnaryOperator", "ConditionalOperator",
    "CallExpr", "MemberExpr", "ArraySubscriptExpr", "UnaryExprOrTypeTraitExpr", "ConstantExpr",
    "InitListExpr", "ImplicitValueInitExpr", "VAArgExpr", "CompoundLiteralExpr", "FunctionDecl",
    "TypedefDecl", "RecordDecl", "EnumDecl", "FieldDecl", "EnumConstantDecl",
    "BinaryConditionalOperator", "OpaqueValueExpr", "StmtExpr", "PredefinedExpr",
    "AttributedStmt", "IndirectGotoStmt", "AddrLabelExpr",
}
# type-node kinds that appear under decls (e.g. a local typedef); skipped
TYPE_NODE = re.compile(r".*Type$")

ANON = re.compile(r"(?:[A-Za-z_]\w*::)*\((?:unnamed|anonymous)(?: struct| union| enum)? at [^)]*?:(\d+):(\d+)\)")
TOK = re.compile(r"\{anon:[^}]*\}|[A-Za-z_]\w*|\S")
RECNAME = re.compile(r"(struct|union)\s+(\{anon:[^}]*\}|[A-Za-z_]\w*)")
LAYOUT_LINE = re.compile(r"^\s*(\d+)(?::(\d+)-(\d+))?\s\|(\s+)(.*)$")


def H(s):
    return hashlib.sha1(s.encode()).hexdigest()[:20]


def plain(ct):
    """Resolved type without the typedef-expansion brackets (for structure tests)."""
    return " ".join(ct.replace("\u27e6", " ").replace("\u27e7", " ").split())


def clang(src, extra):
    r = subprocess.run([CCV] + FLAGS + ["-fsyntax-only", "-x", "c", "-"] + extra,
                       input=src, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr[-500:])
    return r.stdout


class Proj:
    def __init__(self, src):
        self.src = src
        self.lines = [0] + [m.end() for m in re.finditer("\n", src)]
        J = json.loads(clang(src, ["-Xclang", "-ast-dump=json"]))
        self.layout_txt = clang(src, ["-Xclang", "-fdump-record-layouts-complete"])
        self.fallbacks = collections.Counter()
        self.items = []
        self.node, self.parent = {}, {}
        self.typedef = {}          # name -> type dict
        self.typedef_owned = {}    # typedef name -> owned anonymous RecordDecl id
        self.recdef = {}           # record canonical name -> RecordDecl id (definition)
        self.funcs = collections.defaultdict(list)
        self.gvars = collections.defaultdict(list)
        self.enumval, self.enum_of = {}, {}
        self.top = J["inner"]
        self._index(J, None)
        self.anon_name = {}        # "L:C" -> canonical anon name
        self._name_anons()
        self._canon_memo = {}
        self.used_types = set()
        self._layouts()
        self.used_types = set()  # layout lines are not types; probe only what the walk uses

    # ------------------------------------------------------------ indexing
    def _index(self, n, parent):
        if "id" in n:
            self.node[n["id"]] = n
            if parent is not None:
                self.parent[n["id"]] = parent
        k = n.get("kind")
        if k == "TypedefDecl":
            self.typedef[n["name"]] = n["type"]
            for c in n.get("inner", []):
                o = c.get("ownedTagDecl")
                if o and not o.get("name"):
                    self.typedef_owned[n["name"]] = o["id"]
        elif k == "EnumDecl":
            prev = -1
            for c in n.get("inner", []):
                if c.get("kind") != "EnumConstantDecl":
                    continue
                v = None
                if c.get("inner"):
                    v = c["inner"][0].get("value")
                    v = int(v) if v is not None else None
                else:
                    v = prev + 1 if prev is not None else None
                self.enumval[c["id"]] = v
                self.enum_of[c["id"]] = n
                prev = v
        elif k == "FunctionDecl" and parent is not None and parent.get("kind") == "TranslationUnitDecl":
            self.funcs[n["name"]].append(n)
        elif k == "VarDecl" and parent is not None and parent.get("kind") == "TranslationUnitDecl":
            self.gvars[n["name"]].append(n)
        for c in n.get("inner", []):
            self._index(c, n)

    def text(self, n):
        r = n.get("range", {})
        b, e = r.get("begin", {}), r.get("end", {})
        if "offset" not in b or "offset" not in e:
            return None
        return "\0".join(t[1] for t in e8.tokenize(self.src[b["offset"]:e["offset"] + e.get("tokLen", 0)]))

    def lc(self, off):
        ln = bisect.bisect_right(self.lines, off)
        return f"{ln}:{off - self.lines[ln - 1] + 1}"

    def _name_anons(self):
        """Anonymous records get a location-free name: the containing record's
        name plus the field (or member index) they are the type of; a typedef'd
        one is 'struct <typedef>' as clang spells it; otherwise a hash of its text."""
        recs = [n for n in self.node.values() if n.get("kind") == "RecordDecl"]
        owned = {v: k for k, v in self.typedef_owned.items()}
        pending = [r for r in recs if not r.get("name")]

        def lckey(r):
            return self.lc(r["loc"]["offset"])
        for r in recs:
            if r.get("name") and r.get("completeDefinition"):
                self.recdef[f"{r['tagUsed']} {r['name']}"] = r["id"]
        for _ in range(8):  # nested anonymous records: resolve outer first
            left = []
            for r in pending:
                key = lckey(r)
                p = self.parent.get(r["id"])
                if r["id"] in owned:
                    name = f"{r['tagUsed']} {owned[r['id']]}"
                    self.anon_name[key] = None  # spelled by clang as the typedef
                    self.recdef[name] = r["id"]
                    continue
                if p is not None and p.get("kind") == "RecordDecl":
                    pname = p.get("name") and f"{p['tagUsed']} {p['name']}"
                    if not pname:
                        pk = lckey(p)
                        if pk not in self.anon_name:
                            left.append(r)
                            continue
                        pname = self.anon_name[pk] or "?"
                    sibs = p.get("inner", [])
                    i = sibs.index(r)
                    fld = next((s for s in sibs[i + 1:i + 2] if s.get("kind") == "FieldDecl"
                                and key in self._anon_keys(s["type"])), None)
                    name = f"{{anon:{pname}::{fld['name'] if fld else '#' + str(i)}}}"
                else:
                    name = f"{{anon:{H(self.text(r) or key)}}}"
                self.anon_name[key] = name
                self.recdef[f"{r['tagUsed']} {name}"] = r["id"]
            if not left:
                break
            pending = left
        for r in pending if left else []:
            self.fallbacks["anon_unresolved"] += 1

    def _anon_keys(self, t):
        s = (t.get("qualType", "") + " " + t.get("desugaredQualType", "")) if isinstance(t, dict) else t
        return {f"{a}:{b}" for a, b in ANON.findall(s)}

    # ------------------------------------------------------------ types
    def canon(self, t):
        if t is None:
            return None
        s = t.get("qualType") if isinstance(t, dict) else t
        self.used_types.add(s)
        if s in self._canon_memo:
            return self._canon_memo[s]
        self._canon_memo[s] = s  # cycle guard

        def anon(m):
            nm = self.anon_name.get(f"{m.group(1)}:{m.group(2)}")
            if nm is None:
                self.fallbacks["anon_unnamed_in_type"] += 1
                return "{anon:?}"
            return nm
        x = ANON.sub(anon, s)
        out, prev = [], ""
        for tk in TOK.findall(x):
            if tk in self.typedef and prev not in ("struct", "union", "enum"):
                out.append("\u27e6 " + self.canon(self.typedef[tk]) + " \u27e7")
            else:
                out.append(tk)
            prev = tk
        r = " ".join(out)
        self._canon_memo[s] = r
        return r

    # ------------------------------------------------------------ layouts
    def _layouts(self):
        self.layout = {}  # record canonical name -> (lines, size, align, fields{name:(off,bits,type)})
        blocks = self.layout_txt.split("*** Dumping AST Record Layout")
        for b in blocks[1:]:
            lines = [ln for ln in b.split("\n") if ln.strip()]
            m = LAYOUT_LINE.match(lines[0])
            head = m.group(5).strip()
            am = ANON.search(head)
            if am:
                nm = self.anon_name.get(f"{am.group(1)}:{am.group(2)}")
                tag = head.split()[0]
                if nm is None:  # typedef'd anonymous: find the typedef owning it
                    nm = next((k.split(" ", 1)[1] for k, v in self.recdef.items()
                               if self.lc(self.node[v]["loc"]["offset"]) == f"{am.group(1)}:{am.group(2)}"),
                              None)
                    if nm is None:
                        continue
                name = f"{tag} {nm}"
            else:
                name = head
            fields, body = {}, []
            sm = re.search(r"\[sizeof=(\d+),(?: dsize=\d+,)? align=(\d+)", lines[-1])
            for ln in lines[1:-1]:
                fm = LAYOUT_LINE.match(ln)
                if not fm:
                    continue
                off, bs, be, ind, rest = fm.groups()
                lvl = (len(ind) - 1) // 2
                rest = ANON.sub(lambda m: self.anon_name.get(f"{m.group(1)}:{m.group(2)}") or "{anon:?}", rest)
                body.append(f"{lvl}|{off}|{bs}-{be}|{self.canon(rest)}")
                if lvl == 1:
                    fname = rest.split()[-1] if " " in rest else ""
                    ftype = rest[:len(rest) - len(fname)].strip() if fname else rest
                    fields[fname] = (int(off) * 8 + (int(bs) if bs else 0),
                                     (int(be) - int(bs) + 1) if bs else None, self.canon(ftype))
            self.layout[name] = (body, int(sm.group(1)), int(sm.group(2)), fields)

    def rec_name_of_decl(self, r):
        if r.get("name"):
            return f"{r['tagUsed']} {r['name']}"
        for k, v in self.recdef.items():
            if v == r["id"]:
                return k
        return None

    def whole(self, recname, why):
        """Whole-record layout of a resolved record name ('struct X')."""
        lay = self.layout.get(recname)
        if lay is None:
            rid = self.recdef.get(recname)
            if rid is None:  # incomplete type used by value? keep the name only
                self.items.append(("Lmissing", recname))
                return
            self.fallbacks["layout_missing"] += 1
            self.items.append(("Ldecl", recname, self.text(self.node[rid])))
            return
        self.items.append(("L", why, recname, lay[1], lay[2], "\n".join(lay[0])))

    def records_in(self, ctype, by_value_only=True):
        """Record names mentioned in a resolved type string; by_value_only skips
        those directly followed by '*' (pointers)."""
        ctype = plain(ctype)
        out = []
        for m in RECNAME.finditer(ctype):
            rest = ctype[m.end():].lstrip()
            if by_value_only and rest.startswith("*"):
                continue
            out.append(f"{m.group(1)} {m.group(2)}")
        return out

    # ------------------------------------------------------------ walk
    def key(self):
        units = [n for nm, ds in self.funcs.items() for n in ds
                 if any(c.get("kind") == "CompoundStmt" for c in n.get("inner", []))]
        self.fnrefs = {n["name"] for n in units}
        self.varrefs, self.enumrefs = set(), set()
        for n in units:
            self.items.append(("BODY", n["name"], self.text(n)))
            self.walk(n, ctx=None)
        done = set()
        while self.varrefs - done:  # globals' initializers may reference more
            v = sorted(self.varrefs - done)[0]
            done.add(v)
            for d in self.gvars.get(v, []):
                ct = self.canon(d.get("type"))
                attrs = [self.clean(c) for c in d.get("inner", []) if c.get("kind", "").endswith("Attr")]
                self.items.append(("G", v, ct, d.get("storageClass"), d.get("tls"), json.dumps(attrs)))
                if "const" in ct.split() or d.get("init") or (
                        d.get("storageClass") not in ("extern",)):
                    self.items.append(("Gdecl", v, self.text(d)))
                    for c in d.get("inner", []):
                        if not c.get("kind", "").endswith("Attr"):
                            self.walk(c, ctx=None)
        for f in sorted(self.fnrefs):
            for d in self.funcs.get(f, []):
                attrs = [self.clean(c) for c in d.get("inner", []) if c.get("kind", "").endswith("Attr")]
                self.items.append(("F", f, self.canon(d.get("type")), d.get("storageClass"),
                                   d.get("inline"), d.get("variadic"), json.dumps(attrs)))
        self.probe()
        self.items.append(("CC", PIN))
        return H(json.dumps(self.items))

    def probe(self):
        """sizeof, _Alignof and __alignof__ of every type the key mentions, as
        clang evaluates them (so typedef attributes such as aligned(64) and
        packed enums count): a syntax-only run of the unit plus one
        `typedef char __e16p_i[sizeof(T)][_Alignof(T)][__alignof__(T)];` per type.
        Probes clang rejects (unspellable or incomplete types) are dropped and
        counted; anonymous records are covered by their parent's layout."""
        types = sorted(t for t in self.used_types
                       if t and not re.search(r"unnamed|anonymous|\{anon", t))
        self.fallbacks["probe_unspellable_anon"] += len(self.used_types) - len(types)
        def spell(t):  # clang prints a typedef'd anonymous tag as 'enum OpCode': spell it 'OpCode'
            m = re.fullmatch(r"((?:const |volatile )*)(?:struct|union|enum) (\w+)", t)
            if m and m.group(2) in self.typedef_owned and not any(
                    n.get("name") == m.group(2) and n.get("kind") in ("RecordDecl", "EnumDecl")
                    for n in self.node.values()):
                return m.group(1) + m.group(2)
            return t
        live = {i: spell(t) for i, t in enumerate(types)}
        base = self.src.count("\n") + (0 if self.src.endswith("\n") else 1)
        for _ in range(4):
            order = sorted(live)
            text = self.src + ("" if self.src.endswith("\n") else "\n") + "".join(
                f"typedef char __e16p_{i}[sizeof({live[i]})][_Alignof({live[i]})][__alignof__({live[i]})];\n"
                for i in order)
            r = subprocess.run([CCV] + FLAGS + ["-fsyntax-only", "-ferror-limit=0", "-x", "c", "-",
                                                "-Xclang", "-ast-dump", "-Xclang", "-ast-dump-filter=__e16p_"],
                               input=text, capture_output=True, text=True)
            bad = {int(m) - base - 1 for m in re.findall(r"<stdin>:(\d+):\d+: error", r.stderr)}
            bad = {order[j] for j in bad if 0 <= j < len(order)}
            if r.returncode == 0 or not bad:
                break
            self.fallbacks["probe_rejected"] += len(bad)
            for i in bad:
                live.pop(i)
        got = {int(i): (a, b, c) for i, a, b, c in
               re.findall(r"__e16p_(\d+) 'char\[(\d+)\]\[(\d+)\]\[(\d+)\]'", r.stdout)}
        for i in sorted(live, key=lambda i: live[i]):
            if i not in got:
                self.fallbacks["probe_missing"] += 1
                continue
            self.items.append(("TY", self.canon(live[i]), got[i]))

    def clean(self, n):
        return {k: (self.clean(v) if isinstance(v, dict) else
                    [self.clean(x) for x in v] if k == "inner" else v)
                for k, v in n.items() if k not in ("id", "loc", "range")}

    def is_ptr(self, ct):
        s = plain(ct)
        for q in (" const", " volatile", " restrict"):
            while s.endswith(q):
                s = s[:-len(q)]
        return s.endswith("*"), s[:-1].strip()

    def walk(self, n, ctx):
        if not n:  # absent child (e.g. a for-loop without init)
            self.items.append(("null",))
            return
        k = n.get("kind", "")
        if TYPE_NODE.match(k) or k.endswith("Attr"):
            return
        if k not in KNOWN:
            self.fallbacks["unknown:" + k] += 1
            ct = self.canon(n.get("type")) or ""
            self.items.append(("X", k, self.text(n)))
            for r in self.records_in(ct, False):
                self.whole(r, "fallback")
        ct = self.canon(n.get("type"))
        ref = n.get("referencedDecl") or {}
        self.items.append(("N", k, n.get("opcode"), n.get("castKind"), n.get("value"), n.get("name"),
                           n.get("valueCategory"), n.get("isArrow"), n.get("storageClass"), ct,
                           self.canon(n.get("argType")), self.canon(n.get("computeLHSType")),
                           self.canon(n.get("computeResultType")), ref.get("kind"), ref.get("name")))
        if ct:
            # record by value
            bare = re.sub(r"(^|\s)(const|volatile)(?=\s|$)", " ", plain(ct)).strip()
            if RECNAME.fullmatch(bare) and ctx != "noval":
                self.whole(bare, "value")
            if ct.startswith("enum ") or " enum " in ct:
                for m in re.finditer(r"enum\s+(\{anon:[^}]*\}|[A-Za-z_]\w*)", ct):
                    self.items.append(("ET", m.group(1), self.enum_ut(m.group(1))))
        if k == "MemberExpr":
            self.member(n)
        elif k == "DeclRefExpr":
            rk, rn, rid = ref.get("kind"), ref.get("name"), ref.get("id")
            if rk == "FunctionDecl":
                self.fnrefs.add(rn)
            elif rk == "VarDecl" and rn in self.gvars and any(d["id"] == rid for d in self.gvars[rn]):
                self.varrefs.add(rn)
            elif rk == "EnumConstantDecl":
                v = self.enumval.get(rid)
                if v is None:
                    self.fallbacks["enum_value"] += 1
                    e = self.enum_of.get(rid)
                    self.items.append(("Edecl", rn, self.text(e) if e else None))
                else:
                    self.items.append(("E", rn, v))
        elif k == "UnaryExprOrTypeTraitExpr":
            at = self.canon(n.get("argType")) if n.get("argType") else \
                self.canon(n["inner"][0].get("type")) if n.get("inner") else ""
            szs = [(r, self.layout[r][1], self.layout[r][2]) if r in self.layout else (r, None, None)
                   for r in self.records_in(at)]
            if any(s[1] is None for s in szs):
                self.fallbacks["sizeof_record_missing"] += 1
                for r, _, _ in szs:
                    if r in self.recdef:
                        self.items.append(("Ldecl", r, self.text(self.node[self.recdef[r]])))
            self.items.append(("SZ", n.get("name"), at, json.dumps(szs)))
        elif k == "OffsetOfExpr":
            pass  # unreachable: not in KNOWN (fallback above)
        # pointer arithmetic / subscripting
        ptr_ops = (k in ("BinaryOperator", "CompoundAssignOperator") and n.get("opcode") in ("+", "-", "+=", "-=")) \
            or (k == "UnaryOperator" and n.get("opcode") in ("++", "--")) or k == "ArraySubscriptExpr"
        if ptr_ops:
            for c in n.get("inner", []):
                cc = self.canon(c.get("type")) or ""
                p, pointee = self.is_ptr(cc)
                if p:
                    for r in self.records_in(pointee, False):
                        self.whole(r, "ptrarith")
        if k == "OffsetOfExpr" or (k not in KNOWN and "offsetof" in (self.text(n) or "")):
            m = re.match(r"__builtin_offsetof\0\(\0(.*?)\0,", self.text(n) or "")
            if m:
                tn = self.canon(" ".join(m.group(1).split("\0")))
                for r in self.records_in(tn, False):
                    self.whole(r, "offsetof")
        for i, c in enumerate(n.get("inner", [])):
            cctx = None
            if (k == "MemberExpr" and not n.get("isArrow")) or (k == "UnaryOperator" and n.get("opcode") == "&"):
                cctx = "noval"
            elif k == "ParenExpr" and ctx == "noval":
                cctx = "noval"
            self.walk(c, cctx)

    def enum_ut(self, ename):
        """Underlying-type determinants of an enum: sign and bit width of its
        constants' range. Typedef'd anonymous enums resolve through the typedef;
        an unresolvable enum falls back to all enums' text (counted)."""
        e = next((n for n in self.node.values() if n.get("kind") == "EnumDecl" and n.get("name") == ename), None)
        if e is None and ename in self.typedef_owned:
            e = self.node.get(self.typedef_owned[ename])
        if e is not None:
            vs = [self.enumval.get(c["id"]) for c in e.get("inner", []) if c.get("kind") == "EnumConstantDecl"]
            if vs and all(v is not None for v in vs):
                lo, hi = min(vs), max(vs)
                return f"{'u' if lo >= 0 else 's'}{max(hi.bit_length(), (-lo).bit_length() if lo < 0 else 0)}"
        self.fallbacks["enum_ut"] += 1
        return self.text(e) if e is not None else H(json.dumps(sorted(
            self.text(n) or "" for n in self.node.values() if n.get("kind") == "EnumDecl")))

    def member(self, n):
        fid = n.get("referencedMemberDecl")
        f = self.node.get(fid)
        r = self.parent.get(fid)
        rname = r and self.rec_name_of_decl(r)
        lay = self.layout.get(rname)
        if f is None or lay is None or n.get("name") not in lay[3]:
            self.fallbacks["member_unresolved"] += 1
            if r is not None:
                self.items.append(("Mdecl", rname, n.get("name"), self.text(r)))
            return
        off, bits, ftype = lay[3][n["name"]]
        self.items.append(("M", rname, n["name"], off, bits, ftype, self.canon(f.get("type")), lay[1], lay[2]))
        if bits is not None or f.get("isBitfield"):
            # the load/store width depends on the bitfield access unit (neighbouring
            # bitfields), not just this field: key the whole layout (conservative)
            self.fallbacks["bitfield_whole_layout"] += 1
            self.whole(rname, "bitfield")


def project(src):
    t0 = time.process_time()
    c0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    p = Proj(src)
    k = p.key()
    c1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = time.process_time() - t0 + (c1.ru_utime - c0.ru_utime) + (c1.ru_stime - c0.ru_stime)
    return k, dict(p.fallbacks), cpu, p


# ---------------------------------------------------------------- mutation tests
SYN = r"""
typedef int T;
struct S { int a; int x; char c; T t; };
struct Big { long a; long b; };
void g(void);
int h(int) __attribute__((const));
enum E { E0, E1, E2 };
typedef struct Arr { char buf[64]; } ArrT;
typedef struct Elt { long v[3]; } *EltP;
int use(ArrT *);
long k(EltP a, EltP b) { ArrT local; use(&local); return a - b; }
int f(struct S *p, struct Big *q) { T loc = p->x + p->t; if (!p) g(); return loc + h(E2) + (int)(q + 1)->a; }
"""
MUTS = [  # (name, from, to, expect key change)
    ("used field offset (swap a,x)", "int a; int x;", "int x; int a;", True),
    ("typedef width (T int->long)", "typedef int T;", "typedef long T;", True),
    ("typedef signedness (T int->unsigned)", "typedef int T;", "typedef unsigned T;", True),
    ("callee attribute (+noreturn on g)", "void g(void);", "void g(void) __attribute__((noreturn));", True),
    ("callee attribute (const->pure on h)", "__attribute__((const))", "__attribute__((pure))", True),
    ("enum constant value used (E2)", "E0, E1, E2", "E0, E1, E5, E2", True),
    ("pointer-arith pointee layout (Big)", "long a; long b; }", "long a; long b; long c; }", True),
    ("typedef'd record local by value (ArrT size)", "char buf[64];", "char buf[128];", True),
    ("pointer subtraction via typedef'd pointer (Elt size)", "long v[3];", "long v[2];", True),
    ("control: unused field type, same size (a int->unsigned)", "int a; int x;", "unsigned a; int x;", False),
    ("control: unused callee attribute elsewhere", "enum E {", "void zz(void) __attribute__((noreturn));\nenum E {", False),
]


# adversarial edits found by the verifier: typedef attributes, packed enums,
# bitfield access unit
SYN2 = [  # (name, source, from, to); each must change the key
    ("bitfield store access unit (trailing c 4->20 bits: movw->movl)",
     "struct S { unsigned a:3; unsigned b:5; unsigned c:4; };\nvoid f(struct S *p) { p->b = 7; }\n", "c:4", "c:20"),
    ("packed enum, sizeof(E)", "typedef enum E { A, B } E;\nunsigned long f(void) { return sizeof(E); }",
     "typedef enum E {", "typedef enum __attribute__((packed)) E {"),
    ("typedef aligned(32), local whose address escapes", "typedef int T;\nvoid g(T*);\nvoid f(void) { T x; g(&x); }",
     "typedef int T;", "typedef int T __attribute__((aligned(32)));"),
]


def mutate(e15out):
    ok = True
    for name, src, a, b in SYN2:
        k0, k1 = project(src)[0], project(src.replace(a, b, 1))[0]
        ok &= k0 != k1
        print(f"  {'PASS' if k0 != k1 else 'FAIL'} synthetic: {name}: key {'changed' if k0 != k1 else 'same'}")
    base = project(SYN)[0]
    for name, a, b, want in MUTS:
        assert a in SYN, a
        k = project(SYN.replace(a, b, 1))[0]
        res = (k != base) == want
        ok &= res
        print(f"  {'PASS' if res else 'FAIL'} synthetic: {name}: key {'changed' if k != base else 'same'}")
    # real Lua slice: lua_gc (global_State fields, lu_byte, luaE_setdebt callee)
    U = json.load(open(os.path.join(e15out, "units.json")))
    e15._init(None, e15out)
    h = sorted(h for h, v in U.items() if v and "lua_gc" in v)[-1]
    src = gzip.open(e15.slice_path(U[h]["lua_gc"][0]), "rt").read()
    base = project(src)[0]
    gs = re.search(r"typedef struct global_State \{(.*?)\} global_State;", src, re.S)
    fields = [x.strip() for x in gs.group(1).split(";") if x.strip()]
    used = [f for f in fields if re.search(r"\bg->" + re.escape(f.split()[-1].lstrip("*").split("[")[0]) + r"\b", src)]
    def fname(f):
        return f.split()[-1].lstrip("*").split("[")[0]

    def ftype(f):
        return f[:len(f) - len(f.split()[-1])].strip()

    def swap(i):
        a, b = fields[i] + ";", fields[i + 1] + ";"
        return gs.group(0).replace(a, "\0").replace(b, a).replace("\0", b)
    # a used field next to an unused one of the same declared type: swapping
    # moves the used field's offset and keeps the record's size
    iu = next(i for i in range(len(fields) - 1) if ftype(fields[i]) == ftype(fields[i + 1])
              and (fields[i] in used) != (fields[i + 1] in used))
    # two adjacent unused fields of the same type: control, no used offset moves
    ic = next(i for i in range(len(fields) - 1) if ftype(fields[i]) == ftype(fields[i + 1])
              and fields[i] not in used and fields[i + 1] not in used)
    a = (fname(fields[iu]), fname(fields[iu + 1]))
    real = [
        (f"real lua_gc: swap global_State.{a[0]}/{a[1]} (used field's offset moves, size same)",
         gs.group(0), swap(iu), True),
        (f"real lua_gc control: swap unused global_State.{fname(fields[ic])}/{fname(fields[ic + 1])}",
         gs.group(0), swap(ic), False),
        ("real lua_gc: lu_byte unsigned char->unsigned short", "typedef unsigned char lu_byte;",
         "typedef unsigned short lu_byte;", True),
        ("real lua_gc: +noreturn on callee luaE_setdebt", "extern void luaE_setdebt",
         "extern void __attribute__((noreturn)) luaE_setdebt", True),
    ]
    print(f"  (lua_gc slice from TU {h[:10]}; global_State fields used: {len(used)}/{len(fields)})")
    for name, x, y, want in real:
        if x not in src or x == y:
            print(f"  SKIP {name}: pattern not found")
            ok = False
            continue
        k = project(src.replace(x, y, 1))[0]
        res = (k != base) == want
        ok &= res
        print(f"  {'PASS' if res else 'FAIL'} {name}: key {'changed' if k != base else 'same'}")
    # real Lua slice: luaL_gsub (local luaL_Buffer b); aligned(64) on the typedef only
    h = sorted(h for h, v in U.items() if v and "luaL_gsub" in v)[-1]
    src = gzip.open(e15.slice_path(U[h]["luaL_gsub"][0]), "rt").read()
    x, y = "} luaL_Buffer;", "} luaL_Buffer __attribute__((aligned(64)));"
    if x not in src:
        print("  SKIP real luaL_gsub: pattern not found")
        ok = False
    else:
        k0, k1 = project(src)[0], project(src.replace(x, y, 1))[0]
        ok &= k0 != k1
        print(f"  {'PASS' if k0 != k1 else 'FAIL'} real luaL_gsub: aligned(64) on the luaL_Buffer typedef "
              f"(record layout unchanged): key {'changed' if k0 != k1 else 'same'}")
    print("mutation tests:", "all pass" if ok else "FAILURES")
    return ok


# ---------------------------------------------------------------- keys over history
def _key(skey):
    src = gzip.open(e15.slice_path(skey), "rt").read()
    try:
        k, fb, cpu, _ = project(src)
    except Exception as ex:  # noqa: BLE001
        return skey, None, {"error": str(ex)[:200]}, 0.0
    return skey, k, fb, cpu


def slice_changed(work, U, O):
    """Slice-changed function transitions with both outputs compiled, in a seeded order."""
    rows = [(f, i, ha, hb, t) for f, i, ha, hb, t in e15.transitions(work, U)
            if ha != hb and U[ha][t][0] != U[hb][t][0]]
    random.seed(16)
    random.shuffle(rows)
    return rows


def keys(work, e15out, out, n, jobs):
    U = json.load(open(os.path.join(e15out, "units.json")))
    O = json.load(open(os.path.join(e15out, "outputs.json")))
    rows = slice_changed(work, U, O)
    if n != "all":
        rows = rows[:int(n)]
    need = sorted({U[r[2]][r[4]][0] for r in rows} | {U[r[3]][r[4]][0] for r in rows})
    of = os.path.join(out, "proj.json")
    P = json.load(open(of)) if os.path.exists(of) else {}
    todo = [s for s in need if s not in P]
    print(f"transitions {len(rows)}; slices {len(need)}; to key {len(todo)}", flush=True)
    t0 = time.time()
    with ProcessPoolExecutor(jobs, initializer=e15._init, initargs=(work, e15out)) as ex:
        for j, (s, k, fb, cpu) in enumerate(ex.map(_key, todo, chunksize=8)):
            P[s] = [k, fb, cpu]
            if j % 5000 == 4999:
                json.dump(P, open(of, "w"))
                print(f"  keyed {j + 1} ({time.time() - t0:.0f}s)", flush=True)
    json.dump(P, open(of, "w"))
    cpus = [P[s][2] for s in need if P[s][0]]
    print(f"wall {time.time() - t0:.0f}s for {len(todo)} slices; extraction CPU per slice: "
          f"mean {sum(cpus) / len(cpus) * 1000:.0f} ms, median {sorted(cpus)[len(cpus) // 2] * 1000:.0f} ms")


# ---------------------------------------------------------------- analyze
def analyze(work, e15out, out, n):
    U = json.load(open(os.path.join(e15out, "units.json")))
    O = json.load(open(os.path.join(e15out, "outputs.json")))
    P = json.load(open(os.path.join(out, "proj.json")))
    rows = slice_changed(work, U, O)
    if n != "all":
        rows = rows[:int(n)]
    c = collections.Counter()
    hit_commits, false_cases = collections.Counter(), []
    for f, i, ha, hb, t in rows:
        sa, sb = U[ha][t][0], U[hb][t][0]
        oa, ob = O.get(sa), O.get(sb)
        pa, pb = P.get(sa, [None])[0], P.get(sb, [None])[0]
        c["slice_changed"] += 1
        if oa is None or ob is None:
            c["excl_compile_fail"] += 1
            continue
        if pa is None or pb is None:
            c["excl_extract_fail"] += 1
            continue
        c["counted"] += 1
        out_ch, proj_ch = oa != ob, pa != pb
        c["output_changed"] += out_ch
        c["proj_changed"] += proj_ch
        if not proj_ch:
            c["informative_hits"] += 1
            hit_commits[i] += 1
            if out_ch:
                c["false_hits"] += 1
                false_cases.append((f, i, ha, hb, t))
        elif not out_ch:
            c["proj_changed_output_same"] += 1
    # all function-transitions (slice unchanged => projection and output unchanged)
    nall = sum(1 for _ in e15.transitions(work, U)) if n == "all" else None
    fb = collections.Counter()
    nfb = 0
    used = {U[r[2]][r[4]][0] for r in rows} | {U[r[3]][r[4]][0] for r in rows}
    cpus = []
    for s in used:
        if s in P and P[s][0]:
            cpus.append(P[s][2])
            if any(not k.startswith("probe_") for k in P[s][1]):  # probe rejects are not fallbacks
                nfb += 1
            for k, v in P[s][1].items():
                fb[k] += v
    print(f"slice-changed transitions: {c['slice_changed']}; excluded: compile fail {c['excl_compile_fail']}, "
          f"extraction fail {c['excl_extract_fail']}; counted {c['counted']}")
    print(f"  output changed (E15) {c['output_changed']}; projection changed {c['proj_changed']}; "
          f"projection changed, output same {c['proj_changed_output_same']}")
    ratio = c["proj_changed"] / c["output_changed"]
    print(f"  ratio projection/output change rate: {ratio:.3f}  (slice/output: {c['counted'] / c['output_changed']:.3f})")
    if nall:
        print(f"  per all {nall} function-transitions: slice {c['slice_changed'] / nall:.5f}, output "
              f"{c['output_changed'] / nall:.5f}, projection {c['proj_changed'] / nall:.5f}")
    ih, fh, C = c["informative_hits"], c["false_hits"], len(hit_commits)
    print(f"  informative hits (slice changed, projection equal): {ih} in {C} distinct commits")
    print(f"  false hits (informative hit, whole-unit output changed): {fh} "
          f"in {len({x[1] for x in false_cases})} commits")
    if fh == 0:
        print(f"  95% upper bound on false-hit rate: per-hit (rule of three) {3 / ih:.5%}; "
              f"commit-clustered (hits in a commit fully correlated, n_eff = commits) {3 / C:.4%}")
    else:
        lo, hi = cluster_ci(rows, U, O, P, false_cases)
        print(f"  false-hit rate {fh / ih:.4%}; commit-clustered bootstrap 95% CI [{lo:.4%}, {hi:.4%}]")
    print(f"  slices keyed: {len(used)}; with >=1 fallback: {nfb} ({nfb / max(1, len(used)):.2%}); events: {dict(fb)}")
    if cpus:
        cs = sorted(cpus)
        print(f"  extraction CPU per slice: mean {sum(cs) / len(cs) * 1000:.0f} ms, median "
              f"{cs[len(cs) // 2] * 1000:.0f} ms, p90 {cs[int(len(cs) * .9)] * 1000:.0f} ms, total {sum(cs):.0f} s")
    json.dump(false_cases, open(os.path.join(out, f"false_hits_{n}.json"), "w"))
    for x in false_cases[:20]:
        print("  FALSE HIT", x)


def explain(work, e15out, out, n=400):
    """Over-invalidation drivers: for a seeded sample of transitions where the
    projection changed but the whole-unit output did not, recompute both item
    lists and tally which item kinds (and records) differ."""
    U = json.load(open(os.path.join(e15out, "units.json")))
    O = json.load(open(os.path.join(e15out, "outputs.json")))
    P = json.load(open(os.path.join(out, "proj.json")))
    e15._init(work, e15out)
    rows = [r for r in slice_changed(work, U, O)
            if None not in (O.get(U[r[2]][r[4]][0]), O.get(U[r[3]][r[4]][0]))
            and P.get(U[r[2]][r[4]][0], [None])[0] and P.get(U[r[3]][r[4]][0], [None])[0]
            and O[U[r[2]][r[4]][0]] == O[U[r[3]][r[4]][0]]
            and P[U[r[2]][r[4]][0]][0] != P[U[r[3]][r[4]][0]][0]]
    sample = rows[:n]
    kinds, recs, only = collections.Counter(), collections.Counter(), collections.Counter()

    def items(s):
        return project(gzip.open(e15.slice_path(s), "rt").read())[3].items

    def label(it):
        if it[0] in ("M", "Mdecl"):
            return "M (field access)", it[1]
        if it[0] == "L":
            return f"L (whole layout, {it[1]})", it[2]
        if it[0] in ("BODY", "N"):
            return "BODY/N (bodies, node types)", None
        return it[0], it[1] if len(it) > 1 else None
    for f, i, ha, hb, t in sample:
        a, b = collections.Counter(map(repr, items(U[ha][t][0]))), collections.Counter(map(repr, items(U[hb][t][0])))
        diff = (a - b) + (b - a)
        ks, rs = set(), set()
        for r in diff:
            k, rec = label(eval(r))
            ks.add(k)
            if rec:
                rs.add((k, rec))
        for x in rs:
            recs[x] += 1
        for k in ks:
            kinds[k] += 1
        if len(ks) == 1:
            only[next(iter(ks))] += 1
    print(f"projection changed, output same: {len(rows)} transitions; sampled {len(sample)}")
    print("item kinds differing (transitions touching each; 'alone' = the only differing kind):")
    for k, v in kinds.most_common():
        print(f"  {v:5d}  alone {only[k]:5d}  {k}")
    print("top (kind, record/name) pairs (transitions touching each):")
    for (k, r), v in recs.most_common(15):
        print(f"  {v:6d}  {k}: {r}")


def cluster_ci(rows, U, O, P, false_cases):
    per = collections.defaultdict(lambda: [0, 0])
    fs = {(x[0], x[1], x[4]) for x in false_cases}
    for f, i, ha, hb, t in rows:
        sa, sb = U[ha][t][0], U[hb][t][0]
        if None in (O.get(sa), O.get(sb), P.get(sa, [None])[0], P.get(sb, [None])[0]):
            continue
        if P[sa][0] == P[sb][0]:
            per[i][0] += 1
            per[i][1] += (f, i, t) in fs
    cl = list(per.values())
    random.seed(1)
    est = []
    for _ in range(2000):
        s = [cl[random.randrange(len(cl))] for _ in cl]
        est.append(sum(b for _, b in s) / max(1, sum(a for a, _ in s)))
    est.sort()
    return est[50], est[1949]


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "key":
        k, fb, cpu, p = project(open(sys.argv[2]).read())
        for it in p.items:
            print(str(it)[:300])
        print(k, fb, f"{cpu * 1000:.0f} ms")
    elif cmd == "mutate":
        sys.exit(0 if mutate(sys.argv[2]) else 1)
    elif cmd == "keys":
        os.makedirs(sys.argv[4], exist_ok=True)
        keys(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5],
             int(sys.argv[6]) if len(sys.argv) > 6 else os.cpu_count())
    elif cmd == "explain":
        explain(sys.argv[2], sys.argv[3], sys.argv[4])
    elif cmd == "analyze":
        analyze(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
    else:
        sys.exit(__doc__)
