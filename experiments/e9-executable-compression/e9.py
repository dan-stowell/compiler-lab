"""E9 step 1: lower bound for an executable grammar form of chibicc stage2's .text.

usage: python3 e9.py ASM_DIR DENOMINATORS_TXT

Parses every ASM_DIR/*.s (chibicc output), keeps instructions in .text sections, splits
each into (skeleton, operand values), and runs Re-Pair within basic blocks in two variants:
  A: symbols are whole instructions (skeleton + operand values), operands in an
     instruction table;
  B: symbols are skeletons, operands in a separate fixed-width stream.
Labels are qualified by TU when TU-local, so every label and operand stays distinct.
"""
import collections, glob, heapq, math, os, re, sys

asm_dir, den_file = sys.argv[1], sys.argv[2]


def bits(n):  # ceil(log2 n), 0 for n <= 1
    return (n - 1).bit_length() if n > 1 else 0


# ---------------------------------------------------------------- parsing
INT = re.compile(r'^-?\d+$')
LABEL_DEF = re.compile(r'^([A-Za-z_.$][\w.$]*):$')


def split_ops(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def parse_tu(path):
    tu = os.path.basename(path)[:-2]
    lines = open(path).read().split('\n')
    defined, globl, funcs = set(), set(), set()
    for l in lines:
        s = l.strip()
        m = LABEL_DEF.match(s)
        if m:
            defined.add(m.group(1))
        elif s.startswith('.globl '):
            globl.add(s.split()[1])
        elif s.startswith('.comm '):
            defined.add(s.split()[1].rstrip(','))
        elif s.startswith('.type ') and s.endswith('@function'):
            funcs.add(s.split()[1].rstrip(','))

    def q(name):
        return f'{tu}:{name}' if name in defined and name not in globl else name

    items = []  # ('label', qname, is_func) or ('ins', skeleton, values, kinds)
    in_text = False
    for l in lines:
        s = l.strip()
        if not s:
            continue
        if s.startswith('.'):
            d = s.split()[0]
            if d == '.text':
                in_text = True; continue
            if d in ('.data', '.bss', '.section'):
                in_text = False; continue
            if not LABEL_DEF.match(s):
                continue  # other directives (.loc, .type, .globl, .align, ...)
        if not in_text:
            continue
        m = LABEL_DEF.match(s)
        if m:
            items.append(('label', q(m.group(1)), m.group(1) in funcs))
            continue
        parts = s.split(None, 1)
        mn, rest = parts[0], (parts[1] if len(parts) > 1 else '')
        skel_ops, vals, kinds = [], [], []
        for op in split_ops(rest):
            if op.startswith('%') or op.startswith('*%'):
                skel_ops.append(op)
            elif op.startswith('$'):
                v = op[1:]
                if INT.match(v):
                    skel_ops.append('$I'); vals.append(int(v)); kinds.append('imm')
                else:
                    skel_ops.append('$S'); vals.append(q(v)); kinds.append('sym')
            elif '(' in op:
                pre, base = op.split('(', 1)
                if pre == '':
                    skel_ops.append(op)
                elif INT.match(pre):
                    skel_ops.append('D(' + base); vals.append(int(pre)); kinds.append('disp')
                else:
                    sym, _, reloc = pre.partition('@')
                    skel_ops.append('S' + ('@' + reloc if reloc else '') + '(' + base)
                    vals.append(q(sym)); kinds.append('sym')
            elif mn.startswith('j') and re.match(r'^[A-Za-z_.$][\w.$]*$', op):
                skel_ops.append('L'); vals.append(q(op)); kinds.append('label')
            else:
                skel_ops.append(op)  # e.g. 'stosb' after 'rep'
        items.append(('ins', mn + ' ' + ','.join(skel_ops), tuple(vals), tuple(kinds)))
    return items


items = []
for p in sorted(glob.glob(os.path.join(asm_dir, '*.s'))):
    items += parse_tu(p)

# ---------------------------------------------------------------- blocks
ENDS = re.compile(r'^(j\w+|call|ret)\b')
blocks, cur = [], []          # each block: list of instruction indices
instrs = []                   # (skel, vals, kinds)
label_defs, func_labels = [], set()
for it in items:
    if it[0] == 'label':
        if cur:
            blocks.append(cur); cur = []
        label_defs.append(it[1])
        if it[2]:
            func_labels.add(it[1])
        continue
    instrs.append(it[1:])
    cur.append(len(instrs) - 1)
    if ENDS.match(it[1]):
        blocks.append(cur); cur = []
if cur:
    blocks.append(cur)

N = len(instrs)
assert sum(map(len, blocks)) == N
assert len(set(label_defs)) == len(label_defs), 'label collision after TU qualification'
refs = collections.Counter()
for sk, vals, kinds in instrs:
    for v, k in zip(vals, kinds):
        if k in ('label', 'sym'):
            refs[v] += 1
text_labels = set(label_defs)
B_set = {l for l in label_defs if l in refs or l in func_labels}
B = len(B_set)
syms = {v for sk, vals, kinds in instrs for v, k in zip(vals, kinds) if k == 'sym'}
ext_syms = syms - text_labels       # data, string literals, libc functions
unref_labels = len(label_defs) - B

# ---------------------------------------------------------------- operand slots
skels = sorted({i[0] for i in instrs})
S = len(skels)
slot_vals = collections.defaultdict(set)
slot_n = collections.Counter()
slot_kind = {}
for sk, vals, kinds in instrs:
    for j, (v, k) in enumerate(zip(vals, kinds)):
        slot_vals[(sk, j)].add(v); slot_n[(sk, j)] += 1; slot_kind[(sk, j)] = k
width = {s: bits(len(v)) for s, v in slot_vals.items()}


def raw_width(s):
    k = slot_kind[s]
    if k == 'label':
        return bits(B)
    if k == 'sym':
        return bits(len(syms))
    vs = slot_vals[s]
    return bits(max(vs) - min(vs) + 1)


# per slot: cheapest of (index + dictionary) and (raw values, no dictionary)
def slot_cost_with_dict(s, n):
    return min(n * width[s] + len(slot_vals[s]) * raw_width(s), n * raw_width(s))


tokens = {}
for sk, vals, kinds in instrs:
    tokens.setdefault((sk, vals), len(tokens))
T_A = len(tokens)
tok_per_slot = collections.Counter()
for (sk, vals) in tokens:
    for j in range(len(vals)):
        tok_per_slot[(sk, j)] += 1

# ---------------------------------------------------------------- Re-Pair


def repair(seqs, nterm):
    """Re-Pair within blocks. Returns list of (rules, seqlen) after each rule, rules, final."""
    sym, nxt, prv = [], [], []
    for b in seqs:
        base = len(sym)
        for i, x in enumerate(b):
            sym.append(x)
            prv.append(base + i - 1 if i > 0 else -1)
            nxt.append(base + i + 1 if i + 1 < len(b) else -1)
    alive = [True] * len(sym)
    occ = collections.defaultdict(set)
    for i in range(len(sym)):
        if nxt[i] != -1:
            occ[(sym[i], sym[nxt[i]])].add(i)
    heap = [(-len(v), p) for p, v in occ.items()]
    heapq.heapify(heap)
    length = len(sym)
    hist = [(0, length)]
    rules = []
    nsym = nterm
    while heap:
        c, p = heapq.heappop(heap)
        cnt = len(occ.get(p, ()))
        if cnt != -c:
            if cnt >= 2:
                heapq.heappush(heap, (-cnt, p))
            continue
        if cnt < 2:
            break
        a, b = p
        X = nsym; nsym += 1
        touched = set()
        done = 0
        for i in sorted(occ.pop(p)):
            if not alive[i] or sym[i] != a:
                continue
            j = nxt[i]
            if j == -1 or sym[j] != b:
                continue
            h, k = prv[i], nxt[j]
            if h != -1:
                occ[(sym[h], a)].discard(h); touched.add((sym[h], a))
            if k != -1:
                occ[(b, sym[k])].discard(j); touched.add((b, sym[k]))
            sym[i] = X; alive[j] = False
            nxt[i] = k
            if k != -1:
                prv[k] = i
            if h != -1:
                occ[(sym[h], X)].add(h); touched.add((sym[h], X))
            if k != -1:
                occ[(X, sym[k])].add(i); touched.add((X, sym[k]))
            done += 1
        if done == 0:
            nsym -= 1
            continue
        rules.append((a, b))
        length -= done
        hist.append((len(rules), length))
        for t in touched:
            if t != p and len(occ.get(t, ())) >= 2:
                heapq.heappush(heap, (-len(occ[t]), t))
    # rebuild final blocks
    final = []
    for b0 in range(len(sym)):
        if alive[b0] and prv[b0] == -1:
            out, i = [], b0
            while i != -1:
                out.append(sym[i]); i = nxt[i]
            final.append(out)
    return hist, rules, final


def expand(final, rules, nterm):
    memo = {}

    def ex(x):
        if x < nterm:
            return [x]
        if x not in memo:
            a, b = rules[x - nterm]
            memo[x] = ex(a) + ex(b)
        return memo[x]
    return [[y for x in blk for y in ex(x)] for blk in final]


def check(seqs, nterm):
    hist, rules, final = repair(seqs, nterm)
    assert expand(final, rules, nterm) == seqs, 'Re-Pair round trip failed'
    return hist


skel_id = {s: i for i, s in enumerate(skels)}
seqA = [[tokens[(instrs[i][0], instrs[i][1])] for i in b] for b in blocks]
seqB = [[skel_id[instrs[i][0]] for i in b] for b in blocks]
histA = check(seqA, T_A)
histB = check(seqB, S)

# ---------------------------------------------------------------- costs (bits)
SKEL_DESC_BITS = 32       # estimate: opcode template + slot layout per skeleton
SYMADDR_BITS = 32         # estimate: address of each external/data symbol
operand_stream = sum(slot_n[s] * width[s] for s in slot_n)                     # B: per occurrence
operand_stream_dict = sum(slot_cost_with_dict(s, slot_n[s]) for s in slot_n)
instr_table_ops = sum(tok_per_slot[s] * width[s] for s in tok_per_slot)       # A: per distinct instr
instr_table_ops_dict = sum(slot_cost_with_dict(s, tok_per_slot[s]) for s in tok_per_slot)
skel_table = S * SKEL_DESC_BITS
symaddr_table = len(ext_syms) * SYMADDR_BITS


def variant(hist, nterm, fixed, opoff_bits):
    best = None
    for R, L in hist:
        w = bits(nterm + R)
        c = {'top-level symbols': L * w, 'rule bodies': 2 * R * w,
             'label table': B * (bits(L) + opoff_bits)}
        tot = sum(c.values()) + sum(fixed.values())
        if best is None or tot < best[0]:
            best = (tot, R, L, w, c)
    tot, R, L, w, c = best
    c.update(fixed)
    return {'total': tot, 'rules': R, 'len': L, 'w': w, 'parts': c}


A = variant(histA, T_A, {'operands (in instruction table)': instr_table_ops,
                         'instruction table: skeleton ids': T_A * bits(S),
                         'skeleton table (est. 32 b/skeleton)': skel_table}, 0)
Bv = variant(histB, S, {'operands (separate stream)': operand_stream,
                        'skeleton table (est. 32 b/skeleton)': skel_table},
             bits(operand_stream))

den = {}
for l in open(den_file):
    f = l.split()
    den[f[0]] = {k: v for k, v in (x.split('=') for x in f[1:])}
# Primary denominator (the plan's pinned ~32.5 KB): stage2's .text as the 9 .o files carry it
# (relocation fields zero). Alternative: the linked binary's .text (relocations resolved,
# plus ~264 B of crt code), which is larger and so more favourable to the bound.
D = int(den['stage2-objs.text']['xz9e'])
DL = int(den['stage2.text']['xz9e'])


def kb(b):
    return b / 8


print(f"instructions={N} blocks={len(blocks)} distinct_instrs={T_A} skeletons={S} "
      f"operand_slots={len(slot_n)} operands={sum(slot_n.values())}")
print(f"labels_in_text={len(label_defs)} label_table_B={B} (functions={len(func_labels)}, "
      f"unreferenced non-function labels={unref_labels}) symbols_referenced={len(syms)} "
      f"external_or_data_symbols={len(ext_syms)}")
print(f"Re-Pair round trip: OK for both variants")
for name, v, h in (('A whole-instruction', A, histA), ('B skeleton+operand stream', Bv, histB)):
    print(f"\n[{name}] chosen stop: rules={v['rules']} (of {h[-1][0]} run) top-level={v['len']} "
          f"symbol width={v['w']} bits")
    for k, x in v['parts'].items():
        print(f"  {k:40s} {kb(x):10.0f} B")
    print(f"  {'PAYLOAD BOUND':40s} {kb(v['total']):10.0f} B  ratio={kb(v['total'])/D:.3f} (vs linked {kb(v['total'])/DL:.3f})")
best = min((A, 'A'), (Bv, 'B'), key=lambda t: t[0]['total'])
bb = kb(best[0]['total'])
print(f"\ndenominator: xz -9e of stage2 .text (9 .o files, concatenated) = {D} B "
      f"(size {den['stage2-objs.text']['size']}, sha256 {den['stage2-objs.text']['sha256']}...)")
print(f"alt denominator: xz -9e of linked stage2 .text = {DL} B (size {den['stage2.text']['size']}, "
      f"sha256 {den['stage2.text']['sha256']}...)")
print(f"context: clang -Oz .text = {den['clang-oz.text']['size']} B, xz -9e = {den['clang-oz.text']['xz9e']} B")
print(f"smaller bound: variant {best[1]} = {bb:.0f} B, ratio = {bb/D:.3f} (vs linked: {bb/DL:.3f})")
extra = (instr_table_ops_dict - instr_table_ops) if best[1] == 'A' else (operand_stream_dict - operand_stream)
print(f"fuller bound (+ per-slot value dictionaries {kb(extra):.0f} B, + external symbol address "
      f"table {kb(symaddr_table):.0f} B est.): {bb + kb(extra + symaddr_table):.0f} B, "
      f"ratio = {(bb + kb(extra + symaddr_table))/D:.3f} (vs linked {(bb + kb(extra + symaddr_table))/DL:.3f})")
for hb in (16, 32):
    print(f"handler cost (estimate, {hb} B/handler x {S} skeletons) = {S*hb} B; payload+handlers "
          f"ratio = {(bb + S*hb)/D:.3f} (vs linked {(bb + S*hb)/DL:.3f})")
print("DECISION:", "bound > 1.25x denominator: representation loses, stop" if min(bb / D, bb / DL) > 1.25
      else "bound <= 1.25x denominator: not ruled out (full encoder not built)")
