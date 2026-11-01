# K3: Residency on a butterfly DAG that cannot be fused

Reproduce: `./run.sh` (about 1 min). It writes `summary.txt` and `perf.csv`; the numbers below come from the run recorded there. Machine: AMD EPYC 9554P (exe.dev VM, 8 vCPUs, L2 1 MiB per core shared by 2 vCPUs, L3 32 MiB), Linux 6.12.113, gcc 13.3.0 `-O3 -march=native`.

**Result.**
- On computed bytes the rule passes trivially. The model moves 0 bytes past L2, against 128 MiB for the blocked schedule.
- On the corrected counter the rule fails: blocked 75.4 MB vs model 15.5 MB, which is **4.9×** (≥100× needed).
- The model's 15.5 MB is the VM's noise floor. A control that is L2-resident and has no recomputation reads 16.8 MB in the same number of cycles. With the floor subtracted, the model's residual cannot be told apart from zero, so the ratio cannot be resolved on this VM.
- The cost is a 28.4× recomputation factor (butterflies), 46× the instructions and 25× the cycles.
- The mechanism is Savage's red-pebble time–space tradeoff for the FFT graph (recompute ∝ n/S). It is a known lower-bound regime, not a new one.

## Workload (k3.c)
- n = 2^24 u32 elements. Leaf i is the top 32 bits of splitmix64(i).
- Stage s (0..23) pairs i with i|2^s and computes `a' = a+b`, `b' = rotl(a−b, s%31+1)` mod 2^32.
- Output: H = Σ mix64((i<<32) | out_i) mod 2^64. The sum is order-independent, so every schedule can emit outputs in its own order.
- All schedules use the same stage kernel. Stages whose pair distance fits a 16 KiB L1 sub-block run sub-block by sub-block.

## Implementations
- **(a) mat:** generate leaves into a 64 MiB array, then make 24 full in-place passes, then a hash pass.
- **(b) blocked**, a four-step schedule with log n / log B = 2 passes, the minimum at a 512 KiB block. Both passes are fused at their ends:
  - Pass 1 generates the leaves of each contiguous 2^17-element (512 KiB) block in an L2 buffer and runs stages 0–16. It writes each block out once with non-temporal stores.
  - Pass 2 gathers groups of 1024 columns × 128 rows (4 KiB contiguous runs) into the L2 buffer, runs stages 17–23 and hashes there. Nothing is written back.
  - Compulsory traffic is one spill round trip: 64 MiB written plus 64 MiB read. There is no input read and no output write.
  - `blocked-rs` is the same with regular stores, which adds RFOs but lets L3 keep part of the data. Both variants are measured and the better one is used.
- **(c) model:** the only storage is one 1 MiB arena (the budget). Nothing is ever spilled.
  - The output is produced in chunks of c columns. For each chunk, every one of the R = 2^(24−k) level-k blocks is regenerated from seeds and run through stages 0..k−1, and only the chunk's c columns are kept.
  - The remaining 24−k stages then run on the resident R×c chunk, which is hashed.
  - The chunk width is c = ⌊(2^18 − 2^k)/R⌋. A sweep over k = 7..14 minimises butterflies at k = 9: c = 7, 74 chunks.

All five schedules (mat, blocked, blocked-rs, model, and a cross-check of model over k = 7..13) produce the same hash at log2n = 10, 12, …, 24. At log2n = 24 the hash is `721447264319c607`.

## Calibration (measured)
DRAM bytes = 64 × (`ls_any_fills_from_sys.dram_io_all` + `l2_pf_miss_l2_l3.all`), user mode only, median of 5 runs.
- A 1 GiB read sweep (after subtracting a memset-only run) gives **1.086 GiB**, so the counter is within +9% for streaming reads.
- A 1 GiB non-temporal write gives 0.087 GB. **Writes are essentially invisible** to these events, so measured bytes are reads only, and computed bytes cover the writes.

## Numbers, log2n = 24 (n = 64 MiB of u32)
| | DRAM measured (reads) | computed DRAM bytes R+W | instructions | cycles | butterflies | recompute |
|---|---|---|---|---|---|---|
| mat | 1,495 MB | 3,355 MB (1,678 R + 1,678 W) | 0.264 G | 0.306 G | 201 M | 1.00 |
| blocked (NT) | **75.4 MB** | 134 MB (67 R + 67 W) | 0.274 G | 0.157 G | 201 M | 1.00 |
| blocked-rs | 77.7 MB | ≤201 MB (RFO upper bound) | 0.269 G | 0.155 G | 201 M | 1.00 |
| **model** | **15.5 MB** | **0** | 12.59 G | 3.88 G | 5,713 M | **28.38** |
| resident control | 18.9 MB | 0 | 12.77 G | 4.36 G | 11,796 M | n/a |

- **Model recomputation factor:** 28.4× in butterflies and 74× in leaves (1.24 G splitmix calls for 16.8 M leaves).
- **Resident control:** `k3 resident 0 5000` runs the same kernel 5000 times over the same 1 MiB arena, roughly cycle-matched to the model. Its rate is 4.34 MB/Gcycle; the model's is 3.99 MB/Gcycle.
- **Floor scaled to the model's cycles (estimate):** 16.8 MB, so model minus floor is about 0. The VM's load average was about 8 because other experiments ran on sibling vCPUs that share L2 and L3. Even a separate 256 KiB L2-resident loop, checked by hand, recorded 1.2–3.2 MB/Gcycle.
- **Blocked:** the measured 75 MB is close to its computed 67 MB of reads. Its 64 MiB of NT writes are not seen.

**Sanity point, log2n = 16 (256 KiB, fits L2), measured:** all four schedules read 20–52 KB, which is noise. All have 1.00 recomputation (the model takes k = 9 with c covering every column in one chunk), and instructions are about 1.0 M each.

## Rule
- **Measured:** 75.4 MB / 15.5 MB = **4.9× (fail)**. This ratio is bounded by the environmental floor, not by the model. Any ~4 s run on this VM reads about 15 MB, so even a perfect model cannot show 100× against a 75 MB baseline.
- **Computed:** 134 MB / 0 B. This passes, and is true by construction.
- **Honest reading:** the model's only DRAM traffic is the floor that any L2-resident computation shows, so "no spill" holds as far as the counter can resolve. Whether this beats the baseline 100× depends on which of the two numbers the plan accepts. I do not think it moves a known lower bound:
  - Hong–Kung's bound assumes values may be spilled. Pebbling without spills, with regenerable sources, is the red-pebble game.
  - For the FFT graph that game's tradeoff (Savage) predicts recomputation ∝ n/S. That is what is observed here: 74× leaves, n/S = 64.

## Where it fails (estimates from the counted work)
- **Non-regenerable leaves:** if leaves must be read from DRAM, the model re-reads the input once per chunk, 74 × 64 MiB ≈ 5 GB, against the blocked schedule's 128 MB. That is about 37× *more* traffic.
- **Scaling:** recomputation grows like n/B. At n = 2^30 with B = 1 MiB that is about 4096× the leaves. The blocked schedule's traffic grows only like n·log n / log B, about 2–3 passes. Instructions become prohibitive long before bytes matter.
