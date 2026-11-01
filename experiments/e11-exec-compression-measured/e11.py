"""E11: measured executable compression (payload gate).

usage: python3 e11.py E9_DIR OUT_DIR

Execs E9_DIR/e9.py (as E9's verify/extra.py does) to get the parsed stage2 instructions,
basic blocks and skeletons, then writes an actual payload file OUT_DIR/payload.bin:
  - Re-Pair over skeleton ids within basic blocks (E9 variant B), symbols fixed width;
  - operands interleaved after each instruction's skeleton, in one bit stream;
  - jump operands hold the target's absolute bit address in the stream (width W, iterated
    to a fixed point); an entry table only for the functions;
  - every non-jump operand slot (skeleton, operand index) gets its own canonical Huffman
    code; tables are stored as the slot's sorted values plus one code length per value;
  - the skeleton table is serialized (mnemonic and operand-template strings + indices).
Payload size = len(payload.bin). The decoder below reads only the file's bytes; all
checks run against a decoder built from the file read back from disk.
"""
import collections, contextlib, hashlib, heapq, io, os, re, sys

e9_dir, out_dir = sys.argv[1], sys.argv[2]
sys.argv = ['e9.py', os.path.join(e9_dir, 'work/s2'), os.path.join(e9_dir, 'work/denominators.txt')]
g = {'__name__': 'e9'}
with contextlib.redirect_stdout(io.StringIO()):
    exec(open(os.path.join(e9_dir, 'e9.py')).read(), g)
instrs, blocks, items, skels, S = g['instrs'], g['blocks'], g['items'], g['skels'], g['S']
bits, split_ops, ENDS = g['bits'], g['split_ops'], g['ENDS']
N = len(instrs)


