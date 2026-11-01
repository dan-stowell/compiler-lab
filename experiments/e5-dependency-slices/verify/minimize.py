# Independent check: stub every other function body as {}, then greedily delete
# top-level decls (from the end) while the target's asm stays identical to full-TU asm.
import re, subprocess, sys, os, tempfile
cc, tu = sys.argv[1], sys.argv[2]; targets = sys.argv[3:]
src = open(tu).read()
tok = re.compile(r'\s+|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|\.?\d[\w.]*(?:[eEpP][+-]\w*)?|\.\.\.|->|<<=|>>=|[-+*/%&|^=!<>]=|\+\+|--|<<|>>|&&|\|\||.', re.S)
T = [m.group(0) for m in tok.finditer(src) if not m.group(0).isspace()]
# split
decls=[]; cur=[]; depth=0; body=False; init=False
for i,t in enumerate(T):
    cur.append(t)
    if t=='=' and depth==0: init=True
    if t in '([{':
        if t=='{' and depth==0 and T[i-1]==')' and not init: body=True; hdr=len(cur)-1
        depth+=1
    elif t in ')]}':
        depth-=1
        if t=='}' and depth==0 and body:
            decls.append({'t':cur,'f':True,'h':cur[:hdr]}); cur=[]; body=False; init=False
    elif t==';' and depth==0:
        decls.append({'t':cur,'f':False}); cur=[]; init=False
assert not cur
for d in decls:
    if d['f']:
        h=d['h']; k=max(j for j,x in enumerate(h) if x=='(' and h[:j].count('(')==h[:j].count(')')) if False else None
        # name = last identifier before first top-level '('
        j=h.index('('); d['n']=h[j-1]
total=len(T); print('tokens',total,'decls',len(decls),'funcs',sum(d['f'] for d in decls))
tmp=tempfile.mkdtemp()
def asm(text):
    open(tmp+'/x.c','w').write(text)
    r=subprocess.run([cc,'-S','-o',tmp+'/x.s',tmp+'/x.c'],capture_output=True)
    return open(tmp+'/x.s').read() if r.returncode==0 else None
def cut(a,name):
    out=[];on=False
    for ln in a.split('\n'):
        s=ln.split()
        if s[:1] in (['.globl'],['.local']): on = s[1]==name
        if on and not ln.strip().startswith(('.loc','.file')): out.append(ln)
    text='\n'.join(out); ren={}
    # bijective renaming of every local label regardless of kind
    return re.sub(r'\.L[\w.]*', lambda m: ren.setdefault(m.group(0), 'L%d'%len(ren)), text), set(re.findall(r'\.L\.\.\d+',text))
def strdata(a,labels):
    # data bytes for each string-literal label referenced by the function
    res={}; lines=a.split('\n')
    for i,ln in enumerate(lines):
        if ln.strip().endswith(':') and ln.strip()[:-1] in labels:
            b=[]; j=i+1
            while j<len(lines) and lines[j].strip().startswith('.byte'): b.append(lines[j].strip()); j+=1
            res[ln.strip()[:-1]]=tuple(b)
    return res
full=asm(' '.join(T)); assert full
for name in targets:
    want,wl=cut(full,name); wd=strdata(full,wl)
    tgt=[i for i,d in enumerate(decls) if d['f'] and d['n']==name][0]
    def render(keep):
        return '\n'.join(' '.join(d['t']) if (i==tgt or not d['f']) else ' '.join(d['h'])+' {}' for i,d in enumerate(decls) if i in keep)
    def ok(keep):
        a=asm(render(keep))
        if a is None: return False
        g,gl=cut(a,name); return g==want
    keep=set(range(len(decls)))
    assert ok(keep), name
    for i in reversed(range(len(decls))):
        if i==tgt: continue
        keep.discard(i)
        if not ok(keep): keep.add(i)
    # second pass
    for i in reversed(sorted(keep)):
        if i==tgt: continue
        keep.discard(i)
        if not ok(keep): keep.add(i)
    ntok=sum(len(decls[i]['t']) if (i==tgt or not decls[i]['f']) else len(decls[i]['h'])+2 for i in keep)
    a=asm(render(keep)); g,gl=cut(a,name)
    # compare string data by order of first use
    wseq=[wd[l] for l in sorted(wl,key=lambda l: cut(full,name)[0].find(l))] 
    gd=strdata(a,gl)
    nst=sum(1 for i in keep if decls[i]['f'] and i!=tgt)
    print(f'{name}: min-slice {ntok} tok = {100*ntok/total:.2f}%  decls={len(keep)-1} stubs={nst} strlabels={len(wl)} strdata_same={sorted(wd.values())==sorted(gd.values())}')
    open(f'{os.path.dirname(sys.argv[0])}/{name}.c','w').write(render(keep))
