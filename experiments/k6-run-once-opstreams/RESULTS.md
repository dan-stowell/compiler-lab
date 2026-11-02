# K6: run-once code for op streams executed once

Reproduce with `./run.sh` (about 2.5 minutes). It writes `results/summary.txt`, `raw.txt`,
`check.txt` and `convert.txt`. Unless a line is marked **estimate**, every number here is
**measured**: perf user-mode counters over the measured region only, pinned to one vCPU, medians
of 5 repetitions (11 for the 16K-record natural stream). Cycles/op for `jit` include generation.

Setup notes. The runs used CPU 2 (`CPU=` overrides it). Another K4 job was running on CPU 6,
and on this VM the vCPU pairs 0-1, 2-3, 4-5 and 6-7 share L1 and L2, so CPU 7 was not clean
either. Load average was 1.4–2.0 during the runs: that K4 job plus this one. Each config ran
twice, because the 6th counter reads 0 on this VM. Group A counted cycles, instructions,
branch-misses, ic_cache_fill_l2 and ic_cache_fill_sys. Group B counted cycles and
bp_de_redirect.

## What was built

- `k6syn.c` is stream 1. It runs 16M ops of the form `R[d] = f(R[a], R[b])` over 16 uint32
  registers, with 16 kinds of uneven cost: 11 one-cycle ALU ops, MIN, MAX, SLT, MUL, MULHI, DIV
  and REM. With probability Q the op is ADD; otherwise its kind is uniform. Q = 0 is the
  synthetic stream (4 bits of entropy), and the sweep over Q is the predictability sweep.
  - Baselines: a computed-goto interpreter (`cgoto`); a fully branch-free version that computes
    all 16 kinds and selects one (`pred`); and a hybrid that runs the 14 cheap kinds branch-free
    and branches on DIV/REM.
  - Generator (K4 design): each kind has a precomputed template and length. Per op it does one
    `pshufb` of `opword<<2` to place the register displacements, then a single 16-byte store,
    and advances by the template length. There is no byte patching. The code runs as chunks of
    500 ops in a 2 × 8 KiB RWX ring.
- `k6lua.c` and `tracer/` are streams 2 and 3, real Lua 5.4.6 bytecode.
  - The tracer is patched into `vmfetch`. At each instruction it logs the instruction, its base,
    its K constant, and the stack slots it changed, found by diffing the stack against a shadow
    copy.
  - The converter builds a replay program over the Lua stack. It covers a supported subset with
    Lua semantics: MOVE/LOAD*, LOADNIL, ADD/SUB/MUL/DIV and their K/I forms, the bitwise ops and
    shifts, NOT, FORLOOP, and the numeric comparisons and TEST (whose results are accumulated;
    the trace already resolves the jumps). Every other instruction becomes EXT, which applies
    the writes the real VM made.
  - The converter replays its own output and checks each supported op against the real VM's
    logged result. Mismatches = 0 and final stack differences = 0 on all three programs.
- Natural stream: I chose **Lua bytecode at PCs executed exactly once**, because stream 3 needs
  the same tracer, converter and engine, so one faithful implementation serves both. The PDF
  option would have needed a second engine and real content streams. The modules are the main
  chunks of 86 real Lua files (Neovim runtime and LuaJIT `jit/*`), run under a permissive
  `vim`/`require` stub; 80 complete.
  - Of 86.6K executed instructions, 15.9K come from once-executed PCs (18.3%). Each run of other
    instructions collapses into one EXT holding its net writes.
  - The stream is 16.3K records: 41% MOVE (mostly LOADK), 58% EXT (SETFIELD, CLOSURE,
    NEWTABLE, ...).
- Control: the full trace of the project's Lua benchmark set (`e13 bench/*.lua`, sizes reduced
  to give 5.95M instructions).
- Lua run-once code:
  - Inlined: MOVE and single-write EXT, each a pair of `vmovups`. EXT with no writes emits
    nothing.
  - Every other kind is a direct `call` to its handler (call-threaded).
  - Generation is again one 16-byte template store per record. It is branch-free, with the
    fields placed by `pshufb`.
- Correctness: every implementation (cgoto, pred, hybrid, jit) reproduces all 16 registers
  (synthetic) or the full register file plus the comparison accumulator (Lua), at every Q and
  on every program (`results/check.txt`).

## Step 0 gate (measured inputs)

The gate estimates the interpreter as 3 + 20 × (cgoto branch-misses/op) and kills the stream if
that is below 2 × (generation cycles/op + 1).

| stream | cgoto mis/op | est. interp | gen cyc/op | 2(gen+1) | gate |
|---|---|---|---|---|---|
| synthetic (Q=0, 4 bits) | 0.937 | 21.7 | 3.31 | 8.6 | **pass** |
| natural: Lua once-executed PCs | 0.285 | 8.7 | 6.94 | 15.9 | **KILL** |
| control: Lua benchmark trace | 0.006 | 3.1 | 9.09 | 20.2 | KILL (expected) |
| (natural-module full trace, extra) | 0.072 | 4.4 | 7.07 | 16.2 | KILL |

The natural stream fails the gate. I report its measured implementation numbers below anyway,
because they cost nothing extra (the engine was needed for the control).

## Cycles/op, generation included (medians)

| stream | cgoto | pred | hybrid | **jit** (gen alone) | best base / jit | br-mis/op cgoto → jit |
|---|---|---|---|---|---|---|
| synthetic Q=0 | 24.22 | 23.65 | 20.69 | **5.47** (3.31) | **3.78×** | 0.937 → 0.003 |
| natural (once PCs) | 11.39 | n/a | 12.38 | **11.19** (6.94) | **1.02×** | 0.285 → 0.039 |
| control (bench trace) | 7.49 | n/a | 8.96 | **25.01** (9.09) | 0.30× | 0.006 → 0.006 |
| natural full trace | 7.56 | n/a | 8.62 | 14.95 (7.07) | 0.51× | 0.072 → 0.021 |

