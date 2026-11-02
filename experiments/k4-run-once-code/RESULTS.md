# K4 (R1): Run-once generated code on a data-dependent program

**Result: just inside the rule's 3× line against the strongest baseline. Generation is the whole gap.**
- Measured: the best run-once variant, lane-interleaved (`jitL`, 64 KiB ring), uses **2.34–2.37× the cycles of the best table DFA** (`tableB`, 8 lanes), generation included.
  - The first-round single-chain run-once code (`jit`) is 3.9–4.1× `tableB`, which is past the 3× line.
  - Both were 1.2× the single-chain table, which turned out to be a weak baseline.
- Measured: generation alone costs **1.9–2.1 cycles per input byte** (0.18–0.20 ns per generated instruction, 0.54–0.59 ns per input byte). That is more than the whole of `tableB` (1.62 cyc/B).
  - Estimated by subtraction: the generated code itself runs at 1.83–1.87 cyc/B, against `tableB`'s 1.62.
- Measured: chunks under 64 instructions fail (6.6–145× `tableB`).
- The specialization folds only the input byte into the code. The DFA state stays a runtime value, so run-once code does the table's work minus one load, plus the cost of emitting it. This DFA is not a workload where specialization removes work.

Reproduce: `sh run.sh`. It writes `results/raw.txt` (every run), `results/summary.txt`, `results/selftest.txt` and `results/load.txt`. Defaults are 5 reps, CPU 6 and a 1 GiB input; override with `REPS`, `CPU`, `G` (input MiB) and `CM` (MiB for the failure curve).

## Setup
- **DFA**: an Aho-Corasick automaton for `.*(w1|w2|…)`, an unanchored search for 63 literals: C and Lua keywords plus common identifiers.
  - The list is cut so that |Q| = 256 exactly; the last word, `strcmp`, is truncated. A = 70 of the states are accepting.
  - Output: the final state, and the number of input positions after which the state is accepting.
  - `k4 selftest` checks the automaton against a naive matcher on a 1 MiB prefix of each input. Both pass.
- **Inputs (1 GiB each)**:
  - `seeded`: text-like, from splitmix(1). 35% of tokens are keywords; the rest are random lowercase words and separators.
  - `text`: Lua 5.4.6 `src/*.[ch]` plus chibicc 90d1f7f `*.[ch]` (71 files, 1.1 MB), repeated.
- **table**: a single-chain tight loop, `s = T[in[i]<<8 | s]; cnt += s < A`. It is bound by load latency, about 5 cyc/B.
- **tableB** (the strongest baseline, from the verifier):
  - Each 32 KiB block is split into 8 lanes of 4 KiB, giving 8 independent chains.
  - Lanes 1–7 start at the root 32 bytes early. This is exact because an Aho-Corasick state depends only on the last ≤ maxdepth (9) bytes; `main()` checks that maxdepth ≤ 32.
  - Lane 0 continues the previous block's lane 7.
- **interp**: a switch interpreter over per-chunk bytecode (`STEP b`, `ACC`).
- **jit** (single chain):
  - For each chunk of n input bytes, the generator emits `movzx eax, byte [rax + T+256*b]; cmp al, A; adc ecx, 0` per byte into an RWX ring slot, calls it once, and overwrites the slot later. That is 12 code bytes per input byte.
  - The chunk size is the largest that fills the ring: n = 340, 1364 or 5460 bytes for 4, 16 and 64 KiB rings.
- **jitL** (lane-interleaved):
  - It uses the same 8-lane split as `tableB`. Each chunk is c steps; a step advances all 8 lanes by one byte, interleaved.
  - Per lane byte it emits `movzx rL, byte [rL + T+256*b]; cmp rLb, A; adc cnt(L mod 4), 0`. Warm-up steps get the movzx only.
  - Lane states live in r8–r11, r13–r15 and rbx within a chunk, and in a context struct between chunks.
  - 14.8–15.2 code bytes per input byte; c = 32, 135 or 544 steps for 4, 16 and 64 KiB rings.
  - The generator copies a 16-byte per-(lane, byte) template, with unrolled constant lengths.
