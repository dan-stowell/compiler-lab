import sys, re, random, statistics
R=sys.argv[1]
OPS={'+':lambda a,b:(a+b)&15,'-':lambda a,b:(a-b)&15,'*':lambda a,b:(a*b)&15,'&':lambda a,b:a&b,'|':lambda a,b:a|b,'^':lambda a,b:a^b}
def parse(s):
    toks=re.findall(r'\d+|[xyz]|[-+*&|^()]',s); i=[0]
    def atom():
        t=toks[i[0]]; i[0]+=1
        if t=='(':
            l=atom(); op=toks[i[0]]; i[0]+=1; r=atom(); assert toks[i[0]]==')'; i[0]+=1; return (op,l,r)
        return t if t in 'xyz' else int(t)
    e=atom(); assert i[0]==len(toks); return e
def ev(e,env):
    if isinstance(e,tuple): return OPS[e[0]](ev(e[1],env),ev(e[2],env))
    return env[e] if isinstance(e,str) else e
BIN={'ADD':'+','SUB':'-','MUL':'*','AND':'&','OR':'|','XOR':'^'}
def run(prog,env):
    st=[]
    for ins in prog.split('; '):
        if ins in ('PUSHX','PUSHY','PUSHZ'): st.append(env[ins[-1].lower()])
        elif ins.startswith('PUSHC '): k=int(ins[6:]); assert 0<=k<16; st.append(k)
        elif ins in BIN: b=st.pop(); a=st.pop(); st.append(OPS[BIN[ins]](a,b))
        elif ins=='DUP': st.append(st[-1])
        elif ins=='SWAP': st[-1],st[-2]=st[-2],st[-1]
        elif ins=='OVER': st.append(st[-2])
        else: raise ValueError(ins)
    if len(st)!=1: raise ValueError('stack %d'%len(st))
    return st[0]
envs=[{'x':x,'y':y,'z':z} for x in range(16) for y in range(16) for z in range(16)]
# folding compiler
def fold(e):
    if not isinstance(e,tuple): return e
    op,l,r=e[0],fold(e[1]),fold(e[2])
    if isinstance(l,int) and isinstance(r,int): return OPS[op](l,r)
    if l==r:
        if op in '-^': return 0
        if op in '&|': return l
    for a,b in ((l,r),(r,l)):  # commutative identities
        if op in '+*&|^' or a is l:
            pass
    if op=='+':
        if l==0: return r
        if r==0: return l
    if op=='-' and r==0: return l
    if op=='*':
        if l==0 or r==0: return 0
        if l==1: return r
        if r==1: return l
    if op=='&':
        if l==0 or r==0: return 0
        if l==15: return r
        if r==15: return l
    if op=='|':
        if l==15 or r==15: return 15
        if l==0: return r
        if r==0: return l
    if op=='^':
        if l==0: return r
        if r==0: return l
    return (op,l,r)
def size(e): return 2*size(e[1])+0 if False else (size(e[1])+size(e[2])+1 if isinstance(e,tuple) else 1)
bad=0; ratios=[]; rows=[]; foldbad=0; fs=[]
for line in open(R):
    f=line.rstrip('\n').split('\t')
    idx,es,dl,sl,status,dp,fp=int(f[0]),f[1],int(f[2]),int(f[3]),f[4],f[9],f[10]
    e=parse(es)
    tv=[ev(e,v) for v in envs]
    for p,L in ((dp,dl),(fp,sl)):
        if len(p.split('; '))!=L or [run(p,v) for v in envs]!=tv: bad+=1; print('MISMATCH',idx,p)
    fe=fold(e); 
    if [ev(fe,v) for v in envs]!=tv: foldbad+=1
    fl=size(fe); fs.append(fl)
    ratios.append(sl/fl); rows.append((idx,es,dl,sl,fl,status))
    if sl>fl: print('SEARCH LONGER THAN FOLDED?',idx,es,sl,fl)
print('programs failing check:',bad,'/400; fold wrong:',foldbad)
print('mean search/derived',statistics.mean(r[3]/r[2] for r in rows),'median',statistics.median(r[3]/r[2] for r in rows),'shorter',sum(r[3]<r[2] for r in rows))
print('mean folded/derived',statistics.mean(r[4]/r[2] for r in rows),'mean folded len',statistics.mean(fs))
print('mean search/folded',statistics.mean(ratios),'median',statistics.median(ratios),'search strictly shorter than folded',sum(r[3]<r[4] for r in rows))
random.seed(1)
opt=[r for r in rows if r[5]=='derived_optimal' and r[2] in (5,7)]
print('SAMPLE', ' '.join(str(r[0]) for r in random.sample(opt,10)))
