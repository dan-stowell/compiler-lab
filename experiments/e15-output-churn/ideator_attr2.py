import sys, gzip, random, importlib.util, os, hashlib, collections
sp=importlib.util.spec_from_file_location("e8","/home/exedev/lab-e10/experiments/e8-slices-O2/slice.py")
e8=importlib.util.module_from_spec(sp); sp.loader.exec_module(e8)
W="/home/exedev/lab-e10/experiments/e10-hidden-deps/work"
per=collections.defaultdict(dict)
for ln in open(W+"/tok.tsv"):
    i,c,f,st,h=ln.split()[:5]
    if st=="ok": per[f][int(i)]=h
pairs=[]
for f,m in per.items():
    ks=sorted(m)
    for a,b in zip(ks,ks[1:]):
        if b==a+1 and m[a]!=m[b]: pairs.append((f,m[a],m[b]))
random.seed(1); random.shuffle(pairs); pairs=pairs[:int(sys.argv[1])]
cache={}
def TU(h):
    if h not in cache:
        if len(cache)>4: cache.clear()
        cache[h]=e8.TU(gzip.open(f"{W}/cas/{h}.i.gz","rt").read())
    return cache[h]
def kind(d):
    T=[t for _,t,_,_ in d.toks]
    if d.is_func: return "funcbody"
    s=" ".join(T[:3])
    if "typedef" in T[:1]:
        return "typedef-struct" if ("struct" in T[:3] or "union" in T[:3]) and "{" in T else "typedef"
    if T[0] in("struct","union","enum") and "{" in T: return T[0]+"-def"
    if "(" in T and T[-1]==";": return "proto/extern"
    return "other"
tot=collections.Counter(); attr=collections.Counter(); names=collections.Counter()
for f,ha,hb in pairs:
    A,B=TU(ha),TU(hb)
    for t in A.fdef:
        if t not in B.fdef or t in A.static_names: continue
        ta=A.txt(A.fdef[t].toks); tb=B.txt(B.fdef[t].toks)
        tot["fn"]+=1
        if " ".join(ta.split())!=" ".join(tb.split()): tot["body"]+=1; continue
        sa=A.build({t})[0]; sb=B.build({t})[0]
        if " ".join(sa.split())==" ".join(sb.split()): continue
        tot["ctx_only"]+=1
        na=" ".join(sa.split()); nb=" ".join(sb.split())
        da=set(); db=set()
        for d in A.decls:
            x=" ".join(A.txt(d.toks).split()); 
            if x in na: da.add(x)
            if d.is_func: da.add(" ".join((A.txt(d.header)+";").split()))
        for d in B.decls:
            x=" ".join(B.txt(d.toks).split())
            if x in nb: db.add(x)
        # classify changed decls by re-finding them
        ks=set()
        for d in B.decls:
            x=" ".join(B.txt(d.toks).split())
            if x in db and x not in da:
                k=kind(d) if not (d.is_func and d.name!=t) else "funcbody"
                ks.add(k); names[(k,(d.name if d.is_func else " ".join(sorted(d.decl_ord|d.decl_tag))[:40]))]+=1
        for d in B.funcs:
            if d.name!=t:
                x=" ".join((B.txt(d.header)+";").split())
                if x in db and x not in da: ks.add("callee-proto")
        for k in ks: attr[k]+=1
        if not ks:
            attr["removed-only"]+=1
            if attr["removed-only"]<3:
                import difflib; print("\n".join(list(difflib.unified_diff(sa.split("\n"),sb.split("\n"),lineterm="",n=0))[:12]))
print(len(pairs),tot); print(attr); print(names.most_common(15))
