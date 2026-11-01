# K2 results: memo-table transfer in 1-D HashLife

Every number below is **measured** by `run.sh` on this VM (gcc 13.3 -O2, about 9 min wall time). Raw output is in `results/`.
Lines marked *estimated* or *interpretation* are not measurements.

## Machine
- `k2.c` is a 1-D HashLife for any elementary CA rule.
  - Nodes are hash-consed pairs. Ids 0 and 1 are the two cells, and a level-k node holds 2^k cells.
  - `result(n,k)` returns the centre 2^(k-1) cells after 2^(k-2) steps. It uses 5 level-(k-1) sub-results, as in Gosper's
    construction, and is memoised in a separate result table: the "memo".
  - To evolve 2^T steps, the code builds a level-(T+2) root with the patch at its centre and the background everywhere else.
    The root's result is exact on its centre 2^(T+1) cells.
- Rule 110 background: the ether `00010011011111`. It has spatial period 14, and each step is a left shift by 4 cells,
  so it repeats after 7 steps (checked by enumeration).
- Rule 30 background: all zeros.
- Patches are i.i.d. fair bits from splitmix64. A uses seed 1000, and B_i uses seed 1000+i (i = 1..20). Default patch size P = 256.
- Transfer protocol:
  - Build A, then freeze it by recording watermarks on the node and memo tables.
  - Evolve each B_i against the frozen tables. Each lookup is counted as a frozen hit, a hit on an entry B_i itself added
    ("own hit"), or a miss, which creates a new entry.
  - After each B_i, the tables are truncated back to the freeze point, so every B_i sees only A's memo.
  - Each B_i is also built from scratch in empty tables.
- Hit rate = frozen hits / all memo lookups during B_i. New/scratch = new memo entries / from-scratch memo entries.

## Correctness (measured, `results/validate.txt`)
- HashLife was compared with direct bit-sliced stepping over the full root width. There are 17 runs, all with 0 mismatches:
  - rule 110 at 2^12 steps, 5 patches;
  - rule 30 at 2^12 steps, 5 patches;
  - rule 110 at 2^16 steps, 2 patches with P=1024;
  - rule 30 at 2^16 steps, 1 patch;
  - pure-background runs for each case.
- The rule 110 ether is stationary under the implementation: a patch-free root differs from the expected ether phase in
  0 of 8,192 cells (2^12 steps) and 0 of 131,072 cells (2^16 steps). The ether needs only 88 memo entries at 2^12 steps
  and 116 at 2^16.

## Main result: rule 110, 2^24 steps, P=256, 20 unseen B (measured)
| quantity | value |
|---|---|
| frozen memo of A | 6,678 entries (80 KB) + 6,690 nodes (80 KB), at 12 B per entry |
| transfer hit rate, mean (min) | **0.558** (0.489) |
| hit rate on levels ≥6 only | 0.374 |
| new entries / from-scratch, mean (max) | **0.818** (0.888) |
| from-scratch entries per B | 4,030 – 15,167 |
| memo cap 2^24 | not approached |

Per-level frozen hit rate, all B combined:
- 1.00 at level 2 and 0.998 at level 3, where all 16 and 256 possible nodes occur;
- 0.64 at level 4;
- 0.33–0.41 from level 5 up to level 24.

*Interpretation:* the high overall rate comes from the small levels, where every possible node occurs in any run.

Sensitivity (measured, 20 B each; `results/summary.txt`):
| run | hit rate | levels ≥6 | new/scratch |
|---|---|---|---|
| T=24, P=64 | 0.522 | 0.413 | 0.791 |
| T=24, P=256 | 0.558 | 0.374 | 0.818 |
| T=24, P=1024 | 0.616 | 0.343 | 0.762 |
| T=24, P=4096 | 0.641 | 0.373 | 0.745 |
| T=16, P=256 | 0.601 | 0.354 | 0.775 |
| T=20, P=256 | 0.552 | 0.329 | 0.815 |

## Control: rule 30 on zeros (measured)
- **2^24 steps:** building A from scratch exceeded the 2^24-entry memo cap, and so did every B_i (21 of 21 runs, aborted
  at 16,777,216 entries). The transfer step was not run.
- **2^15 steps:** the largest horizon tried that fits under the cap (2^16 needed 28.2 M entries in validation).
  - A's memo: 7.14 M entries (85.7 MB) + 7.14 M nodes (85.7 MB).
  - Hit rate: mean 0.750, but only 0.0018 on levels ≥6.
  - New/scratch: 0.990.
  - Level 4 hits 100%, because all 65,536 16-cell nodes occur. Level 5 and above hit 0.1–25%.
- At 2^12 steps the hit rate is 0.744 and new/scratch is 0.646. At 2^14 steps they are 0.749 and 0.964. These two were
  measured during development and are not in `run.sh`.

## Decision rule
The rule requires a hit rate ≥ 90% and new entries ≤ 10% of from-scratch. Rule 110 measures 56% and 82%, and no
configuration comes within reach of either threshold. Rule 30 fails both too, and it also exceeds the cap at 2^24.
**Verdict: the memo is input-specific. On this test, compile = execute adds nothing beyond Gosper.**

*Interpretation:* what transfers is the background and its small, saturated windows. The patch's
transient is new for every B, and that is where about 80% of B's work lies. Rule 110's structure (gliders over the ether)
compresses each run well, at about 6 K entries for 2^24 steps, but the compressed program differs per input.

*Estimated:* none of the numbers above are estimated.

## Caveats
- The hit rate counts every memo lookup at every level, as the plan defines it. A hit stops the recursion, so frozen
  hits also shrink B's total lookups. The levels ≥6 column is the less flattering view.
- A is a single random patch. A memo built from many A's was not tested, because the plan does not call for it.

## Independent verification (2026-11-01)

Confirmed. A reduced rerun reproduces (hit 0.557, new/scratch 0.823), and an independent Python stepper (`verify/direct.py`) matches HashLife at 2^13 and 2^16 steps. The overall hit rate is pinned near 55% by trivially-hitting low levels; **new/scratch is the honest measure**. A memo built from many patches (`verify/k2m.c`):

| A patches | memo entries | hit | levels ≥6 | new/scratch |
|---|---|---|---|---|
| 1 | 6.7K | 0.557 | 0.37 | 0.82 |
| 10 | 54K | 0.559 | 0.40 | 0.64 |
| 100 | 399K | 0.551 | 0.44 | 0.50 |
| 1000 | 3.2M | 0.548 | 0.46 | 0.42 |

Transfer improves slowly while the memo grows by ~3.2K entries per patch, so the verdict stands: the memo is input-specific.
