# E3 results: search instead of derivation

Reproduce with `./run.sh` (builds `e3.c` with gcc 13.3 `-O3 -march=native`, writes `expressions.tsv`, `results.tsv`, `summary.md`). Ran on an 8-vCPU AMD EPYC 9554P exe.dev VM, 8 jobs in parallel, about 25 s wall-clock.

## Setup
- 200 expressions from splitmix64 seed `0xE3E3E3E3`. Each has 1-4 binary ops (uniform) drawn uniformly from + - * & | ^, with a random tree shape. A leaf is x, y, z or a constant, each with probability 1/4; constants are uniform over 0..15. All arithmetic is mod 16.
- Derived program: a plain postfix tree walk with no folding, so its length is 2*ops+1.
- The VM has PUSHX/Y/Z, PUSHC k, ADD SUB MUL AND OR XOR, DUP SWAP OVER, each of length 1. A program's result is the single value left on the stack.
- The search is breadth-first by length. A state is the stack of 4096-entry value vectors, deduplicated by a 128-bit hash of the whole stack. It prunes states whose stack depth can't be reduced to one value within the length bound.
- Lengths 1..L-1 are searched (L = derived length). If nothing is found, the shortest is L, and the derived program is the verified witness. Every program is checked exhaustively over all 4096 inputs by a separate scalar interpreter. The time cap was 600 CPU-s per expression.

## Numbers (all measured)
- Timeouts (failures): **0/200**.
- **Mean length ratio (search/derived): 0.768**. Median: **1.000**.
- Search was strictly shorter for **83/200**. Only **1** of those 83 uses DUP/SWAP/OVER.
- Mean length: derived 5.77, search 4.28. Histogram of search lengths: 1:36, 3:68, 5:47, 7:30, 9:19.
- Search CPU: **177.1 s total**. Per expression: mean 0.885 s, median 0.002 s, max 7.95 s (on 9-instruction expressions, which must be searched to length 8).
- Peak RSS: **104 MiB** (the largest single per-expression process).
- States: 40.7 M distinct states stored in total, at most 1.51 M for one expression. 186 M child states examined.

| derived len | n | mean search len | mean ratio | strictly shorter | mean CPU s |
|---|---|---|---|---|---|
| 3 | 64 | 2.50 | 0.833 | 16 | 0.000 |
| 5 | 42 | 4.19 | 0.838 | 13 | 0.003 |
| 7 | 47 | 4.91 | 0.702 | 26 | 0.070 |
| 9 | 47 | 6.15 | 0.683 | 28 | 3.695 |

## Striking examples
1. #184 `(((6 * (z + y)) + 5) * 8)`
   derived (9): `PUSHC 6; PUSHZ; PUSHY; ADD; MUL; PUSHC 5; ADD; PUSHC 8; MUL`
   found (1): `PUSHC 8`. 6*(..) is even, so +5 makes it odd, and 8*odd = 8 mod 16.
2. #126 `(x ^ (z - (x & (z | z))))`
   derived (9): `PUSHX; PUSHZ; PUSHX; PUSHZ; PUSHZ; OR; AND; SUB; XOR`
   found (3): `PUSHX; PUSHZ; OR`. Since z - (x&z) = z & ~x, x ^ (z & ~x) = x | z.
3. #74 `((x ^ y) * ((x ^ y) & 13))`
   derived (9): `PUSHX; PUSHY; XOR; PUSHX; PUSHY; XOR; PUSHC 13; AND; MUL`
   found (7): `PUSHX; PUSHY; XOR; PUSHC 13; OVER; AND; MUL`. This is the only case where the saving comes from sharing a subexpression on the stack.

## Decision
The mean ratio is 0.768, above 0.75. **The rule is not met: at this scale search buys little over derivation.**

## Caveats
- The result is close to the threshold, and it depends on the generator. With only 3 variables and 1/4 constant leaves, many random trees contain degenerate subterms such as x^x, z-z or c op c. Almost all of the savings are algebraic simplification or constant results: 36/200 programs collapse to one instruction. Only 1/83 savings comes from stack sharing.
- Estimated, not measured: a derivation pass with constant folding plus x-x / x^x / x&x identities would recover much of the gap. This was not run.
- Search cost grows about 50x for each 2 instructions of derived length: mean 0.07 s at L=7 and 3.7 s at L=9. Expressions with 5+ ops were outside this experiment's scope.
