"""usage: python3 sections.py OUT OBJ...  -> OUT = all objects' .text, then all their .rela.text.

Same concatenation as E9's verify/elfsec.py (sorted object order). .symtab/.strtab are left out.
"""
import struct, sys


def secs(p):
    d = open(p, 'rb').read()
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3a)
    sh = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize) for i in range(shnum)]
    so = sh[shstrndx][4]
    out = {}
    for s in sh:
        n = d[so + s[0]:d.index(b'\0', so + s[0])].decode()
        out[n] = d[s[4]:s[4] + s[5]] if s[1] != 8 else b''
    return out


objs = [secs(p) for p in sorted(sys.argv[2:])]
open(sys.argv[1], 'wb').write(b''.join(s['.text'] for s in objs) + b''.join(s.get('.rela.text', b'') for s in objs))
