# K4 (R1): Run-once generated code on a data-dependent program

**Result: the rule's "viable" branch, with a caveat about what was specialized.**
- Measured: data-specialized run-once code uses **1.17–1.24× the table-driven DFA's cycles**, generation included. That is within 3×.
- Measured: generation costs **0.15–0.17 ns per generated instruction** and **0.45–0.50 ns per input byte**.
- Measured: chunks under 64 instructions fail badly, at 2.1–47× the table's cycles.
- Caveat: the specialization can only fold the input byte into the code. The DFA state stays a runtime value, so the code does exactly the table's work minus one load. The rule's next step is a workload where specialization removes work the table must do. As explained below, this DFA is not one.

Reproduce: `sh run.sh`. It writes `results/raw.txt` (every run), `results/summary.txt`, `results/selftest.txt` and `results/load.txt`. Defaults are 5 reps, CPU 5 and a 1 GiB input; override with `REPS`, `CPU`, `G` (input in MiB) and `CM` (MiB for the failure curve).

## Setup
- **DFA**: an Aho-Corasick automaton for `.*(w1|w2|…)`, an unanchored search for 63 literals: C and Lua keywords plus common identifiers.
  - The list is cut so that |Q| = 256 exactly; the last word, `strcmp`, is truncated. A = 70 of the states are accepting.
  - Output: the final state, and the number of input positions after which the state is accepting.
  - `k4 selftest` checks the automaton against a naive all-patterns matcher on a 1 MiB prefix of each input. Both pass.
- **Inputs (1 GiB each)**:
  - `seeded`: text-like, from splitmix(1). 35% of tokens are keywords; the rest are random lowercase words and separators.
  - `text`: Lua 5.4.6 `src/*.[ch]` plus chibicc 90d1f7f `*.[ch]` (71 files, 1.1 MB), repeated to 1 GiB.
- **table** (baseline): a tight loop, unrolled 8×, `s = T[in[i]<<8 | s]; cnt += s < A`. The table is transposed so that the input byte selects a 256-byte column.
- **interp**: a switch interpreter over per-chunk bytecode (`STEP b`, `ACC`). This is the same specialized program, interpreted.
- **jit**:
  - For each chunk of n input bytes, the generator emits straight-line x86 into an RWX ring, calls it once, and moves to the next slot, overwriting old code.
  - Per input byte it emits `movzx eax, byte [rax + T+256*b]` (the byte is an immediate inside the displacement), `cmp al, A`, `adc ecx, 0`. That is 3 instructions and 12 code bytes.
  - Each chunk adds a 5-instruction prologue and epilogue.
  - The chunk size is the largest that fills the ring: n = 340, 1364 or 5460 bytes for 4, 16 and 64 KiB rings.
- **gen**: the jit generator alone. It emits into the ring and never executes.
- **Measurement**:
  - perf 6.8: `/usr/lib/linux-tools-6.8.0-147/perf stat`, user mode only, `taskset -c 5`.
  - Counters cover only the scan. The program brackets that region using `--control fifo`, so input construction is excluded.
  - Every figure is the median of 5 runs.
  - Each config is run twice per rep, with different events. **This VM's PMU reads 0 on the 6th programmed counter** (checked by reordering events), so at most 5 events go in one run.
- **DRAM bytes**: 64 × (`ls_any_fills_from_sys.dram_io_all` + `l2_pf_miss_l2_l3.all`). This counts reads only; writes are not counted. `l2_pf_miss_l2_l3.all` reads correctly only when it is not the 6th event.
- **Zen4 event names used**: `ic_cache_fill_l2`, `ic_cache_fill_sys`, `ls_any_fills_from_sys.dram_io_all` and `l2_pf_miss_l2_l3.all`.

## Noise
- Load average was 11.5 at the start of the run and 1.1 at the end; the 8-vCPU machine was shared with other experiments.
- Control: a fixed chain of 2^28 splitmix steps, measured once per group per rep (10 runs).
  - Wall time varied by **20%**; user cycles varied by **0.8%**.
  - Wall time between modes is therefore unreliable here, and **all comparisons below use user cycles**. Per-config ns ranges are in `summary.txt`.

## Measured (medians of 5; 1 GiB; cycles per input byte)

| mode | ring | cyc/B seeded | cyc/B text | instr/B | IPC | icache fills from L2 per KiB | DRAM read B/B | code B/B | × table cycles (seeded / text) |
|---|---|---|---|---|---|---|---|---|---|
| table | – | 5.15 | 5.09 | 7.00 | 1.37 | 0.0 | 1.02 | 0 | 1.00 / 1.00 |
| interp | – | 7.01 | 6.90 | 26.0 | 3.7 | 0.0 | 1.18 | 0 | 1.36 / 1.35 |
| jit | 4 KiB | 6.03 | 6.16 | 12.16 | 2.0 | 192.8 | 1.12 | 12.04 | **1.17 / 1.21** |
| jit | 16 KiB | 6.21 | 6.31 | 12.04 | 1.9 | 192.3 | 1.18 | 12.01 | **1.21 / 1.24** |
| jit | 64 KiB | 6.09 | 6.06 | 12.01 | 2.0 | 192.8 | 1.18 | 12.00 | **1.18 / 1.19** |
| gen only | 4/16/64 KiB | 1.63 / 1.58 / 1.79 | 1.60 / 1.59 / 1.76 | ~9.0 | 5.0–5.7 | 0 | 1.01–1.17 | 12 | – |

