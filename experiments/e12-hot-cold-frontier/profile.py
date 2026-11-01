"""E12 step 1: per-function dynamic instruction counts and native sizes for stage2 chibicc.

usage: python3 profile.py CALLGRIND_OUT STAGE2_BINARY ASM_DIR OUT_CSV

CALLGRIND_OUT must come from `valgrind --tool=callgrind --dump-instr=yes`. Self cost (Ir) of
every cost line is attributed by instruction address: inside the stage2 binary to the .text
symbol whose [addr, next symbol addr) range contains it, elsewhere to the object (libc, ld.so).
chibicc emits no `.size` directives, so `nm --size` reports nothing for its functions; native
size = address delta to the next .text symbol (includes any padding before the next symbol).
The chibicc functions are the `@function` symbols of ASM_DIR/*.s (E9's stage2 asm); the other
.text symbols (crt _start etc., the static atexit stub) are counted as "other native".
"""
import bisect, collections, glob, re, subprocess, sys

cg, binary, asm_dir, out_csv = sys.argv[1:5]

# ---- .text symbols of the binary, by address
syms = []
for l in subprocess.run(['nm', '-n', binary], capture_output=True, text=True).stdout.splitlines():
    f = l.split()
    if len(f) == 3 and f[1] in 'tT':
        syms.append((int(f[0], 16), f[2]))
sec = {}
for l in subprocess.run(['readelf', '-SW', binary], capture_output=True, text=True).stdout.splitlines():
    m = re.search(r'\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)', l)
    if m:
        sec[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
tlo, tsz = sec['.text']
thi = tlo + tsz
syms = [s for s in syms if tlo <= s[0] < thi]
addrs = [a for a, _ in syms]
size = {i: (addrs[i + 1] if i + 1 < len(syms) else thi) - a for i, (a, _) in enumerate(syms)}
assert sum(size.values()) == thi - addrs[0]

cfuncs = set()
for p in glob.glob(asm_dir + '/*.s'):
    for l in open(p):
        m = re.match(r'\s*\.type\s+([\w.$]+),\s*@function', l)
        if m:
            cfuncs.add(m.group(1))
is_c = [name in cfuncs for _, name in syms]

# ---- parse callgrind (positions: instr line)
names = {'ob': {}, 'fl': {}, 'fn': {}}
ns_of = {'ob': 'ob', 'cob': 'ob', 'fl': 'fl', 'fi': 'fl', 'fe': 'fl', 'cfi': 'fl', 'cfl': 'fl',
         'fn': 'fn', 'cfn': 'fn'}


def name(key, rest):
    m = re.match(r'\((\d+)\)\s*(.*)', rest)
    if not m:
        return rest
    d = names[ns_of[key]]
    if m.group(2):
        d[m.group(1)] = m.group(2)
    return d[m.group(1)]


ob = fn = None
after_calls = False
last_addr = 0
self_ir = collections.Counter()        # function index -> Ir
obj_ir = collections.Counter()         # object -> Ir (non-binary objects)
fn_first = {}                          # (ob, fn) -> lowest address seen (for the load bias)
lines = []
total = None
for l in open(cg):
    l = l.rstrip('\n')
    if not l or l.startswith('#'):
        continue
    m = re.match(r'(\w+)=(.*)', l)
    if m:
        k, rest = m.groups()
        if k == 'ob':
            ob = name(k, rest)
        elif k == 'fn':
            fn = name(k, rest)
        elif k in ns_of:
            name(k, rest)
        elif k == 'calls':
            after_calls = True
        continue
    if l.startswith('summary:'):
        total = int(l.split()[1])
        continue
    if not (l[0].isdigit() or l[0] in '+-*'):
        continue
    f = l.split()
    a = f[0]
    if a == '*':
        addr = last_addr
    elif a[0] in '+-':
        addr = last_addr + (1 if a[0] == '+' else -1) * int(a[1:], 0)
    else:
        addr = int(a, 0)
    last_addr = addr
    ir = int(f[2]) if len(f) > 2 else 0
    if after_calls:              # inclusive cost of a call: not self cost
        after_calls = False
        continue
    lines.append((ob, fn, addr, ir))

# load bias of the binary: callgrind addresses are runtime addresses
bias_votes = collections.Counter()
by_name = {n: a for a, n in syms if [x for _, x in syms].count(n) == 1}
for ob_, fn_, addr, ir in lines:
    if ob_ and ob_.endswith(binary.split('/')[-1]) and fn_ in by_name:
        fn_first[fn_] = min(fn_first.get(fn_, addr), addr)
for fn_, a in fn_first.items():
    bias_votes[a - by_name[fn_]] += 1
bias = bias_votes.most_common(1)[0][0]
for ob_, fn_, addr, ir in lines:
    if ob_ and ob_.endswith(binary.split('/')[-1]):
        a = addr - bias
        if tlo <= a < thi:
            self_ir[bisect.bisect_right(addrs, a) - 1] += ir
        else:
            obj_ir[ob_ + ' (outside .text: plt/init)'] += ir
    else:
        obj_ir[ob_] += ir
assert sum(self_ir.values()) + sum(obj_ir.values()) == total, (sum(self_ir.values()) + sum(obj_ir.values()), total)

with open(out_csv, 'w') as o:
    o.write('rank,function,kind,self_ir,native_bytes\n')
    rows = sorted(range(len(syms)), key=lambda i: (-self_ir[i], syms[i][0]))
    r = 0
    for i in rows:
        kind = 'chibicc' if is_c[i] else 'other-native'
        if kind == 'chibicc':
            r += 1
        o.write(f"{r if kind == 'chibicc' else ''},{syms[i][1]}@{syms[i][0]:x},{kind},{self_ir[i]},{size[i]}\n")
    for k, v in obj_ir.most_common():
        o.write(f",{k},library,{v},\n")

c_ir = sum(v for i, v in self_ir.items() if is_c[i])
o_ir = sum(v for i, v in self_ir.items() if not is_c[i])
print(f"callgrind total Ir={total}  load bias=0x{bias:x}  chibicc functions={sum(is_c)} "
      f"(native bytes {sum(size[i] for i in size if is_c[i])})  .text={tsz} B")
print(f"Ir: chibicc functions {c_ir} ({c_ir / total:.2%}); other binary .text {o_ir}; "
      + '; '.join(f"{k} {v} ({v / total:.2%})" for k, v in obj_ir.most_common()))
