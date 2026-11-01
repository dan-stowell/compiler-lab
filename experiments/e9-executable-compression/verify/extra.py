import sys, io, contextlib, collections, math
sys.argv=['e9.py','work/s2','work/denominators.txt']
g={'__name__':'e9'}
with contextlib.redirect_stdout(io.StringIO()):
    exec(open('e9.py').read(), g)
instrs=g['instrs']; blocks=g['blocks']; slot_n=g['slot_n']; slot_vals=g['slot_vals']; width=g['width']; B=g['B']; bits=g['bits']
Bv=g['Bv']; D=g['D']; DL=g['DL']
kinds=collections.Counter(); kbits=collections.Counter()
for (sk,j),n in slot_n.items():
    kinds[g['slot_kind'][(sk,j)]]+=n; kbits[g['slot_kind'][(sk,j)]]+=n*width[(sk,j)]
print('operands by kind (count, fixed-width bytes):',{k:(kinds[k],kbits[k]//8) for k in kinds})
# zero-order entropy per slot
H=0
cnt=collections.defaultdict(collections.Counter)
for sk,vals,ks in instrs:
    for j,v in enumerate(vals): cnt[(sk,j)][v]+=1
for s,c in cnt.items():
    n=sum(c.values()); H+=-sum(x*math.log2(x/n) for x in c.values())
print('operand stream: fixed %d B, per-slot order-0 entropy %d B'%(g['operand_stream']//8,H/8))
# Huffman per slot (actual code lengths)
import heapq
def huff(c):
    if len(c)==1: return 0
    h=[(v,i,0) for i,v in enumerate(c.values())]; heapq.heapify(h); tot=0;k=len(h)
    while len(h)>1:
        a=heapq.heappop(h); b=heapq.heappop(h); tot+=a[0]+b[0]; heapq.heappush(h,(a[0]+b[0],k,0)); k+=1
    return tot
Hf=sum(huff(c) for c in cnt.values()); print('per-slot Huffman operands %d B'%(Hf/8))
# shared dict across all slots of same kind
kc=collections.defaultdict(collections.Counter)
for sk,vals,ks in instrs:
    for v,k in zip(vals,ks): kc[k][v]+=1
print('shared-per-kind fixed width B:', sum(sum(c.values())*bits(len(c)) for c in kc.values())//8)
# label refs
lab_refs=sum(1 for sk,vals,ks in instrs for k in ks if k=='label')
print('jump label refs',lab_refs,'label table entries',B)
# grammar/label rows
parts=Bv['parts']; tot=Bv['total']/8
gram=(parts['top-level symbols']+parts['rule bodies'])/8
print('B total',tot,'grammar',gram,'label',parts['label table']/8,'operands',parts['operands (separate stream)']/8)
floor=tot-gram+len(blocks)*0  # zero grammar cost
print('B with zero grammar cost: %.0f B ratio %.3f'%(tot-gram, (tot-gram)/D))
# Tweak 1: interleave operands into symbol stream -> one offset per label
comb_bits=parts['top-level symbols']+parts['rule bodies']+parts['operands (separate stream)']
lt=B*bits(comb_bits); t1=tot-parts['label table']/8+lt/8
print('tweak1 interleaved, label entry %d bits: %.0f B ratio %.3f / linked %.3f / text+rela %.3f'%(bits(comb_bits),t1,t1/D,t1/DL,t1/41212))
# Tweak 2: + per-slot Huffman operands (still decodable sequentially within block; random access at labels via bit offset)
t2=t1-parts['operands (separate stream)']/8+Hf/8
print('tweak1+2 Huffman operands: %.0f B ratio %.3f / linked %.3f / text+rela %.3f'%(t2,t2/D,t2/DL,t2/41212))
# Tweak 3: Huffman on skeleton/rule symbols of top-level stream
# Tweak 3: jump operands hold the target bit offset directly; table only for 324 functions
lab_slots=[s for s in cnt if g['slot_kind'][s]=='label']
Hf_nolab=sum(huff(cnt[s]) for s in cnt if s not in lab_slots)
fn=len(g['func_labels'])
t3=(parts['top-level symbols']+parts['rule bodies'])/8 + Hf_nolab/8 + lab_refs*19/8 + fn*19/8 + parts['skeleton table (est. 32 b/skeleton)']/8
print('tweak1+2+3 direct jump offsets: %.0f B ratio %.3f / linked %.3f / text+rela %.3f'%(t3,t3/D,t3/DL,t3/41212))
# Huffman dictionaries: distinct values * (raw width + 5b code length), non-label slots
dict_bits=sum(len(slot_vals[s])*(g['raw_width'](s)+5) for s in cnt if s not in lab_slots and len(slot_vals[s])>1)
print('Huffman dict cost (est) %d B; + symbol address table est %d B'%(dict_bits/8, g['symaddr_table']/8))
t4=t3+dict_bits/8
print('t3 + dicts: %.0f B ratio %.3f / linked %.3f / text+rela %.3f'%(t4,t4/D,t4/DL,t4/41212))
# order-0 Huffman on grammar symbols too (top-level only; rule bodies fixed)
# reconstruct final from repair at chosen stop is costly; approximate: entropy of skeleton ids at R=0
