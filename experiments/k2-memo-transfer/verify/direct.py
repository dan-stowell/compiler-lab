import sys
T, P, seed = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
M64=(1<<64)-1
st=(seed*0x1234567+77)&M64
bits=[]
for _ in range(P):
    st=(st+0x9E3779B97F4A7C15)&M64; z=st
    z=((z^(z>>30))*0xBF58476D1CE4E5B9)&M64; z=((z^(z>>27))*0x94D049BB133111EB)&M64; z^=z>>31
    bits.append(z&1)
RW=1<<(T+2); p0=(1<<(T+1))-P//2
M=(1<<T)+64
while (RW+2*M)%14: M+=1
L=RW+2*M
E="00010011011111"
cells=[int(E[x%14]) for x in range(-M,RW+M)]
cells[M+p0:M+p0+P]=bits
a=int(''.join(map(str,reversed(cells))),2)
mask=(1<<L)-1
for t in range(1<<T):
    l=(a<<1)&mask; r=a>>1
    a=(a|r)&~(l&a&r)&mask
s=bin(a)[2:].zfill(L)[::-1]
print(s[M+RW//4:M+3*RW//4])
