import sys,subprocess,re,os
cc,tu,d=sys.argv[1:4]
def asm(c):
    subprocess.run([cc,'-S','-o','/tmp/claude-1000/-home-exedev/8dacbf12-f718-44f5-a239-aefe4726da86/scratchpad/verify-e5/t.s',c],check=True); return open('/tmp/claude-1000/-home-exedev/8dacbf12-f718-44f5-a239-aefe4726da86/scratchpad/verify-e5/t.s').read()
def cut(a,name):
    out=[];on=False
    for ln in a.split('\n'):
        s=ln.split()
        if s[:1] in (['.globl'],['.local']): on = s[1]==name
        if on and not ln.strip().startswith(('.loc','.file')): out.append(ln)
    text='\n'.join(out).rstrip(); ren={}
    labs=re.findall(r'\.L\.\.\d+',text)
    return re.sub(r'\.L[\w.]*', lambda m: ren.setdefault(m.group(0),'L%d'%len(ren)), text), list(dict.fromkeys(labs))
def data(a,l):
    m=re.search(re.escape(l)+r':\n((?:\s*\.byte.*\n)*)',a); return m.group(1) if m else None
full=asm(tu); bad=0; raw=0
for f in sorted(os.listdir(d)):
    n=f[:-2]; a=asm(os.path.join(d,f))
    w,wl=cut(full,n); g,gl=cut(a,n)
    if a.count(n+':')==0: print('missing',n)
    # raw (no label normalization) compare too
    if w!=g or [data(full,x) for x in wl]!=[data(a,x) for x in gl]: bad+=1; print('DIFF',n)
    if w.count('\n')<3: print('tiny',n)
print('checked',len(os.listdir(d)),'bad',bad)
