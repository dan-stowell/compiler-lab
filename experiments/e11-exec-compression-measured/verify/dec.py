"""Independent decoder for e11 payload.bin + ground-truth check against the stage2 .s files.
usage: python3 dec.py payload.bin ASM_DIR"""
import sys, os, re, glob, collections
data = open(sys.argv[1], 'rb').read()
NB = len(data) * 8
BIG = int.from_bytes(data, 'big')
def rd(p, n):
    return (BIG >> (NB - p - n)) & ((1 << n) - 1) if n else 0
def clog2(n): return (n - 1).bit_length() if n > 1 else 0
class R:
    def __init__(s, p=0): s.p = p
    def get(s, n): v = rd(s.p, n); s.p += n; return v
    def gam(s):
        z = 0
        while rd(s.p, 1) == 0: z += 1; s.p += 1
        return s.get(z + 1)
    def cstr(s):
        b = bytearray()
        while True:
            c = s.get(8)
            if not c: return b.decode()
            b.append(c)
r = R()
nmn, nop, nsk, nrule, nsym, nfunc, W, total = [r.get(32) for _ in range(8)]
print('header', nmn, nop, nsk, nrule, nsym, nfunc, W, total)
mns = [r.cstr() for _ in range(nmn)]; ops = [r.cstr() for _ in range(nop)]
def kind(t):
    if t == 'L': return 'L'
    if t == '$I': return 'I'
    if t == '$S' or t[0] == 'S': return 'S'
    if t.startswith('D('): return 'D'
    return None
sk = []
for _ in range(nsk):
    m = mns[r.get(clog2(nmn))]
    o = [ops[r.get(clog2(nop))] for _ in range(r.gam() - 1)]
    sk.append((m, o, [kind(t) for t in o if kind(t)]))
w = clog2(nsk + nrule)
rules = [(r.get(w), r.get(w)) for _ in range(nrule)]
def unzz(u): return u // 2 if u % 2 == 0 else -(u + 1) // 2
tabs = {}
for si, (m, o, ks) in enumerate(sk):
    for j, k in enumerate(ks):
        if k == 'L': continue
        n = r.gam(); v = [unzz(r.gam() - 1)]
        for _ in range(n - 1): v.append(v[-1] + r.gam())
        lw = r.gam() - 1
        ln = [r.get(lw) for _ in v]
        # canonical: order by (len, value)
        t = {}; code = 0; prev = 0
        for l, val in sorted(zip(ln, v)):
            code <<= (l - prev); t[(l, code)] = val; code += 1; prev = l
        tabs[(si, j)] = t
entry = [r.get(W) for _ in range(nfunc)]
r.p = (r.p + 7) // 8 * 8
base = r.p
print('stream base bit', base, 'file bits', NB, 'stream end', base + total)
def exp(x):
    out, st = [], [x]
    while st:
        y = st.pop()
        if y < nsk: out.append(y)
        else: a, b = rules[y - nsk]; st.append(b); st.append(a)
    return out
TERM = re.compile(r'^(j\w+|call|ret)$')
def decode(pos, stop=True):
    rr = R(base + pos); out = []
    while rr.p < base + total:
        sst = rr.p - base
        for n_, si in enumerate(exp(rr.get(w))):
            st = sst if n_ == 0 else rr.p - base
            m, o, ks = sk[si]; vals = []
            for j, k in enumerate(ks):
                if k == 'L': vals.append(rr.get(W))
                else:
                    t = tabs[(si, j)]; c = l = 0
                    while (l, c) not in t:
                        c = 2 * c + rr.get(1); l += 1
                        assert l < 64
                    vals.append(t[(l, c)])
            out.append((st, si, vals, rr.p - base))
            if stop and TERM.match(m): return out
    return out

# ---------------- ground truth from the .s files (own parser)
LBL = re.compile(r'^([A-Za-z_.$][\w.$]*):$')
gt = []; labpos = {}; funcorder = []; funcs_all = set()
for path in sorted(glob.glob(os.path.join(sys.argv[2], '*.s'))):
    tu = os.path.basename(path); L = [l.strip() for l in open(path)]
    defd = {LBL.match(l).group(1) for l in L if LBL.match(l)} | {l.split()[1].rstrip(',') for l in L if l.startswith('.comm ')}
    glob_ = {l.split()[1] for l in L if l.startswith('.globl ')}
    fn = {l.split()[1].rstrip(',') for l in L if l.startswith('.type ') and l.endswith('@function')}
    Q = lambda n, tu=tu, defd=defd, glob_=glob_: (tu, n) if n in defd and n not in glob_ else n
    intext = False
    for l in L:
        if not l: continue
        if l.split()[0] in ('.text',): intext = True; continue
        if l.split()[0] in ('.data', '.bss', '.section'): intext = False; continue
        if not intext: continue
        m = LBL.match(l)
        if m:
            labpos[Q(m.group(1))] = len(gt)
            if m.group(1) in fn: funcorder.append(Q(m.group(1))); funcs_all.add(Q(m.group(1)))
            continue
        if l.startswith('.'): continue
        gt.append((l, Q))
