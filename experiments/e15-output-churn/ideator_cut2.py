import sys, gzip, random, importlib.util, collections, tempfile, os
sp=importlib.util.spec_from_file_location("e8","/home/exedev/lab-e10/experiments/e8-slices-O2/slice.py")
e8=importlib.util.module_from_spec(sp); sp.loader.exec_module(e8)
W="/home/exedev/lab-e10/experiments/e10-hidden-deps/work"
CC=["clang-18","-std=c99","-O2","-fno-stack-protector","-fno-common","-fno-pic","-w"]
per=collections.defaultdict(dict)
for ln in open(W+"/tok.tsv"):
    i,c,f,st,h=ln.split()[:5]
    if st=="ok": per[f][int(i)]=h
pairs=[]
for f,m in per.items():
    ks=sorted(m)
    for a,b in zip(ks,ks[1:]):
        if b==a+1 and m[a]!=m[b]: pairs.append((f,m[a],m[b]))
random.seed(2); random.shuffle(pairs); pairs=pairs[:int(sys.argv[1])]
def clo(T,t):
    b={t}; fr={t}
    while fr:
        fr=set().union(*(T.calls[x] for x in fr)) & T.static_names - b; b|=fr
    return b
tmp=tempfile.mkdtemp(dir=".")
def asm(src,t,tag):
    r=e8.compile_s(CC,src,tmp,tag)
    return e8.norm_func(e8.parse_asm(r[0]),t) if r[0] else None
c=collections.Counter(); per_pair=[]
for f,ha,hb in pairs:
    A=e8.TU(gzip.open(f"{W}/cas/{ha}.i.gz","rt").read()); B=e8.TU(gzip.open(f"{W}/cas/{hb}.i.gz","rt").read())
    pk=0; po=0; pb=0
    for t in A.fdef:
        if t not in B.fdef or t in A.static_names: continue
        c["fn"]+=1
        sa=A.build(clo(A,t))[0]; sb=B.build(clo(B,t))[0]
        if " ".join(sa.split())==" ".join(sb.split()): continue
        pk+=1; c["key_changed"]+=1
        ba=" ".join(" ".join(A.txt(A.fdef[x].toks).split()) for x in sorted(clo(A,t))); bb=" ".join(" ".join(B.txt(B.fdef[x].toks).split()) for x in sorted(clo(B,t)))
        if ba!=bb: c["body_changed"]+=1; pb+=1
        a1=asm(sa,t,"a"); a2=asm(sb,t,"b")
        if a1 is None or a2 is None: c["compfail"]+=1; continue
        if a1!=a2:
            c["asm_changed"]+=1; po+=1
            if ba==bb: c["asm_changed_ctx_only"]+=1
    per_pair.append((pk,po,pb))
print(c)
allk=sum(1 for p in per_pair if p[0]>0); print("pairs",len(per_pair),"pairs w/ any key change",allk,"pairs w/ any asm change",sum(1 for p in per_pair if p[1]>0))