- **gen / genL**: the respective generator alone. It emits into the ring and never executes.
- **Measurement**:
  - perf 6.8, user mode only, `taskset -c 6`.
  - Counters cover only the scan, bracketed by the program using `--control fifo`, so input construction is excluded.
  - Every figure is the median of 5 runs.
  - **This VM's PMU reads 0 on the 6th programmed counter**, checked by reordering events. Each config therefore runs twice per rep: group A is cycles, instructions, `ic_cache_fill_l2`, `ic_cache_fill_sys` and `ls_any_fills_from_sys.dram_io_all`; group B is cycles, `dram_io_all` and `l2_pf_miss_l2_l3.all`.
- **DRAM bytes**: 64 × (`dram_io_all` + `l2_pf_miss_l2_l3.all`), from group B. This counts reads only; writes are not counted.

## Noise
- Load average: 0.42 at the start and 1.67 at the end (an 8-vCPU machine shared with other experiments; the first round ran at load 11.5).
- Control: a fixed chain of 2^28 splitmix steps, measured 10 times.
  - Wall time varied by **81%**; user cycles varied by **0.3%**.
  - Wall time is therefore unusable for comparisons here, and **every comparison uses user cycles**.

## Measured (medians of 5; 1 GiB; cycles per input byte)

| mode | ring | cyc/B seeded | cyc/B text | instr/B | IPC | icache fills from L2 per KiB | DRAM read B/B | code B/B | × tableB (seeded / text) |
|---|---|---|---|---|---|---|---|---|---|
| table (1 chain) | – | 5.07 | 5.09 | 7.00 | 1.38 | 0.0 | 1.02 | 0 | 3.11 / 3.14 |
| **tableB (8 lanes)** | – | **1.63** | **1.62** | 7.03 | 4.33 | 0.0 | 1.07 | 0 | **1.00** |
| interp | – | 7.26 | 7.31 | 26.0 | 3.6 | 0.0 | 1.16 | 0 | 4.46 / 4.51 |
| jit | 4 KiB | 6.27 | 6.38 | 12.15 | 1.9 | 192.8 | 1.11 | 12.04 | 3.85 / 3.94 |
| jit | 16 KiB | 6.55 | 6.61 | 12.04 | 1.8 | 192.3 | 1.16 | 12.01 | 4.02 / 4.08 |
| jit | 64 KiB | 6.36 | 6.41 | 12.01 | 1.9 | 192.8 | 1.16 | 12.00 | 3.91 / 3.95 |
| jitL | 4 KiB | 4.69 | 4.47 | 10.26 | 2.2 | 245.0 | 1.03 | 15.19 | 2.88 / 2.76 |
| jitL | 16 KiB | 4.25 | 4.27 | 9.15 | 2.1 | 238.9 | 1.03 | 14.90 | 2.61 / 2.64 |
| **jitL** | **64 KiB** | **3.86** | **3.80** | 8.89 | 2.3 | 239.8 | 1.04 | 14.83 | **2.37 / 2.34** |
| gen only | 4/16/64 KiB | 2.08 / 2.05 / 2.07 | 2.09 / 2.04 / 2.07 | ~9.0 | 4.4 | 0 | 1.0–1.15 | 12 | – |
| genL only | 4/16/64 KiB | 2.05 / 1.94 / 1.99 | 2.00 / 1.92 / 1.97 | 5.9–7.1 | 3.0–3.5 | 0 | 1.05–1.06 | 14.8–15.2 | – |

- All modes agree on both inputs:
  - seeded: final state 70, 77,833,848 matches;
  - text: final state 70, 47,107,915 matches.
  - The failure-curve runs agree too.
- **Generation cost (measured)**:
  - jit: 0.58–0.59 ns per input byte, 0.193–0.197 ns or 0.68–0.69 cycles per generated instruction, 3.0 instructions per byte.
  - jitL: 0.54–0.57 ns per input byte, 0.178–0.186 ns or 0.63–0.66 cycles per generated instruction, 3.0–3.1 instructions per byte (including warm-up and per-chunk overhead).