print('gt instructions', len(gt), 'labels', len(labpos), 'functions', len(funcorder))

full = decode(0, False)
print('decoded instructions', len(full))
starts = [s for s, *_ in full] + [total]
# render & compare
sym_of = {}; name_of = {}; jtargets = collections.Counter(); jlabels = set(); textlabel_syms = set(); nmis = 0
def tok(op, Q):
    return op
for i, ((st, si, vals, en), (line, Q)) in enumerate(zip(full, gt)):
    m, o, ks = sk[si]
    parts = line.split(None, 1); gm = parts[0]; gops = []
    if len(parts) > 1:
        d = 0; cur = ''
        for ch in parts[1]:
            d += ch == '('; d -= ch == ')'
            if ch == ',' and d == 0: gops.append(cur.strip()); cur = ''
            else: cur += ch
        if cur.strip(): gops.append(cur.strip())
    ok = gm == m and len(gops) == len(o)
    vi = 0
    if ok:
        for t, g_ in zip(o, gops):
            k = kind(t)
            if k is None:
                ok &= t == g_; continue
            v = vals[vi]; vi += 1
            if k == 'I': ok &= g_ == f'${v}'
            elif k == 'D': ok &= g_ == f'{v}(' + t[2:]
            elif k == 'L':
                name = Q(g_); jlabels.add(name); jtargets[v] += 1
                ok &= name in labpos and starts[labpos[name]] == v
            else:  # symbol
                if t == '$S': nm = g_[1:]
                else:
                    pre, b = g_.split('(', 1); s_, _, rel = pre.partition('@')
                    ok &= t == 'S' + ('@' + rel if rel else '') + '(' + b; nm = s_
                nm = Q(nm)
                if nm in labpos and nm not in funcs_all: textlabel_syms.add(nm)
                if sym_of.setdefault(v, nm) != nm or name_of.setdefault(nm, v) != v: ok = False
    if not ok:
        nmis += 1
        if nmis < 5: print('MISMATCH', i, line, m, o, vals)
print('round trip mismatches', nmis, 'len equal', len(full) == len(gt))
print('sym bijection: indices used', len(sym_of), 'nsym', nsym)
print('functions-first check:', all(sym_of.get(i, funcorder[i]) == funcorder[i] for i in range(nfunc)),
      'entry table == label positions:', all(entry[i] == starts[labpos[f]] for i, f in enumerate(funcorder)))
print('text labels referenced as symbols (not functions):', len(textlabel_syms), sorted(textlabel_syms)[:5])
print('jumps', sum(jtargets.values()), 'distinct jump labels', len(jlabels), 'distinct target addresses', len(jtargets))
print('labels defined in .text', len(labpos), 'distinct label positions', len(set(labpos.values())))
indirect = [l for l, _ in gt if re.match(r'(j\w+|call)\s+\*', l)]
print('indirect jumps/calls', len(indirect), collections.Counter(l.split()[0] + ' ' + l.split()[1][:2] for l in indirect))
# fresh decodes
idx_of = {s: i for i, s in enumerate(starts[:-1])}
def chk(pos):
    if pos not in idx_of: return False
    i0 = idx_of[pos]; seq = decode(pos)
    return [x[1:3] for x in seq] == [x[1:3] for x in full[i0:i0 + len(seq)]] and TERM.match(sk[seq[-1][1]][0])
bt = [chk(p) for p in jtargets]
rp = [chk(en) for st, si, v, en in full if sk[si][0] == 'call' and en < total]
fe = [chk(p) for p in entry]
print('fresh decode: branch targets %d/%d, resume %d/%d, entries %d/%d' % (sum(map(bool,bt)), len(bt), sum(map(bool,rp)), len(rp), sum(map(bool,fe)), len(fe)))
