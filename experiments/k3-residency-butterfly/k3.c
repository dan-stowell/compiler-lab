/* K3: residency on a DAG that cannot be fused.
 *
 * Workload: radix-2 butterfly network over n = 2^L u32 elements.
 *   leaf i            = high 32 bits of splitmix64(i)
 *   stage s (0..L-1)  pairs i and i|2^s (bit s of i clear):
 *                       a' = a + b,  b' = rotl(a - b, R(s))     (mod 2^32)
 *   output            = H = sum_i mix64((i << 32) | out_i)  mod 2^64
 *                       (order-independent, so any schedule may emit in any order)
 *
 * Modes:
 *   mat     per-stage materialization: leaves -> array, 24 full passes, hash pass
 *   blocked best conventional schedule (four-step): pass 1 = stages 0..K1-1 inside
 *           L2-sized contiguous blocks (leaves generated in-cache, written out once
 *           with non-temporal stores); pass 2 = remaining stages over column groups
 *           gathered into an L2 buffer, hashed in-cache (never written back).
 *           blocked-rs: same but regular stores in pass 1 (lets L3 keep part of it).
 *   model   residency budget B = 1 MiB, no spilling: one 1 MiB arena is the only
 *           storage. Output is produced in column chunks; for each chunk every
 *           level-k block is recomputed from seeds (stages 0..k-1), the chunk's
 *           columns are kept, then the remaining L-k stages run on the resident
 *           chunk and it is hashed. Nothing ever leaves the arena.
 *   resident     control: k3 resident 0 REPS runs the stage kernel REPS times over a
 *                1 MiB arena (same footprint as model, nothing regenerated)
 *   calib-read   sum over a 1 GiB array (pre-touched)  -> expect 1 GiB of fills
 *   calib-ntw    non-temporal write of a 1 GiB array   -> writes (not fills)
 *
 * Usage: k3 MODE [log2n=24] [model_k=9]
 * Prints: mode, n, hash, and counted work (butterflies, leaves, computed bytes).
 */
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;

#define CH 4096 /* L1 sub-block, elements (16 KiB) */