- **Against R1's microbenchmark (1.42–1.61 ns per generated instruction, about 1.3× native)**:
  - Per instruction, generation here is about 8× cheaper (R1 computed a splitmix per instruction).
  - End to end, however, run-once is 2.3× the best native loop, not 1.3×. The native lane loop needs only about 1.6 cyc/B, and emitting 15 code bytes per input byte costs more than that by itself.
- **Estimated by subtraction (jit − gen)**:
  - jitL executes at 1.83–1.87 cyc/B (64 KiB ring) against `tableB`'s 1.62. Interleaving recovers the memory-level parallelism, but fetching 15 bytes of fresh code per input byte (about 240 icache fills from L2 per KiB of input) costs roughly 0.2 cyc/B more than the table.
  - Single-chain jit executes at 4.2–4.6 cyc/B, latency-bound like `table`.
- **Code fetch**: icache fills from L2 match code bytes ÷ 64. `ic_cache_fill_sys` stays under about 100 per MiB of input, essentially no code from DRAM. DRAM reads are 1.0–1.16 B per input byte in every mode (the input, once). Write-backs are not counted; the ring of 64 KiB or less stays in L2 (estimated).

## Failure curve (measured; single-chain jit, 128 MiB seeded, 64 KiB ring, chunk = n bytes = 3n+5 instructions)
Reference: `table` 5.06 cyc/B; `tableB` 1.61 cyc/B.

| n | instr/chunk | jit cyc/B | × tableB | × table | ns per chunk |
|---|---|---|---|---|---|
| 1 | 8 | 234 | 145 | 46 | 66 |
| 2 | 11 | 139 | 86 | 27 | 77 |
| 4 | 17 | 81 | 50 | 16 | 90 |
| 8 | 29 | 33.5 | 21 | 6.6 | 76 |
| 16 | 53 | 13.0 | 8.1 | 2.6 | 58 |
| 19 | 62 | 10.6 | 6.6 | 2.1 | 56 |
| 32 | 101 | 7.8 | 4.9 | 1.5 | 71 |
| 64 | 197 | 6.9 | 4.3 | 1.4 | 123 |
| ≥256 | ≥773 | 6.4–6.6 | 4.0–4.1 | 1.3 | – |

- Below 64 instructions per chunk, each write-then-call costs about 55–90 ns, against R1's 40–50 ns. Against `tableB` that is 6.6–145×, beyond the 10× line from n ≤ 8, so the tiny-chunk workload **fails**.
- The curve flattens at about 100 instructions per chunk.

## Rule (against the strongest baseline)
- **Decision**: the best run-once code (`jitL`) is 2.34–2.37× `tableB` in cycles, which is ≤ 3×, so the rule's "viable" branch technically holds. The margin is thin: the 4 KiB ring is 2.76–2.88×, and single-chain jit is 3.9–4.1×, in the 3–10× zone.
- **What the model reveals**:
  - The cost is generation. About 2 cyc/B of emit work replaces one 1-cycle-throughput load per byte. Execution alone is estimated at about 1.15× `tableB`.
  - Generation cannot resolve transitions, because the state is the previous chunk's runtime result. Resolving them would mean the generator runs the DFA, and the code collapses to a constant (reasoned, not measured).
  - So for this program, data-specialized run-once code is the table loop with the input load removed and an emit pass added. It cannot win.
- **Next step under the rule**: a workload where the data fixes control flow that the table version must still compute at runtime. Examples are per-chunk query plans or predicates with data-dependent branches. The DFA is not one.

## Files
- `k4.c`: the DFA, inputs, the table, tableB, interp, jit, gen, jitL, genL, control and selftest modes, and perf control.
- `run.sh`: the full reproduction.
- `summarize.py`: medians, agreement, derived metrics and the rule line.
- `results/`: the outputs listed above.
