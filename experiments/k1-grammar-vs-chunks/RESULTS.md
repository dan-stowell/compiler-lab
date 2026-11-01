# K1 results: grammar-resident data vs a chunk-summary store

To reproduce, run `./run.sh`. It builds `k1.c` with gcc 13.3 `-O2 -march=native`, clones lua/lua into `data/`, and writes `results/synth.txt` and `results/lua.txt`. Machine: an 8-vCPU AMD EPYC 9554P exe.dev VM, single-threaded. The synthetic run takes 44 s and peaks at 3.7 GiB RSS.

## Setup
- **Query.** The language is "the number of (overlapping) occurrences of `xyzzy|plugh|luaV_|vmfetch` is odd". It is built as an Aho-Corasick DFA times a parity bit, with **|Q| = 46**. The DFA is only built after storage is complete. A summary is a |Q|-byte map from start state to end state. Leaf and chunk summaries are built by advancing all |Q| start states together and merging states that coincide (convergence). Both systems use the same routine. Because of the parity bit, at least 2 states always survive.
- **Synthetic workload.** The base is 64 MiB drawn from 26 letters plus space and newline. There are V = 10^4 versions, each with 1–10 overwrites of 1–64 bytes. 10% of the overwrites plant a pattern. That gives 54,619 edits (1.77 MB) in total. Versions are stored as edit scripts and never materialized.
  - **Grammar:** a persistent balanced binary tree over L-byte leaves, using path copying. Nodes created in the current version are mutated in place. Summaries are memoized by node id.
  - **Chunk store:** fixed c-byte chunks (the best case for overwrites), exact dedup (hash + memcmp), and a manifest per version. The query builds each unique chunk's summary once, then threads a single state through each version's manifest. That is 1 lookup per chunk, which is cheaper than a full composition.
- **Real workload:** all 795 committed versions of lua/lua `lvm.c` (26.8 MB raw, final version 62 KB). This one is small.
  - **Grammar deviation:** real diffs insert and delete, so this grammar is a hash-consed content-defined tree (a prolly tree: CDC leaves, average fanout 4) rather than path copying.
  - **Chunk store:** CDC (gear hash) chunks.
- **Counted cost definitions:**
  - ops = DFA transitions and merge work during summary building + |Q| per composition + 1 per state step.
  - bytes = data bytes scanned + summary bytes (3|Q| per composition, |Q| per summary built, |Q| per state step since each step touches one record) + ids (8 B per binary node, 4 B per manifest entry).
  - `bytes_lo` is the same but counts each chunk-store step as touching 1 summary byte. This is the most favourable count for the chunk store.

## Correctness (measured)
- Synthetic: grammar L=16/64/256 and chunks c=256…64K give the same accept/reject on all 10,001 versions (4,982 accept).
- Synthetic, direct scans: versions 0, 1, 2, 3333, 5000, 9999 and 10000 were materialized and scanned directly. All match.
- lvm.c: all 795 versions were checked against a direct scan for every configuration (495 accept).

## Numbers: synthetic, 64 MiB × 10^4 (all measured, exact counts)
Per-version marginal cost is the average over v ≥ 1 and includes the new summaries built for that version.

| config | comps / version | state steps / version | new summary bytes / version | ops / version | bytes / version | bytes_lo / version | query wall |
|---|---|---|---|---|---|---|---|
| grammar L=16 | 116.6 | 0 | 6,109 | 13,849 | 18,029 | 18,029 | 1.81 s |
| grammar L=64 | 97.7 | 0 | 4,866 | **9,566** | 15,152 | 15,152 | 0.78 s |
| grammar L=256 | 84.7 | 0 | 4,179 | 10,093 | **14,220** | **14,220** | 0.47 s |
| chunks c=256 | 0 | 262,144 | 281 | 268,330 | 13.1 M | 1.31 M | 10.8 s |
| chunks c=1K | 0 | 65,536 | 258 | 79,873 | 3.28 M | 333,691 | 4.84 s |
| chunks c=4K | 0 | 16,384 | 253 | **64,191** | 841,961 | **104,681** | 2.72 s |
| chunks c=16K | 0 | 4,096 | 251 | 185,848 | **294,529** | 110,209 | 3.90 s |
| chunks c=64K | 0 | 1,024 | 250 | 717,139 | 408,104 | 362,024 | 13.7 s |

The table below compares the best configuration of each system. The whole query covers all 10,001 versions and includes the base summaries over 64 MiB, which both systems must build.

| metric | best grammar | best chunks | gap |
|---|---|---|---|
| ops / version | 9,566 | 64,191 | **6.7×** |
| bytes / version (line-granular steps) | 14,220 | 294,529 | **20.7×** |
| bytes_lo / version | 14,220 | 104,681 | **7.4×** |
| raw operation count / version (one composition vs one step) | 97.7 | 16,384 | 168× (not comparable: a composition is 46 lanes wide; in lanes it is 3.6×) |
| ops, whole query | 378 M (L=256) | 784 M (c=4K) | 2.1× |
| bytes, whole query | 260 MB (L=256) | 3.01 GB (c=16K) | 11.6× |
| query wall time | 0.47 s | 2.72 s | 5.8× |

## Numbers: lua/lua lvm.c, 795 versions (measured; small)

| metric / version | best grammar (prolly) | best chunks (CDC) |
|---|---|---|
| ops | 10,164 (cdc=256) | **8,846** (cdc=256) |
| bytes | 8,327 (cdc=256) | **7,627** (cdc=1K); bytes_lo 3,490 |

The chunk store wins here. The file is about 60 KB, so a manifest has only about 30–110 entries, and real commits touch many places.

## Why the gap is small (estimated, not measured)
- The chunk store's per-version cost is n/c steps plus about e·c bytes of new chunks to summarize. At the best c this is about 2·sqrt(n·e). The grammar's cost is about e·(log n·|Q| + L).
- The gap therefore grows only as sqrt(n)/log n. Reaching 100× in ops would need n of roughly (100/6.7)^2 × 64 MiB ≈ 14 GiB per version (estimate).
- Summary convergence makes chunk summaries cheap (about 2–3 ops/byte). Without it, summary construction would penalize large chunks |Q|/2 times more. The optimum would then shift, but the asymptotics stay the same.

## Decision
At the workload where the gap should be largest (64 MiB, small edits, per-version marginal), the gap is **6.7× in ops and 7–21× in bytes**. On the real history the gap is below 1×. That is well under the 100× kill threshold. **P1 is the known rope/finger-tree property (O(edits·log n) per version against a sqrt(n) chunk-store optimum), and it is dropped.**

## Independent verification (2026-11-01)

The rerun reproduces exactly, and an independent numpy scan agrees on parity for 5 versions. Corrections:
- **Finer search grid** (64 MiB, V=2000): best grammar L=128 at 9,329 ops; best chunk store c=2K at 58,378. That is **6.3×** ops (not 6.7×) and 6.0–19.7× bytes.
- **Scaling, measured:** ops gap ≈3.5× at 16 MiB, 6.3× at 64 MiB, 11.6× at 256 MiB, i.e. ×1.85 per 4× size, consistent with √n/log n. 100× would need ~32 GiB (not 14 GiB).
- **The kill is stronger:** a chunk store with a tree-shaped manifest and per-node summaries (a standard Merkle layout) *is* the grammar, so the gap to a conventional store is ~1×. "Grammar-resident data" is the known monoid-annotated Merkle tree; known, not in production compilers.
