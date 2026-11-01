/* K2: memo-table transfer in 1-D HashLife (journal/phase2-plan.md, K2).
 *
 * Node ids 0 and 1 are the level-0 cells. Every other node is a hash-consed
 * pair (left, right) of nodes one level down; level-k node = 2^k cells.
 * result(n, k) = the centre 2^(k-1) cells after 2^(k-2) steps (k >= 2),
 * memoised in a separate result table (the "memo").
 *
 * Usage:
 *   k2 validate RULE T P SEED...   HashLife vs bit-sliced direct stepping
 *   k2 transfer RULE T P NB SEEDA  scratch runs, build A, freeze, transfer runs
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MEMO_CAP (1u << 24)   /* kill-test cap on memo entries per build */
#define NODE_CAP (1u << 27)
#define RES_CAP (1u << 26)    /* table capacity (frozen + new in transfer runs) */
#define NB_BITS 26
#define NONE 0xffffffffu
#define MAXL 40

typedef uint32_t u32;
typedef uint64_t u64;

static int RULE;
static u32 *nl, *nr, *nnext, *nhead, ncount;
static u32 *mk, *mv, *mnext, *mhead, mcount;
static u32 node_wm, memo_wm;     /* frozen watermarks (0 = nothing frozen) */
static u32 memo_limit;            /* abort if memo grows past this */
static jmp_buf abort_jb;

/* stats */
static u64 look[MAXL], fhit[MAXL], ohit[MAXL], miss[MAXL];
static u64 cons_f, cons_o, cons_new;

