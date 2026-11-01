import random
random.seed(1)
LEAF=64; NLEAF=1<<14   # 1 MiB base, 64B leaves
tab={}  # hash-cons: key->id ; node=(l,r) or leaf bytes
nodes=[]
def mk(k):
    if k in tab: return tab[k]
    tab[k]=len(nodes); nodes.append(k); return tab[k]
base=[mk(('L',bytes(random.getrandbits(8) for _ in range(LEAF)))) for _ in range(NLEAF)]
def build(a):
    while len(a)>1: a=[mk(('N',a[i],a[i+1])) for i in range(0,len(a),2)]
    return a[0]
def setleaf(root,depth,idx,leaf):
    k=nodes[root]
    if depth==0: return leaf
    half=1<<(depth-1)
    if idx<half: return mk(('N',setleaf(k[1],depth-1,idx,leaf),k[2]))
    return mk(('N',k[1],setleaf(k[2],depth-1,idx-half,leaf)))
D=14; root=build(base); roots=[root]
V=1000; EDITS=10
for v in range(V):
    r=roots[-1]
    for e in range(EDITS):
        i=random.randrange(NLEAF); old=nodes[r]
        r=setleaf(r,D,i,mk(('L',bytes(random.getrandbits(8) for _ in range(LEAF)))))
    roots.append(r)
raw=(V+1)*NLEAF*LEAF
leafbytes=sum(LEAF for k in nodes if k[0]=='L'); internal=sum(1 for k in nodes if k[0]=='N')
gbytes=leafbytes+internal*8
print("raw MB",raw/2**20,"grammar MB",gbytes/2**20,"ratio",raw/gbytes)
# monoid fold (byte sum) over ALL versions, memoized: each unique node visited once
memo={}
for i,k in enumerate(nodes):  # nodes are topologically ordered
    memo[i]= sum(k[1]) if k[0]=='L' else memo[k[1]]+memo[k[2]]
tot=sum(memo[r] for r in roots)
print("fold touches",len(nodes),"nodes; per-version sums computed; total",tot)
# check one version by expansion
def expand(n):
    k=nodes[n]; return k[1] if k[0]=='L' else expand(k[1])+expand(k[2])
assert sum(expand(roots[537]))==memo[roots[537]]; print("check ok")
