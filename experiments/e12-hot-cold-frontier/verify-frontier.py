import csv,sys
P=sys.argv[1]; T=float(sys.argv[2]); c=float(sys.argv[3])*1e-9
tot=967034299; text=263634
rows=sorted([r for r in csv.DictReader(open(P)) if r['kind']=='chibicc'],key=lambda r:-int(r['self_ir']))
ir=[int(r['self_ir']) for r in rows]; nb=[int(r['native_bytes']) for r in rows]
allb=sum(nb)
def go(m,fixedfn):
  best=None
  for k in range(325):
    cold=sum(ir[k:]); hot=sum(nb[:k])
    r=1-cold/tot+cold*m*c/T
    if r<=1.5:
      res=hot+fixedfn(k)
      if best is None or res<best[1]: best=(k,res)
  return best[0],best[1],best[1]/text
for m in (1,3):
  print(m,'claimed acct',go(m,lambda k:47034+5292+35168+3748))
  print(m,'cold payload pro rata bytes, full tables',go(m,lambda k:47034*sum(nb[k:])/allb+5292+35168+3748))
  for tabs in (35168, 8000, 4000):
    print(m,'cold payload, tables',tabs,go(m,lambda k:47034*sum(nb[k:])/allb+5292+tabs+3748))