static inline u32 hpair(u32 a, u32 b) {
  u64 h = (u64)a * 0x9E3779B97F4A7C15ull ^ ((u64)b + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full;
  h ^= h >> 29;
  return (u32)(h >> (64 - NB_BITS));
}
static inline u32 hone(u32 a) {
  u64 h = ((u64)a + 1) * 0x9E3779B97F4A7C15ull;
  return (u32)(h >> (64 - NB_BITS));
}

static u32 cons(u32 a, u32 b) {
  u32 h = hpair(a, b);
  for (u32 i = nhead[h]; i != NONE; i = nnext[i])
    if (nl[i] == a && nr[i] == b) {
      if (i < node_wm) cons_f++; else cons_o++;
      return i;
    }
  if (ncount >= NODE_CAP) { fprintf(stderr, "node table full\n"); longjmp(abort_jb, 2); }
  u32 i = ncount++;
  nl[i] = a; nr[i] = b; nnext[i] = nhead[h]; nhead[h] = i;
  cons_new++;
  return i;
}

static inline int rule(int a, int b, int c) { return (RULE >> (a * 4 + b * 2 + c)) & 1; }

static u32 result(u32 n, int k) {
  u32 h = hone(n);
  look[k]++;
  for (u32 i = mhead[h]; i != NONE; i = mnext[i])
    if (mk[i] == n) {
      if (i < memo_wm) fhit[k]++; else ohit[k]++;
      return mv[i];
    }
  miss[k]++;
  u32 r;
  if (k == 2) {
    u32 L = nl[n], R = nr[n];
    int a = nl[L], b = nr[L], c = nl[R], d = nr[R];
    r = cons(rule(a, b, c), rule(b, c, d));
  } else {
    u32 L = nl[n], R = nr[n];
    u32 a = nl[L], b = nr[L], c = nl[R], d = nr[R];
    u32 r1 = result(L, k - 1), r2 = result(cons(b, c), k - 1), r3 = result(R, k - 1);
    u32 s1 = result(cons(r1, r2), k - 1), s2 = result(cons(r2, r3), k - 1);
    r = cons(s1, s2);
    (void)a; (void)d;
  }
  if (mcount - memo_wm >= memo_limit || mcount >= RES_CAP) longjmp(abort_jb, 1);
  u32 i = mcount++;
  mk[i] = n; mv[i] = r; mnext[i] = mhead[h]; mhead[h] = i;
  return r;
}

/* ---- tables ---- */
static void *xalloc(size_t n) { void *p = malloc(n); if (!p) { perror("malloc"); exit(1); } return p; }
static void tables_init(void) {
  nl = xalloc(4ull * NODE_CAP); nr = xalloc(4ull * NODE_CAP); nnext = xalloc(4ull * NODE_CAP);
  mk = xalloc(4ull * RES_CAP); mv = xalloc(4ull * RES_CAP); mnext = xalloc(4ull * RES_CAP);
  nhead = xalloc(4ull << NB_BITS); mhead = xalloc(4ull << NB_BITS);
}
static u32 ecache[MAXL][14];
static void ecache_clear(void) { memset(ecache, 0xff, sizeof ecache); }
static void tables_reset(void) {
  memset(nhead, 0xff, 4ull << NB_BITS); memset(mhead, 0xff, 4ull << NB_BITS);
  ncount = 2; mcount = 0; node_wm = memo_wm = 0; ecache_clear();
}
/* drop everything added since the freeze; new entries are always chain heads */
static void truncate_to_freeze(void) {
  for (u64 h = 0; h < (1ull << NB_BITS); h++) {
    while (nhead[h] != NONE && nhead[h] >= node_wm) nhead[h] = nnext[nhead[h]];
    while (mhead[h] != NONE && mhead[h] >= memo_wm) mhead[h] = mnext[mhead[h]];
  }
  ncount = node_wm; mcount = memo_wm; ecache_clear();
}
static void stats_clear(void) {
  memset(look, 0, sizeof look); memset(fhit, 0, sizeof fhit); memset(ohit, 0, sizeof ohit);
  memset(miss, 0, sizeof miss); cons_f = cons_o = cons_new = 0;
}

/* ---- background and patches ---- */
static const char *ETHER = "00010011011111"; /* rule 110 ether, x(t+1)[i] = x(t)[i+4] */
static int bgper;
static inline int bgcell(long long x, long long t) {
  if (bgper == 1) return 0;
  long long j = ((x + 4 * t) % 14 + 14) % 14;
  return ETHER[j] - '0';
}
static u32 bgnode(int k, long long off) {
  int ph = (int)(((off % bgper) + bgper) % bgper);
  if (k == 0) return (u32)bgcell(off, 0);
  if (ecache[k][ph] != NONE) return ecache[k][ph];
  u32 a = bgnode(k - 1, off), b = bgnode(k - 1, off + (1ll << (k - 1)));
  return ecache[k][ph] = cons(a, b);
}
static uint8_t *patch; static long long p0, plen;
static u32 build(int k, long long off) {
  long long end = off + (1ll << k);
  if (end <= p0 || off >= p0 + plen) return bgnode(k, off);
  if (k == 0) return patch[off - p0];
  return cons(build(k - 1, off), build(k - 1, off + (1ll << (k - 1))));
}
static u64 sm_state;
static u64 splitmix(void) {
  u64 z = (sm_state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
static void make_patch(u64 seed, long long P, int T) {
  sm_state = seed * 0x1234567ull + 77;
  plen = P; free(patch); patch = xalloc(P);
  for (long long i = 0; i < P; i++) patch[i] = splitmix() & 1;
  p0 = (1ll << (T + 1)) - P / 2;   /* centred in the level-(T+2) root */
}

/* ---- evolution ---- */
typedef struct { int ok; u32 res; double sec; } run_t;
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + 1e-9 * ts.tv_nsec; }
static run_t evolve(u64 seed, int T, long long P) {
  run_t R = {0, 0, 0};
  make_patch(seed, P, T);
  double t0 = now();
  int rc = setjmp(abort_jb);
  if (rc == 0) {
    u32 root = build(T + 2, 0);
    R.res = result(root, T + 2);
    R.ok = 1;
  }
  R.sec = now() - t0;
  return R;
}

/* ---- validation ---- */
static void flatten(u32 n, int k, uint8_t *out) {
  if (k == 0) { out[0] = (uint8_t)n; return; }
  flatten(nl[n], k - 1, out); flatten(nr[n], k - 1, out + (1ll << (k - 1)));
}
static int do_validate(int T, long long P, int nseeds, char **seeds) {
  long long W = 1ll << (T + 2), NW = W / 64;
  int bad = 0;
  for (int s = -1; s < nseeds; s++) {
    u64 seed = s < 0 ? 0 : strtoull(seeds[s], 0, 10);
    long long PP = s < 0 ? 0 : P; /* s<0: pure background (stationarity check) */
    tables_reset(); stats_clear(); memo_limit = RES_CAP;
    run_t R = evolve(seed, T, PP);
    if (!R.ok) { printf("validate: aborted\n"); return 1; }
    uint8_t *hl = xalloc(W / 2);
    flatten(R.res, T + 1, hl);
    /* direct bit-sliced stepping over the full level-(T+2) width */
    u64 *a = calloc(NW, 8), *b = calloc(NW, 8);
    for (long long x = 0; x < W; x++) {
      int v = (x >= p0 && x < p0 + PP) ? patch[x - p0] : bgcell(x, 0);
      if (v) a[x / 64] |= 1ull << (x % 64);
    }
    for (long long t = 0; t < (1ll << T); t++) {
      for (long long i = 0; i < NW; i++) {
        u64 c = a[i];
        u64 l = (c << 1) | (i > 0 ? a[i - 1] >> 63 : 0);       /* cell x-1 */
        u64 r = (c >> 1) | (i + 1 < NW ? a[i + 1] << 63 : 0);  /* cell x+1 */
        u64 o = 0;
        for (int m = 0; m < 8; m++)
          if ((RULE >> m) & 1)
            o |= ((m & 4) ? l : ~l) & ((m & 2) ? c : ~c) & ((m & 1) ? r : ~r);
        b[i] = o;
      }
      u64 *tmp = a; a = b; b = tmp;
    }
    long long mism = 0, bgdiff = 0;
    for (long long j = 0; j < W / 2; j++) {
      long long x = W / 4 + j;
      int d = (a[x / 64] >> (x % 64)) & 1;
      if (d != hl[j]) mism++;
      if (hl[j] != bgcell(x, 1ll << T)) bgdiff++;
    }
    printf("validate rule=%d T=%d steps=%lld P=%lld seed=%s cells_compared=%lld mismatches=%lld "
           "cells_differing_from_background=%lld memo=%u nodes=%u\n",
           RULE, T, 1ll << T, PP, s < 0 ? "background" : seeds[s], W / 2, mism, bgdiff, mcount, ncount);
    if (mism || (s < 0 && bgdiff)) bad = 1;
    free(a); free(b); free(hl);
  }
  printf("validate rule=%d T=%d: %s\n", RULE, T, bad ? "FAIL" : "PASS");
  return bad;
}

/* ---- transfer experiment ---- */
static u64 sum(u64 *v) { u64 s = 0; for (int i = 0; i < MAXL; i++) s += v[i]; return s; }
static int do_transfer(int T, long long P, int NB, u64 seedA) {
  u64 *scr_memo = xalloc(8 * (NB + 1)), *scr_nodes = xalloc(8 * (NB + 1));
  int *scr_ok = xalloc(sizeof(int) * (NB + 1));
  printf("# rule=%d T=%d steps=2^%d P=%lld NB=%d seedA=%llu memo_cap=%u\n", RULE, T, T, P, NB,
         (unsigned long long)seedA, MEMO_CAP);
  /* from-scratch builds: index 0 = A, i = B_i (seed seedA + i) */
  for (int i = 0; i <= NB; i++) {
    tables_reset(); stats_clear(); memo_limit = MEMO_CAP;
    run_t R = evolve(seedA + i, T, P);
    scr_ok[i] = R.ok; scr_memo[i] = mcount; scr_nodes[i] = ncount - 2;
    printf("scratch %s%d ok=%d memo=%u nodes=%u lookups=%llu sec=%.2f%s\n", i ? "B" : "A", i, R.ok,
           mcount, ncount - 2, (unsigned long long)sum(look), R.sec, R.ok ? "" : "  CAP EXCEEDED");
    fflush(stdout);
  }
  if (!scr_ok[0]) { printf("A: memo cap 2^24 exceeded; transfer not run\n"); return 0; }
  /* build A and freeze */
  tables_reset(); stats_clear(); memo_limit = MEMO_CAP;
  run_t RA = evolve(seedA, T, P);
  if (!RA.ok) {
    printf("A: memo cap 2^24 exceeded (memo=%u nodes=%u after %.1fs); transfer not run\n", mcount, ncount - 2, RA.sec);
    return 0;
  }
  node_wm = ncount; memo_wm = mcount;
  printf("frozen A: memo_entries=%u nodes=%u memo_bytes=%llu node_bytes=%llu (12 B each: key,val,chain / l,r,chain)\n",
         memo_wm, node_wm - 2, 12ull * memo_wm, 12ull * (node_wm - 2));
  u64 tl[MAXL] = {0}, tf[MAXL] = {0}, tm[MAXL] = {0};
  double sum_hr = 0, sum_ratio = 0, min_hr = 1, max_ratio = 0; int nok = 0;
  for (int i = 1; i <= NB; i++) {
    stats_clear(); memo_limit = RES_CAP;
    run_t R = evolve(seedA + i, T, P);
    u64 L = sum(look), F = sum(fhit), O = sum(ohit), M = sum(miss);
    double hr = L ? (double)F / L : 0, ratio = scr_ok[i] ? (double)M / scr_memo[i] : -1;
    printf("transfer B%d ok=%d lookups=%llu frozen_hits=%llu own_hits=%llu new_entries=%llu "
           "hit_rate=%.4f scratch_entries=%llu new/scratch=%.4f nodes_new=%llu nodes_frozen_hits=%llu sec=%.2f\n",
           i, R.ok, (unsigned long long)L, (unsigned long long)F, (unsigned long long)O, (unsigned long long)M, hr,
           (unsigned long long)scr_memo[i], ratio, (unsigned long long)cons_new, (unsigned long long)cons_f, R.sec);
    if (R.ok && scr_ok[i]) {
      nok++; sum_hr += hr; sum_ratio += ratio;
      if (hr < min_hr) min_hr = hr;
      if (ratio > max_ratio) max_ratio = ratio;
      for (int k = 0; k < MAXL; k++) { tl[k] += look[k]; tf[k] += fhit[k]; tm[k] += miss[k]; }
    }
    fflush(stdout);
    truncate_to_freeze();
  }
  printf("per-level over all B (level k = 2^k cells, 2^(k-2) steps):\n  k  lookups  frozen_hit_rate  new_entries\n");
  for (int k = 2; k < MAXL; k++)
    if (tl[k]) printf("  %2d %10llu %8.4f %10llu\n", k, (unsigned long long)tl[k], (double)tf[k] / tl[k], (unsigned long long)tm[k]);
  u64 L6 = 0, F6 = 0;
  for (int k = 6; k < MAXL; k++) { L6 += tl[k]; F6 += tf[k]; }
  printf("summary rule=%d T=%d n=%d mean_hit_rate=%.4f min_hit_rate=%.4f hit_rate_levels>=6=%.4f "
         "mean_new/scratch=%.4f max_new/scratch=%.4f\n",
         RULE, T, nok, nok ? sum_hr / nok : 0, min_hr, L6 ? (double)F6 / L6 : 0, nok ? sum_ratio / nok : 0, max_ratio);
  int pass = nok == NB && sum_hr / nok >= 0.90 && sum_ratio / nok <= 0.10;
  printf("rule(hit>=0.90 and new<=0.10 of scratch, means over B): %s\n", pass ? "REUSABLE" : "INPUT-SPECIFIC");
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "usage: see header\n"); return 2; }
  RULE = atoi(argv[2]);
  int T = atoi(argv[3]);
  long long P = atoll(argv[4]);
  bgper = RULE == 110 ? 14 : 1;
  if (T + 2 >= MAXL) return 2;
  tables_init();
  if (!strcmp(argv[1], "validate")) return do_validate(T, P, argc - 5, argv + 5);
  if (!strcmp(argv[1], "transfer") && argc >= 7) return do_transfer(T, P, atoi(argv[5]), strtoull(argv[6], 0, 10));
  return 2;
}