# ---------------------------------------------------------------- bit I/O, codes
class BitWriter:
    def __init__(self):
        self.parts, self.n, self.by = [], 0, collections.Counter()

    def put(self, v, n, comp):
        if n:
            assert 0 <= v < (1 << n), (v, n)
            self.parts.append(format(v, f'0{n}b')); self.n += n; self.by[comp] += n

    def gamma(self, x, comp):  # Elias gamma, x >= 1
        n = x.bit_length()
        self.put(0, n - 1, comp); self.put(x, n, comp)

    def string(self, s, comp):
        for c in s.encode() + b'\0':
            self.put(c, 8, comp)

    def align(self, comp):
        self.put(0, -self.n % 8, comp)

    def bytes(self):
        s = ''.join(self.parts)
        assert len(s) % 8 == 0
        return int(s, 2).to_bytes(len(s) // 8, 'big') if s else b''


class BitReader:
    def __init__(self, data, pos=0):
        self.s, self.p = ''.join(format(b, '08b') for b in data), pos

    def get(self, n):
        if n == 0:
            return 0
        v = int(self.s[self.p:self.p + n], 2); self.p += n
        return v

    def gamma(self):
        z = 0
        while self.s[self.p] == '0':
            z += 1; self.p += 1
        return self.get(z + 1)

    def string(self):
        out = bytearray()
        while (c := self.get(8)):
            out.append(c)
        return out.decode()


def zz(v):   # signed -> unsigned
    return 2 * v if v >= 0 else -2 * v - 1


def unzz(u):
    return u // 2 if u % 2 == 0 else -(u + 1) // 2


def huff_lengths(cnt):
    """Huffman code lengths for {value: count}; deterministic tie-breaking."""
    if len(cnt) == 1:
        return {v: 0 for v in cnt}
    h = [(c, i, (v,)) for i, (v, c) in enumerate(sorted(cnt.items(), key=lambda t: repr(t[0])))]
    heapq.heapify(h); k = len(h); L = collections.Counter()
    while len(h) > 1:
        a, b = heapq.heappop(h), heapq.heappop(h)
        for v in a[2] + b[2]:
            L[v] += 1
        heapq.heappush(h, (a[0] + b[0], k, a[2] + b[2])); k += 1
    return dict(L)


def canonical(lengths):
    """{value: length} -> {value: (code, length)}, canonical order (length, value)."""
    out, code, prev = {}, 0, 0
    for v, l in sorted(lengths.items(), key=lambda t: (t[1], t[0])):
        code <<= (l - prev); out[v] = (code, l); code += 1; prev = l
    return out


def slot_kind(tmpl):
    if tmpl == 'L':
        return 'label'
    if tmpl == '$I':
        return 'imm'
    if tmpl == '$S' or tmpl.startswith('S'):
        return 'sym'
    if tmpl.startswith('D('):
        return 'disp'
    return None


def skel_parts(sk):
    mn, rest = sk.split(' ', 1)
    return mn, split_ops(rest)


# ---------------------------------------------------------------- decoder (file bytes only)
class Decoder:
    def __init__(self, data):
        r = BitReader(data)
        self.r = r
        nmn, nop, nsk, nrule, self.nsym, self.nfunc, self.W, self.stream_bits = (r.get(32) for _ in range(8))
        mns = [r.string() for _ in range(nmn)]
        ops = [r.string() for _ in range(nop)]
        self.skel = []      # (skeleton string, [slot kinds])
        for _ in range(nsk):
            mn = mns[r.get(bits(nmn))]
            o = [ops[r.get(bits(nop))] for _ in range(r.gamma() - 1)]
            self.skel.append((mn + ' ' + ','.join(o), [k for k in map(slot_kind, o) if k]))
        self.w = bits(nsk + nrule)
        self.rules = [(r.get(self.w), r.get(self.w)) for _ in range(nrule)]
        self.tables = {}    # (skel id, slot j) -> {(len, code): value}
        for si, (sk, kinds) in enumerate(self.skel):
            for j, k in enumerate(kinds):
                if k == 'label':
                    continue
                n = r.gamma()
                vals = [unzz(r.gamma() - 1)]
                for _ in range(n - 1):
                    vals.append(vals[-1] + r.gamma())
                lw = r.gamma() - 1
                lens = {v: r.get(lw) for v in vals}
                self.tables[(si, j)] = {(l, c): v for v, (c, l) in canonical(lens).items()}
        self.entry = [r.get(self.W) for _ in range(self.nfunc)]
        r.p += -r.p % 8
        self.base = r.p
        self.nsk = nsk

    def expand(self, x):
        if x < self.nsk:
            return [x]
        a, b = self.rules[x - self.nsk]
        return self.expand(a) + self.expand(b)

    def decode(self, pos, stop_at_terminator=True):
        """Decode from stream bit address pos. Yields (start, skeleton, values, end)."""
        r = self.r; r.p = self.base + pos
        end = self.base + self.stream_bits
        while r.p < end:
            start = r.p - self.base
            for si in self.expand(r.get(self.w)):
                sk, kinds = self.skel[si]
                vals = []
                for j, k in enumerate(kinds):
                    if k == 'label':
                        vals.append(r.get(self.W))
                    else:
                        t, c, l = self.tables[(si, j)], 0, 0
                        while (l, c) not in t:
                            c = 2 * c + r.get(1); l += 1
                        vals.append(t[(l, c)])
                yield start, sk, vals, r.p - self.base
                start = r.p - self.base
                if stop_at_terminator and ENDS.match(sk):
                    return


# ---------------------------------------------------------------- encoder
# labels -> block index (the next block that starts after the label), as e9 builds blocks
label_block, nb, cur = {}, 0, False
for it in items:
    if it[0] == 'label':
        if cur:
            nb += 1; cur = False
        label_block[it[1]] = nb
        continue
    cur = True
    if ENDS.match(it[1]):
        nb += 1; cur = False
assert nb == len(blocks)
block_first = [b[0] for b in blocks] + [N]

funcs = [l for l in g['label_defs'] if l in g['func_labels']]          # definition order
others = sorted(g['syms'] - set(funcs))
symlist = funcs + others              # symbol index space: functions first (= entry table index)
sym_id = {s: i for i, s in enumerate(symlist)}

skel_id = {s: i for i, s in enumerate(skels)}
seqB = [[skel_id[instrs[i][0]] for i in b] for b in blocks]
hist, rules_all, final_all = g['repair'](seqB, S)

# Stop choice: minimise symbol cost (top-level + rule bodies) over the Re-Pair history; the
# other components do not depend on the stop (except W, by at most a bit per jump).
R = min(hist, key=lambda t: t[1] * bits(S + t[0]) + 2 * t[0] * bits(S + t[0]))[0]
rules = rules_all[:R]


def cut(x):   # express a symbol with rules >= R expanded: Re-Pair state after R rules
    return [x] if x < S + R else cut(rules_all[x - S][0]) + cut(rules_all[x - S][1])


final = [[y for x in blk for y in cut(x)] for blk in final_all]
assert g['expand'](final, rules, S) == seqB
L = sum(map(len, final))
w = bits(S + R)

# operand values in coded form, and per-slot Huffman codes
def coded(v, k):
    return sym_id[v] if k == 'sym' else v


slot_cnt = collections.defaultdict(collections.Counter)
for sk, vals, kinds in instrs:
    for j, (v, k) in enumerate(zip(vals, kinds)):
        if k != 'label':
            slot_cnt[(skel_id[sk], j)][coded(v, k)] += 1
codes = {s: canonical(huff_lengths(c)) for s, c in slot_cnt.items()}


def instr_bits(i, W):
    sk, vals, kinds = instrs[i]
    return sum(W if k == 'label' else codes[(skel_id[sk], j)][coded(v, k)][1]
               for j, (v, k) in enumerate(zip(vals, kinds)))


# stream layout: symbols and their instructions' operands, block by block
def layout(W):
    pos, bpos, i = 0, [], 0
    for blk in final:
        bpos.append(pos)
        for x in blk:
            pos += w
            for _ in g['expand']([[x]], rules, S)[0]:
                pos += instr_bits(i, W); i += 1
    assert i == N
    return bpos + [pos], pos


# Fixed point: raise W from 1 until every address 0..total fits; widths only grow, so the
# result is the smallest W that fits its own stream.
W, iters = 1, []
while True:
    bpos, total = layout(W)
    need = bits(total + 1)          # addresses 0..total
    iters.append((W, total, need))
    if need <= W:
        break
    W = need
lab_addr = {l: bpos[b] for l, b in label_block.items()}

# ---------------------------------------------------------------- write payload.bin
mns = sorted({skel_parts(s)[0] for s in skels})
ops = sorted({o for s in skels for o in skel_parts(s)[1]})
bw = BitWriter()
for v in (len(mns), len(ops), S, R, len(symlist), len(funcs), W, total):
    bw.put(v, 32, 'metadata (header fields)')
for s in mns:
    bw.string(s, 'skeleton table')
for s in ops:
    bw.string(s, 'skeleton table')
for s in skels:
    mn, o = skel_parts(s)
    bw.put(mns.index(mn), bits(len(mns)), 'skeleton table')
    bw.gamma(len(o) + 1, 'skeleton table')
    for x in o:
        bw.put(ops.index(x), bits(len(ops)), 'skeleton table')
for a, b in rules:
    bw.put(a, w, 'Re-Pair rule bodies'); bw.put(b, w, 'Re-Pair rule bodies')
for si, s in enumerate(skels):
    nslots = sum(1 for x in skel_parts(s)[1] if slot_kind(x))
    for j in range(nslots):
        if (si, j) not in codes:
            continue      # label slot
        c = codes[(si, j)]
        vals = sorted(c)
        comp = 'Huffman tables (values + code lengths)'
        bw.gamma(len(vals), comp)
        bw.gamma(zz(vals[0]) + 1, comp)
        for a, b in zip(vals, vals[1:]):
            bw.gamma(b - a, comp)
        lw = bits(max(l for _, l in c.values()) + 1)
        bw.gamma(lw + 1, comp)
        for v in vals:
            bw.put(c[v][1], lw, comp)
for f in funcs:
    bw.put(lab_addr[f], W, 'function entry table')
bw.align('alignment')
stream_start = bw.n
i = 0
for blk in final:
    for x in blk:
        bw.put(x, w, 'top-level symbols')
        for _ in g['expand']([[x]], rules, S)[0]:
            sk, vals, kinds = instrs[i]
            for j, (v, k) in enumerate(zip(vals, kinds)):
                if k == 'label':
                    bw.put(lab_addr[v], W, 'jump target offsets')
                else:
                    c, l = codes[(skel_id[sk], j)][coded(v, k)]
                    bw.put(c, l, 'operands (Huffman, non-jump)')
            i += 1
assert bw.n - stream_start == total
bw.align('alignment')
os.makedirs(out_dir, exist_ok=True)
path = os.path.join(out_dir, 'payload.bin')
open(path, 'wb').write(bw.bytes())
size = os.path.getsize(path)
assert size * 8 == sum(bw.by.values())

# ---------------------------------------------------------------- checks (decoder from file only)
dec = Decoder(open(path, 'rb').read())
ok = collections.Counter(); bad = collections.Counter()


def same(i, sk, vals):
    osk, ovals, okinds = instrs[i]
    if sk != osk or len(vals) != len(ovals):
        return False
    for v, ov, k in zip(vals, ovals, okinds):
        want = lab_addr[ov] if k == 'label' else ov
        got = symlist[v] if k == 'sym' else v     # symlist = the excluded symbol table
        if got != want:
            return False
    return True


def check_from(pos, i0, what):
    """Decode from pos (fresh, file state only) to the first terminator; compare with instrs[i0:]."""
    n = 0
    for _, sk, vals, _ in dec.decode(pos):
        if i0 + n >= N or not same(i0 + n, sk, vals):
            bad[what] += 1; return
        n += 1
    end = i0 + n
    # the original block (and any fall-through blocks) up to the same terminator
    if n and (end == N or ENDS.match(instrs[end - 1][0])):
        ok[what] += 1
    else:
        bad[what] += 1


# 1. full round trip, sequential from bit 0
full = list(dec.decode(0, stop_at_terminator=False))
rt = len(full) == N and all(same(i, sk, v) for i, (_, sk, v, _) in enumerate(full))
# 2. every branch target, reached through the jump offsets decoded from the stream
jumps, targets = 0, {}
for i, (_, sk, vals, _) in enumerate(full):
    for v, k, ov in zip(vals, instrs[i][2], instrs[i][1]):
        if k == 'label':
            jumps += 1
            targets.setdefault(v, set()).add(block_first[label_block[ov]])
for t, firsts in targets.items():
    assert len(firsts) == 1   # one address <-> one original position
    check_from(t, firsts.pop(), 'branch targets')
# 3. every post-call resume point: the decoder's position right after each call
calls = [(i, end) for i, (_, sk, _, end) in enumerate(full) if sk.startswith('call')]
for i, end in calls:
    if i + 1 < N:
        check_from(end, i + 1, 'post-call resume points')
# 4. every function entry through the entry table
for fi, f in enumerate(funcs):
    check_from(dec.entry[fi], block_first[label_block[f]], 'function entries')
# every block start covered by one of the above?
starts = set(targets) | {e for i, e in calls} | set(dec.entry)
block_starts = set(bpos[:-1])

# ---------------------------------------------------------------- report
comps = ['metadata (header fields)', 'skeleton table', 'Re-Pair rule bodies',
         'Huffman tables (values + code lengths)', 'function entry table', 'top-level symbols',
         'operands (Huffman, non-jump)', 'jump target offsets', 'alignment']
assert set(bw.by) <= set(comps)
den = {}
for l in open(os.path.join(out_dir, 'denominators.txt')):
    f = l.split(); den[f[0]] = {k: v for k, v in (x.split('=') for x in f[1:])}
D1, D2 = int(den['stage2-objs.text+rela']['xz9e']), int(den['stage2.text']['xz9e'])
print(f"instructions={N} blocks={len(blocks)} skeletons={S} symbols(index space)={len(symlist)} "
      f"functions={len(funcs)} jumps={jumps} calls={len(calls)}")
print(f"Re-Pair: stop at {R} rules (of {hist[-1][0]} run), top-level symbols={L}, symbol width={w} b")
print(f"jump offset width W: iterations (W, stream bits, needed W) = {iters} -> W={W}")
print(f"payload file: {path}  {size} B  sha256 {hashlib.sha256(open(path, 'rb').read()).hexdigest()[:16]}...")
print(f"  {'component':42s} {'bytes':>9s}")
for c in comps:
    print(f"  {c:42s} {bw.by[c] / 8:9.1f}")
print(f"  {'TOTAL = file length':42s} {size:9d}")
print(f"denominator 1: xz -9e(stage2 .o .text + .rela.text) = {D1} B "
      f"(input {den['stage2-objs.text+rela']['size']} B, sha256 {den['stage2-objs.text+rela']['sha256']}...)")
print(f"denominator 2: xz -9e(linked stage2 .text) = {D2} B "
      f"(input {den['stage2.text']['size']} B, sha256 {den['stage2.text']['sha256']}...)")
print(f"ratio vs denominator 1: {size / D1:.3f}")
print(f"ratio vs denominator 2: {size / D2:.3f}")
print(f"check full round trip ({N} instructions, sequential from bit 0): {'OK' if rt else 'FAIL'}")
print(f"  ({len(targets)} distinct branch-target addresses for "
      f"{len({ov for sk, vals, kinds in instrs for ov, k in zip(vals, kinds) if k == 'label'})} jump-target labels, from {jumps} jumps)")
for what in ('branch targets', 'post-call resume points', 'function entries'):
    print(f"check decode from every {what}: {ok[what]} OK, {bad[what]} FAIL")
print(f"block starts reached by targets/resume points/entries: {len(starts & block_starts)} of "
      f"{len(block_starts)} (the rest are fall-through-only blocks); stray addresses: {len(starts - block_starts - {total})}")
est = len(g['ext_syms']) * 32 // 8
print(f"sensitivity (ESTIMATE, not in the payload): + symbol address table for the {len(g['ext_syms'])} "
      f"non-.text symbols at 32 b each = {est} B -> {size + est} B, ratio {(size + est) / D1:.3f} / {(size + est) / D2:.3f}")
allok = rt and not sum(bad.values())
worst = max(size / D1, size / D2)
print("CHECKS:", "all OK" if allok else "FAILED")
print("DECISION:", ("<= 1.25x both denominators: build and time an interpreter next" if worst <= 1.25
                    else "> 1.25x of at least one denominator: this encoding loses to entropy coding")
      if allok else "checks failed, no decision")
