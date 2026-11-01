"""Reference dump from E11's own Python decoder, for the C decoder's round-trip check.

usage: python3 dump_e11.py E11_DIR E9_DIR SCRATCH_DIR OUT_TXT
Execs E11's e11.py (it rebuilds payload.bin into SCRATCH_DIR and runs its checks), asserts the
rebuilt payload equals E11_DIR/work/payload.bin, then writes one line per instruction of
E11's sequential decode from bit 0: "start skeleton_index v0 v1 ..." (labels = stream bit
addresses, symbols = symbol indices, imm/disp = values), the same format as `e12dec dump`.
"""
import contextlib, io, os, shutil, sys

e11_dir, e9_dir, scratch, out = sys.argv[1:5]
os.makedirs(scratch, exist_ok=True)
shutil.copy(os.path.join(e11_dir, 'work/denominators.txt'), scratch)
sys.argv = ['e11.py', e9_dir, scratch]
g = {'__name__': 'e11'}
log = io.StringIO()
with contextlib.redirect_stdout(log):
    exec(open(os.path.join(e11_dir, 'e11.py')).read(), g)
assert 'CHECKS: all OK' in log.getvalue(), log.getvalue()
assert open(os.path.join(scratch, 'payload.bin'), 'rb').read() == \
    open(os.path.join(e11_dir, 'work/payload.bin'), 'rb').read(), 'rebuilt payload differs from E11'
dec = g['dec']
sk_index = {s: i for i, (s, _) in enumerate(dec.skel)}
assert len(sk_index) == len(dec.skel)
with open(out, 'w') as o:
    for start, sk, vals, end in g['full']:
        o.write(' '.join(map(str, [start, sk_index[sk]] + vals)) + '\n')
print(f"E11 Python decode: {len(g['full'])} instructions -> {out}")
