# E14 results: lifetime of TU-free function keys (lua/lua history)

Plan: `journal/cycle6-plan.md`, E14. Everything under **Measured** is produced by `run.sh` on this VM (`results/summary.txt`).
Lines under **Estimated / interpretation** are not measured.

## Setup
- Data: E10's preprocessed history, reused unchanged (`../e10-hidden-deps/work`: `tok.tsv` plus the gzipped `.i` CAS).
  - That is the 1500 newest first-parent lua/lua commits touching core `.c/.h`, pinned at 8cb93bad, clang-18, E10's flags.
  - `run.sh` runs E10's `run.sh` first if that data is absent (about 30 min); otherwise it only keys and analyzes (about 1 min, 8 cores).
- Units: every (commit transition N to N+1, core TU, extern function present in the TU at both N and N+1) where the TU preprocessed and split at both.
  The splitter failed on 1 of 7381 distinct TUs (same handling as E10: skipped).
- Keys per extern function, all token-normalized (compiler and flags are constant over the history, so they do not vary any key):
  - **per-TU**: E10's token hash of the whole preprocessed TU (ccache-style).
  - **E14 TU-free key**: every non-function decl, the target's body, the bodies of the statics it reaches transitively,
    and every other extern function as a prototype. Unreached statics are dropped. This is the E13 worker input.
  - **E8 minimal slice**: `tu.build(target + reached statics)` from E8's slicer, i.e. the callee-only slice with every reached static's body.
    *Deviation:* this is not E10's per-target learned d. Learned d keeps at most these static bodies, so it would change no more often,
    except for E10's "never" targets, whose key is the whole TU.
  - **body hash**: tokens of the target's own definition.
- Primary metric: per-transition change rate (changes / function-transitions). It has no censoring problem.
  Mean lifetime in commits = 1 / rate. The plan asked for median censored lifetime; the rate ratio is used instead and equals the mean-lifetime ratio.

## Measured
739,505 function-transitions over 1500 commits.

| key | changes | change rate | mean lifetime (1/rate, commits) | lifetime vs per-TU |
|---|---:|---:|---:|---:|
| per-TU (token hash) | 135,092 | 0.1827 | 5.5 | 1.00x |
| E14 TU-free key | 104,880 | 0.1418 | 7.1 | **1.29x** |
| E8 minimal slice | 37,312 | 0.0505 | 19.8 | 3.62x |
| body hash | 3,603 | 0.0049 | 205.2 | 37.5x |

- Of the 7,249 TU-level transitions where the TU's tokens changed, **80.9%** (5,862) changed **every** E14 key in that TU.
  Whatever change hit the TU hit the shared part (non-function decls or extern prototypes) that every TU-free key contains.
- These match the plan reviewer's numbers from E10's data exactly (0.183 / 0.142 / 0.051 / 0.0049; 1.0 / 1.29 / 3.6 / 37; 80.9%).

## Decision
- Rule: ≥20x builds the per-function cache of expensively optimized results; under 5x stops. **E14 key: 1.29x, under 5x: stop.**
  TU-free keys churn almost as fast as per-TU keys, so spare-cycle optimization keyed this way would not amortize.
  The bytes metric is only needed in the 5–20x band, so it was not computed.
- *Interpretation:* E14's key includes every non-function decl and every extern prototype, which carries most of the churn.
  Only keys that drop them get long lifetimes: the slice reaches 3.6x and the body hash 37x.
  The body hash is not a sound key for compiled output on its own (E10: hidden dependencies).
- *Caveat (from plan review):* lifetime is necessary, not sufficient. Even a long-lived key says nothing about how much an expensive optimization
  would gain per function, so a pass here would still have needed a benefit estimate. None was made.
