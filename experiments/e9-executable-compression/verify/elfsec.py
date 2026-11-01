import struct,sys,glob,lzma
def secs(p):
    d=open(p,'rb').read()
    shoff,=struct.unpack_from('<Q',d,0x28); shentsize,shnum,shstrndx=struct.unpack_from('<HHH',d,0x3a)
    sh=[struct.unpack_from('<IIQQQQIIQQ',d,shoff+i*shentsize) for i in range(shnum)]
    so=sh[shstrndx][4]
    out={}
    for s in sh:
        n=d[so+s[0]:d.index(b'\0',so+s[0])].decode()
        out[n]=d[s[4]:s[4]+s[5]] if s[1]!=8 else b''
    return out
T=R=SY=b''; inter=b''
for p in sorted(glob.glob(sys.argv[1]+'/*.o')):
    s=secs(p); T+=s['.text']; R+=s.get('.rela.text',b''); SY+=s['.symtab']+s['.strtab']
    inter+=s['.text']+s.get('.rela.text',b'')
x=lambda b: len(lzma.compress(b,preset=9|lzma.PRESET_EXTREME))
print('text',len(T),x(T),'rela',len(R),x(R),'nrel',len(R)//24)
print('text+rela',x(T+R),'interleaved',x(inter),'text+rela+symtab/strtab',x(T+R+SY))