static inline u64 mix64(u64 z) {
    z ^= z >> 30; z *= 0xBF58476D1CE4E5B9ull;
    z ^= z >> 27; z *= 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static inline u32 leaf(u64 i) { return (u32)(mix64(i * 0x9E3779B97F4A7C15ull + 0x9E3779B97F4A7C15ull) >> 32); }
static inline int rot(int s) { return (s % 31) + 1; }
static inline u32 rotl(u32 x, int r) { return (x << r) | (x >> (32 - r)); }

static u64 n_bfly, n_leaf; /* counted work */

static void gen(u32 *restrict x, u64 base, size_t len) {
    for (size_t j = 0; j < len; j++) x[j] = leaf(base + j);
    n_leaf += len;
}

/* one stage: pairs at distance d inside groups of 2d */
#define STAGE_BODY(D)                                                   \
    for (size_t b = 0; b < L; b += 2 * (D))                             \
        for (size_t i = 0; i < (D); i++) {                              \
            u32 a = x[b + i], c = x[b + i + (D)];                       \
            x[b + i] = a + c; x[b + i + (D)] = rotl(a - c, r);          \
        }
static void stage(u32 *restrict x, size_t L, size_t d, int r) {
    switch (d) { /* constant small distances so the compiler can vectorize them */
    case 1: STAGE_BODY(1) break;
    case 2: STAGE_BODY(2) break;
    case 4: STAGE_BODY(4) break;
    case 8: STAGE_BODY(8) break;
    default: STAGE_BODY(d) break;
    }
    n_bfly += L / 2;
}

/* Apply local stages t0..t1-1 to x[0..L): local stage t pairs distance unit<<t,
 * global stage number sbase+t. Stages whose pair distance fits in an L1 sub-block
 * (S = unit<<tl <= CH elements) are run sub-block by sub-block; the rest
 * stage-by-stage over the whole range. L must be a multiple of unit<<t1. */
static void run(u32 *x, size_t L, size_t unit, int t0, int t1, int sbase) {
    int tl = t0;
    while (tl < t1 && (unit << (tl + 1)) <= CH) tl++;
    size_t S = unit << tl; /* sub-block: closed under stages t0..tl-1, divides L */
    if (tl > t0 && S < L) {
        for (size_t o = 0; o < L; o += S)
            for (int t = t0; t < tl; t++) stage(x + o, S, unit << t, rot(sbase + t));
    } else tl = t0;
    for (int t = tl; t < t1; t++) stage(x, L, unit << t, rot(sbase + t));
}

static u64 hash_range(const u32 *v, u64 idx0, size_t len) {
    u64 h = 0;
    for (size_t j = 0; j < len; j++) h += mix64(((idx0 + j) << 32) | v[j]);
    return h;
}

static void *xalloc(size_t bytes) {
    void *p = aligned_alloc(64, (bytes + 63) & ~(size_t)63);
    if (!p) { perror("alloc"); exit(1); }
    return p;
}

static void stream_copy(u32 *dst, const u32 *src, size_t len) { /* len multiple of 16, 64B aligned */
    for (size_t j = 0; j < len; j += 16)
        _mm512_stream_si512((void *)(dst + j), _mm512_load_si512((const void *)(src + j)));
    _mm_sfence();
}

static u64 bytes_r, bytes_w; /* computed DRAM bytes (spill traffic past the 1 MiB L2) */

static u64 do_mat(int lg) {
    size_t n = (size_t)1 << lg;
    u32 *x = xalloc(n * 4);
    gen(x, 0, n);
    bytes_w += n * 4;
    for (int s = 0; s < lg; s++) { stage(x, n, (size_t)1 << s, rot(s)); bytes_r += n * 4; bytes_w += n * 4; }
    u64 h = hash_range(x, 0, n);
    bytes_r += n * 4;
    if (n * 4 <= (1u << 20)) bytes_r = bytes_w = 0; /* fits in L2: no spill */
    free(x);
    return h;
}

static u64 do_blocked(int lg, int nt) {
    size_t n = (size_t)1 << lg;
    int k1 = lg < 17 ? lg : 17; /* 2^17 u32 = 512 KiB block: half of L2 */
    size_t blk = (size_t)1 << k1, R = n >> k1;
    u32 *buf = xalloc(blk * 4);
    if (R == 1) { /* everything fits: one in-cache pass */
        gen(buf, 0, n); run(buf, n, 1, 0, lg, 0);
        u64 h = hash_range(buf, 0, n); free(buf); return h;
    }
    u32 *big = xalloc(n * 4);
    for (size_t hb = 0; hb < R; hb++) { /* pass 1 */
        gen(buf, (u64)hb * blk, blk);
        run(buf, blk, 1, 0, k1, 0);
        if (nt) stream_copy(big + hb * blk, buf, blk);
        else memcpy(big + hb * blk, buf, blk * 4);
    }
    bytes_w += n * 4;
    if (!nt) bytes_r += n * 4; /* RFO on regular stores (upper bound; L3 may absorb) */
    size_t c = blk / R; /* columns per group, so R*c = blk */
    u64 h = 0;
    for (size_t col = 0; col < blk; col += c) { /* pass 2 */
        for (size_t r = 0; r < R; r++) memcpy(buf + r * c, big + r * blk + col, c * 4);
        run(buf, R * c, c, 0, lg - k1, k1);
        for (size_t r = 0; r < R; r++) h += hash_range(buf + r * c, (u64)r * blk + col, c);
    }
    bytes_r += n * 4;
    free(big); free(buf);
    return h;
}

static size_t budget_bytes = 1u << 20; /* B = 1 MiB residency budget */

static u64 do_model(int lg, int k) {
    const size_t ARENA = budget_bytes / 4; /* budget, in u32 */
    u32 *arena = xalloc(ARENA * 4);
    if (k > lg) k = lg;
    size_t blk = (size_t)1 << k, R = (size_t)1 << (lg - k);
    if (blk + R > ARENA) { fprintf(stderr, "k=%d: one column does not fit the budget\n", k); exit(2); }
    size_t c = (ARENA - blk) / R; /* columns per output chunk */
    if (c > blk) c = blk;
    u32 *b = arena, *acc = arena + blk;
    u64 h = 0;
    for (size_t col = 0; col < blk; col += c) {
        size_t cc = col + c <= blk ? c : blk - col;
        for (size_t r = 0; r < R; r++) { /* recompute every block's cone from seeds */
            gen(b, (u64)r * blk, blk);
            run(b, blk, 1, 0, k, 0);
            memcpy(acc + r * cc, b + col, cc * 4);
        }
        run(acc, R * cc, cc, 0, lg - k, k);
        for (size_t r = 0; r < R; r++) h += hash_range(acc + r * cc, (u64)r * blk + col, cc);
    }
    free(arena);
    return h;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: k3 mat|blocked|blocked-rs|model|calib-read|calib-ntw [log2n] [k]\n"); return 1; }
    const char *m = argv[1];
    int lg = argc > 2 ? atoi(argv[2]) : 24;
    int k = argc > 3 ? atoi(argv[3]) : 9;
    if (argc > 4) budget_bytes = (size_t)atoi(argv[4]) << 10; /* KiB */
    if (!strcmp(m, "spin")) { /* ALU-only control: no memory traffic */
        u64 z = 1; for (u64 i = 0; i < ((u64)1 << lg); i++) z = mix64(z + i);
        printf("spin %llu\n", (unsigned long long)z); return 0;
    }
    if (!strncmp(m, "calib", 5)) {
        size_t N = (size_t)1 << 28; /* 1 GiB of u32 */
        u32 *a = xalloc(N * 4);
        u64 s = 0;
        if (!strcmp(m, "calib-read")) {
            memset(a, 1, N * 4); /* touch (writes; measure with the 'touch' variant to subtract) */
            if (argc > 2 && !strcmp(argv[2], "touch-only")) { printf("touch %u\n", a[123]); return 0; }
            for (size_t i = 0; i < N; i++) s += a[i];
        } else {
            __m512i v = _mm512_set1_epi32(7);
            for (size_t i = 0; i < N; i += 16) _mm512_stream_si512((void *)(a + i), v);
            _mm_sfence();
            s = a[12345];
        }
        printf("%s sum=%llu\n", m, (unsigned long long)s);
        return 0;
    }
    if (!strcmp(m, "resident")) { /* control: same 1 MiB footprint and kernel as model, no leaf regen */
        size_t A = budget_bytes / 4; u32 *x = xalloc(A * 4);
        gen(x, 0, A);
        for (int rep = 0; rep < k; rep++) run(x, A, 1, 0, __builtin_ctzl(A), 0);
        printf("mode=resident reps=%d hash=%016llx butterflies=%llu\n", k, (unsigned long long)hash_range(x, 0, A),
               (unsigned long long)n_bfly);
        return 0;
    }
    u64 h;
    if (!strcmp(m, "mat")) h = do_mat(lg);
    else if (!strcmp(m, "blocked")) h = do_blocked(lg, 1);
    else if (!strcmp(m, "blocked-rs")) h = do_blocked(lg, 0);
    else if (!strcmp(m, "model")) h = do_model(lg, k);
    else { fprintf(stderr, "bad mode\n"); return 1; }
    u64 base = (u64)lg << (lg - 1);
    printf("mode=%s log2n=%d k=%d hash=%016llx butterflies=%llu leaves=%llu recompute_factor=%.2f "
           "computed_dram_read=%llu computed_dram_write=%llu\n",
           m, lg, !strcmp(m, "model") ? k : 0, (unsigned long long)h, (unsigned long long)n_bfly,
           (unsigned long long)n_leaf, (double)n_bfly / base, (unsigned long long)bytes_r,
           (unsigned long long)bytes_w);
    return 0;
}
