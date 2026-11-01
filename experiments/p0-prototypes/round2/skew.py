import numpy as np, scipy.sparse as sp, sys
rng=np.random.default_rng(1)
n=1<<18; d=16; B=64  # B blocks (each ~L2-sized in a scaled version)
def graph(pin):
    src=np.repeat(np.arange(n),d); blk=src*B//n
    local=rng.random(n*d)<pin
    dst=np.where(local, blk*(n//B)+rng.integers(0,n//B,n*d), rng.integers(0,n,n*d))
    A=sp.csr_matrix((np.ones(n*d),(dst,src)),shape=(n,n))
    return A.multiply(1.0/d).tocsr()   # column-stochastic: out-degree d
def run(P,inner,tol=1e-9,a=0.85):
    x=np.full(n,1/n); b=(1-a)/n; passes=0
    blocks=[slice(k*n//B,(k+1)*n//B) for k in range(B)]
    rows=[P[s] for s in blocks]
    while True:
        passes+=1; old=x.copy()
        for s,R in zip(blocks,rows):
            for _ in range(inner):          # block-local iterations, block stays resident
                x[s]=a*(R@x)+b
        if np.abs(x-old).sum()<tol: return passes
for pin in [0.0,0.9,0.99,0.999]:
    P=graph(pin)
    print(pin,[run(P,k) for k in (1,4,16)],flush=True)