- All modes agree on both inputs:
  - seeded: final state 70, 77,833,848 matches;
  - text: final state 70, 47,107,915 matches.
- **Generation cost (measured, gen mode)**:
  - 0.45–0.50 ns per input byte;
  - 0.148–0.168 ns per generated instruction, or 0.53–0.60 cycles;
  - 3.0 generated instructions and 12.0 code bytes per input byte.
- **Comparison with R1's microbenchmark (1.42–1.61 ns per generated instruction, about 1.3× native)**:
  - Per instruction, generation here is about 10× cheaper. R1 computed a splitmix per emitted instruction; here each byte costs one 16-byte template copy, so the two figures are not the same kind of quantity.
  - The end-to-end ratio is similar: 1.17–1.24× here against R1's 1.3×.
- **Code fetch**:
  - `ic_cache_fill_l2` is about 192.8 per KiB of input, which matches 12 code bytes per input byte ÷ 64 B per line. Every generated line is fetched once, from L2.
  - `ic_cache_fill_sys` is 19–105 per MiB of input against 8–11 for table. That is at most 0.15% of code lines, so essentially no code comes from DRAM, consistent with "no code bytes fetched from DRAM".
- **Estimated by subtraction (jit − gen)**: the generated code executes at 4.3–4.7 cyc/B, against the table's 5.1. The run-once code itself is slightly cheaper than the table loop, because the input load leaves the loop. The total overhead is generation, at about 0.55 cyc per generated instruction.
- **DRAM**: every mode reads the input once (1.0–1.2 B per byte). The ring and the 64 KiB table stay on-chip. Write-backs of the 12 GiB of generated code are not counted, but a ring of 64 KiB or less never leaves L2, so none are expected (estimated, not measured).

## Failure curve (measured; 128 MiB seeded, 64 KiB ring, chunk = n bytes = 3n+5 instructions)

| n | instr/chunk | jit cyc/B | × table | ns per chunk | icache fills from L2 per KiB |
|---|---|---|---|---|---|
| 1 | 8 | 239 | 47.1 | 67 | 3034 |
| 2 | 11 | 124 | 24.4 | 69 | 1564 |
| 4 | 17 | 78 | 15.4 | 91 | 919 |
| 8 | 29 | 42 | 8.3 | 107 | 697 |
| 16 | 53 | 12.9 | 2.55 | 59 | 261 |
| 19 | 62 | 10.8 | 2.12 | 56 | 242 |
| 32 | 101 | 7.6 | 1.49 | 72 | 225 |
| 64 | 197 | 7.0 | 1.37 | 131 | 208 |
| ≥256 | ≥773 | 6.2–6.3 | 1.23–1.24 | – | 193–196 |

- Below 64 instructions per chunk, each write-then-call costs about 55–110 ns (60–110 ns in the measured n ≤ 19 rows). Compare R1's 40–50 ns and the table's 0.4 ns per byte.
- At n ≤ 8 (29 instructions) the model is 8–47× worse than the table. That puts the tiny-chunk workload beyond the rule's 10× line, so **it fails**.
- The curve flattens at about 100 instructions per chunk. Ring size, at 4, 16 or 64 KiB, makes no measurable difference at full chunks.

## Rule
- **Judgement**: run-once data-specialized code needs 1.17–1.24× the table DFA's cycles, which is ≤3×. By the rule, "fusing data into throwaway code" is a viable execution model, as long as chunks are ≳100 instructions. Below 64 instructions it is not.
- **What the model reveals: the specialization removes almost no work for this program.**
  - The generator can fold the byte into the instruction, which removes the input load. It cannot resolve the transition, because the state at a chunk's start is the previous chunk's runtime result.
  - "Resolving transitions at generation time" in full would mean the generator tracks the state. Then it is running the DFA itself, and the emitted code collapses to a constant.
  - In between, there is constant-column propagation: the generator emits `mov eax, imm` for a byte that sends every state to one target, and then knows the state from there on. For an Aho-Corasick automaton on text, separators reset the state, so this collapses to the same thing. This is reasoned, not measured.
  - So the gap is small *because* the generated program is the table loop with the load removed.
- **Next step under the rule**: look for a workload where the data fixes control flow that the table version must still compute at runtime. Two examples:
  - a bytecode or query plan with data-dependent branches that is fixed per chunk;
  - a filter whose predicate constants come from the data.
  A DFA over its own input is not that workload.

## Files
- `k4.c`: the DFA build, inputs, the table, interp, jit, gen, control and selftest modes, and perf control.
- `run.sh`: the full reproduction.
- `summarize.py`: medians, the agreement check and the derived metrics.
- `results/`: the outputs listed above.