A fully branch-free `pred` is not applicable to the Lua kinds. EXT and LOADNIL loop over a
variable number of writes, and the arithmetic branches on run-time tags. `hybrid` is the
predicated variant there: MOVE and the int fast paths of ADD, SUB, MUL, BAND, BOR and BXOR run
branch-free, and everything else goes through the computed-goto table. Instructions per op and
I-cache fills from L2 are in `results/summary.txt`. On the synthetic stream, jit takes 0.18
`ic_cache_fill_l2` per op: every generated line is refilled from L2.

**Measured, and a new finding.** The control's jit loses by 3.3×. The cause is
`bp_de_redirect` at 0.67 per op. Every handler `call` in freshly written code is a taken branch
the BTB has never seen, so the decoder must re-steer, costing about 20 cycles per call
(cgoto: about 0). Taken branches in run-once code are therefore expensive, and run-once code
must be straight-line to win. The synthetic templates are straight-line (0.003 re-steers per op).
On the natural stream only multi-write EXT records call a handler (0.083 re-steers per op).

**Estimate.** Even with every Lua kind inlined, the control would lose. Its generator alone
(9.1 cycles/op) exceeds cgoto (7.5).

## Predictability sweep (synthetic; crossover)

| entropy (bits) | 4.00 | 3.62 | 2.83 | 1.96 | 1.43 | 0.82 | 0.46 | 0.21 | 0 |
|---|---|---|---|---|---|---|---|---|---|
| cgoto mis/op | 0.937 | 0.864 | 0.647 | 0.422 | 0.292 | 0.150 | 0.060 | 0.021 | 0 |
| best baseline | 20.7 | 20.4 | 18.4 | 12.9 | 10.2 | 6.9 | 4.9 | 4.0 | 3.6 |
| jit (incl. gen) | 5.47 | 5.37 | 5.15 | 5.09 | 5.14 | 5.23 | 5.03 | 5.37 | 5.46 |
| best / jit | 3.78 | 3.79 | 3.58 | 2.54 | 1.98 | 1.32 | 0.97 | 0.75 | 0.66 |

Crossovers come from linear interpolation between measured points:

- Run-once breaks even (1×) at **about 0.067 cgoto misses/op** (about 0.5 bits).
- It is 2× ahead at **about 0.30 misses/op** (about 1.45 bits).
- At low entropy, cgoto is best (3.6 cycles/op on a single kind). Above 0.5 bits, cgoto beats
  the hybrid and pred variants at every point, because they pay for all paths, including DIV.

## Dependency depth (per the K4 verifier's note; measured offline from the streams)

- **Synthetic:** the unit-latency critical path is 2.73M levels for 16.8M ops, so 6.1 ops per
  level. No independent sub-streams were manufactured. The out-of-order core already overlaps
  the independent ops, and the jit code runs at about 2.2 cycles/op beyond generation.
- **Natural:** depth 5 over 16.3K records. EXT writes are logged constants in the replay, so they
  have no input dependence.
- **Control:** about 990 records per level.

The Lua replay's dataflow is therefore shallower than the real VM's (table and call semantics
are not modelled). That makes the replay easier for every implementation alike, and it is no
reason to interleave.

## Decision (rule: ≥2× the best baseline on the natural stream)

**Not met.** On the natural stream run-once code is 1.02× the best baseline (11.19 vs 11.39
cycles/op), and Step 0 had already predicted the kill (8.7 < 15.9).

The natural stream sits at 0.285 cgoto misses/op. On the synthetic axis that is right at the
2× crossover (0.30). The synthetic crossover does not carry over, though. Lua records are
heavier: a 16-byte record versus a 4-byte op, and per-op generation of 6.9 versus 2.8–3.3
cycles. On top of that, interpreting the natural stream is already cheap per op, because 99%
of it is MOVE/EXT.

So run-once is killed as a general execution model for natural once-executed command streams.
It stays recorded as a known but unused technique with a measured regime: at least 2× over
the best interpreter only when dispatch mispredicts at ≥0.3 per op, the generator stays near
3 cycles/op, and the generated code is straight-line with no taken branches.

## Caveats

- The natural stream is small (16.3K records, about 190K cycles), so it runs cold. Its spread
  across 11 repetitions was 11.2–13.4 cycles/op for cgoto and 10.8–16.8 for jit.
- The Lua "faithful" semantics cover only the subset above. EXT stands in for the real VM's work
  on tables, calls and closures by applying logged writes, which understates every
  implementation's cost equally. Base + A is resolved by the converter.
- In the synthetic stream, AND, MIN and SLT tend to collapse register values. At Q=0, 8 of 16
  final values are distinct (`check.txt`), which may make DIV cheaper than it is on random data.
  This affects every implementation equally.

## Independent verification (2026-11-02)

Confirmed.
- **Rerun:** synthetic 3.94×, natural 1.02×, control 0.29×; crossover 0.066 / 0.294 misses/op. All 12 correctness checks pass.
- **Inlining:** removing every handler call on the natural stream would give at most ~1.2×. Generation alone costs 6.82 cycles/op, so even zero-cost execution caps it at 1.66×. **2× is unreachable on this stream with this generator.**
- **Warm variant:** still parity. Over 31 reps: cgoto median 11.29, jit 10.91.
- The BTB explanation for the control is a strong correlation (0.670 re-steers/op), not tested directly.
