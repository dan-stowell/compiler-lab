// K5: huge random objects as recursive split trees (journal/phase2-plan.md, K5).
//
// A column is (N, [lo,hi), seed): the multiset of N iid uniform 64-bit keys in [lo,hi),
// sorted, which exists only as a recursive split tree. A node (range [a,b), count n)
// splits at m = a + (b-a)/2 by drawing the left count ~ Binomial(n, (m-a)/(b-a)) from a
// counter-based RNG seeded by hash(seed, a, b). A node with n <= 64 (or width 1) is a leaf:
// it materializes its n keys iid uniform in [a,b) (Lemire's unbiased bounded draw), sorted.
// Recursive binomial splitting + iid uniform leaves is exactly the law of N sorted iid uniforms.
//
// Modes:
//   verify N W                       materialize + sort + merge vs tree operations
//   join N W [shift]                 full simultaneous-descent join, prints visits
//   est N W k S                      sampled estimate of join visits for huge N (labelled estimate)
//   range N W                        range count / range sum visits for a few random ranges
//   binomtest                        sanity tests of the binomial sampler
// W = 0 means the full 2^64 key space.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef uint64_t u64;
typedef unsigned __int128 u128;
#define LEAF 64

static inline u64 mix(u64 z) {  // splitmix64 finalizer
  z += 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}
typedef struct { u64 s; } rng_t;
static inline u64 next64(rng_t *r) { return mix(r->s += 0x9e3779b97f4a7c15ULL); }
static inline double unif(rng_t *r) { return ((next64(r) >> 11) + 0.5) * 0x1.0p-53; }  // (0,1)
static rng_t node_rng(u64 seed, u128 a, u128 b, u64 tag) {
  u64 h = mix(seed ^ tag);
  h = mix(h ^ (u64)a); h = mix(h ^ (u64)(a >> 64));
  h = mix(h ^ (u64)b); h = mix(h ^ (u64)(b >> 64));
  rng_t r = {h};
  return r;
}
// Unbiased uniform in [0, w), w in [1, 2^64] (Lemire 2019).
static u64 bounded(rng_t *r, u128 w) {
  if (w == ((u128)1 << 64)) return next64(r);
  u64 s = (u64)w;
  u128 m = (u128)next64(r) * s;
  u64 l = (u64)m;
  if (l < s) {
    u64 t = (-s) % s;
    while (l < t) { m = (u128)next64(r) * s; l = (u64)m; }
  }
  return (u64)(m >> 64);
}

// ---- Binomial(n, p), exact up to double rounding --------------------------------------
// n*min(p,1-p) < 10: sequential inversion on the pmf (exact). Otherwise Hormann's BTRS
// (transformed rejection with squeeze, 1993), an exact rejection method; the acceptance
// test uses Stirling tail corrections and log1p ratio forms so that it stays accurate to
// ~1e-12 absolute even at n = 2^40 (no lgamma differences of 1e13-sized numbers).
static double fc(double k) {  // log(k!) - Stirling(k)
  static const double t[10] = {0.08106146679532726, 0.04134069595540929, 0.02767792568499834,
                               0.02079067210376509, 0.01664469118982119, 0.01387612882307075,
                               0.01189670994589177, 0.01041126526197209, 0.009255462182712733,
                               0.008330563433362871};
  if (k < 10) return t[(int)k];
  double r = 1.0 / (k + 1), r2 = r * r;
  return (1.0 / 12 - (1.0 / 360 - r2 / 1260) * r2) * r;
}
static u64 binom_inv(rng_t *r, u64 n, double p) {
  double q = 1 - p, s = p / q, f = pow(q, (double)n), u = unif(r);
  u64 k = 0;
  while (u > f && k < n) { u -= f; f *= s * (double)(n - k) / (double)(k + 1); k++; }
  return k;
}
static u64 binom_btrs(rng_t *r, u64 n, double p) {
  double nd = (double)n, spq = sqrt(nd * p * (1 - p));
  double b = 1.15 + 2.53 * spq, a = -0.0873 + 0.0248 * b + 0.01 * p, c = nd * p + 0.5;
  double vr = 0.92 - 4.2 / b, alpha = (2.83 + 5.1 / b) * spq;
  double m = floor((nd + 1) * p);
  for (;;) {
    double u = unif(r) - 0.5, v = unif(r), us = 0.5 - fabs(u);
    double kd = floor((2 * a / us + b) * u + c);
    if (kd < 0 || kd > nd) continue;
    if (us >= 0.07 && v <= vr) return (u64)kd;
    v = log(v * alpha / (a / (us * us) + b));
    // log( f(k)/f(m) ) for the binomial pmf in Stirling form. Every log of a near-1 ratio is
    // written as log1p of an exactly computed residual (fma), so nothing of size n*eps survives.
    double nm = nd - m, nk = nd - kd, q = 1 - p;
    double d1 = fma(p, nd + 2, -(m + 1)) / q, d3 = fma(p, nd + 2, -(kd + 1)) / q;
    double ub = -(m + 0.5) * log1p(d1 / (m + 1))
              + (nd + 1) * log1p((kd - m) / (nk + 1))
              + (kd + 0.5) * log1p(d3 / (kd + 1))
              + fc(m) + fc(nm) - fc(kd) - fc(nk);
    if (v <= ub) return (u64)kd;
  }
}
static u64 binom(rng_t *r, u64 n, double p) {
  if (n == 0 || p <= 0) return 0;
  if (p >= 1) return n;
  if (p > 0.5) return n - binom(r, n, 1 - p);
  if ((double)n * p < 10) return binom_inv(r, n, p);
  return binom_btrs(r, n, p);
}

// ---- Column tree ----------------------------------------------------------------------
typedef struct { u128 a, b; u64 n; } node;
typedef struct { u64 seed; } col;
static u64 V_split, V_leaf, K_gen;  // node expansions, leaf materializations, keys generated

static inline int is_leaf(node x) { return x.n <= LEAF || x.b - x.a == 1; }
static void split(const col *c, node x, node *L, node *R) {
  u128 m = x.a + (x.b - x.a) / 2;
  double p = (double)(m - x.a) / (double)(x.b - x.a);
  rng_t r = node_rng(c->seed, x.a, x.b, 0xB1);
  u64 k = binom(&r, x.n, p);
  *L = (node){x.a, m, k};
  *R = (node){m, x.b, x.n - k};
  V_split++;
}
static void isort(u64 *v, int n) {
  for (int i = 1; i < n; i++) {
    u64 t = v[i]; int j = i;
    while (j > 0 && v[j - 1] > t) { v[j] = v[j - 1]; j--; }
    v[j] = t;
  }
}
// Leaf keys (requires n <= LEAF; width-1 leaves are handled by callers as a run).
static int leaf_keys(const col *c, node x, u64 *out) {
  rng_t r = node_rng(c->seed, x.a, x.b, 0x1EAF);
  for (u64 i = 0; i < x.n; i++) out[i] = (u64)x.a + bounded(&r, x.b - x.a);
  isort(out, (int)x.n);
  V_leaf++; K_gen += x.n;
  return (int)x.n;
}

// Materialize the whole column in order (for verification).
static void materialize(const col *c, node x, u64 *out, u64 *pos) {
  if (x.n == 0) return;
  if (x.b - x.a == 1) { for (u64 i = 0; i < x.n; i++) out[(*pos)++] = (u64)x.a; V_leaf++; K_gen += x.n; return; }
  if (x.n <= LEAF) { *pos += leaf_keys(c, x, out + *pos); return; }
  node L, R; split(c, x, &L, &R);
  materialize(c, L, out, pos); materialize(c, R, out, pos);
}
// Same, right subtree first (a different traversal; output is then sorted independently).
static void materialize_rev(const col *c, node x, u64 *out, u64 *pos) {
  if (x.n == 0) return;
  if (is_leaf(x)) { materialize(c, x, out, pos); return; }
  node L, R; split(c, x, &L, &R);
  materialize_rev(c, R, out, pos); materialize_rev(c, L, out, pos);
}

// ---- (a) range count / sum over [qa, qb) ----------------------------------------------
static u64 range_count(const col *c, node x, u128 qa, u128 qb) {
  if (x.n == 0 || x.b <= qa || qb <= x.a) return 0;
  if (qa <= x.a && x.b <= qb) return x.n;
  if (x.b - x.a == 1) return x.n;
  if (x.n <= LEAF) {
    u64 k[LEAF]; int n = leaf_keys(c, x, k); u64 s = 0;
    for (int i = 0; i < n; i++) s += (k[i] >= qa && k[i] < qb);
    return s;
  }
  node L, R; split(c, x, &L, &R);
  return range_count(c, L, qa, qb) + range_count(c, R, qa, qb);
}
static u128 range_sum(const col *c, node x, u128 qa, u128 qb) {
  if (x.n == 0 || x.b <= qa || qb <= x.a) return 0;
  if (x.b - x.a == 1) return (u128)x.n * x.a;
  if (x.n <= LEAF) {
    u64 k[LEAF]; int n = leaf_keys(c, x, k); u128 s = 0;
    for (int i = 0; i < n; i++) if (k[i] >= qa && k[i] < qb) s += k[i];
    return s;
  }
  node L, R; split(c, x, &L, &R);
  return range_sum(c, L, qa, qb) + range_sum(c, R, qa, qb);
}

// ---- (b) equi-join count by simultaneous descent --------------------------------------
// count = sum_k cntA(k) * cntB(k). Subtrees with zero count (or disjoint ranges) are pruned.
// probe: sorted keys k[0..n) (multiset, each with weight 1) against node y of column cy.
static u128 probe(const col *cy, node y, const u64 *k, int n) {
  // restrict keys to [y.a, y.b)
  int i = 0, j = n;
  while (i < j && k[i] < y.a) i++;
  while (j > i && (u128)k[j - 1] >= y.b) j--;
  if (i == j || y.n == 0) return 0;
  k += i; n = j - i;
  if (y.b - y.a == 1) return (u128)n * y.n;
  if (y.n <= LEAF) {
    u64 z[LEAF]; int m = leaf_keys(cy, y, z);
    u128 s = 0; int p = 0, q = 0;
    while (p < n && q < m) {
      if (k[p] < z[q]) p++;
      else if (k[p] > z[q]) q++;
      else {
        u64 v = k[p]; int a = 0, b = 0;
        while (p < n && k[p] == v) { p++; a++; }
        while (q < m && z[q] == v) { q++; b++; }
        s += (u128)a * b;
      }
    }
    return s;
  }
  node L, R; split(cy, y, &L, &R);
  int c = 0; while (c < n && (u128)k[c] < L.b) c++;
  return probe(cy, L, k, c) + probe(cy, R, k + c, n - c);
}
static u128 join(const col *ca, node x, const col *cb, node y) {
  if (x.n == 0 || y.n == 0 || x.b <= y.a || y.b <= x.a) return 0;
  if (x.b - x.a == 1) { u64 v = (u64)x.a; return (u128)x.n * range_count(cb, y, v, (u128)v + 1); }
  if (y.b - y.a == 1) { u64 v = (u64)y.a; return (u128)y.n * range_count(ca, x, v, (u128)v + 1); }
  if (x.n <= LEAF) { u64 k[LEAF]; int n = leaf_keys(ca, x, k); return probe(cb, y, k, n); }
  if (y.n <= LEAF) { u64 k[LEAF]; int n = leaf_keys(cb, y, k); return probe(ca, x, k, n); }
  node xl, xr, yl, yr;
  if (x.a == y.a && x.b == y.b) {  // aligned: split both
    split(ca, x, &xl, &xr); split(cb, y, &yl, &yr);
    return join(ca, xl, cb, yl) + join(ca, xr, cb, yr);
  }
  if (x.b - x.a >= y.b - y.a) { split(ca, x, &xl, &xr); return join(ca, xl, cb, y) + join(ca, xr, cb, y); }
  split(cb, y, &yl, &yr);
  return join(ca, x, cb, yl) + join(ca, x, cb, yr);
}

// ---- helpers ---------------------------------------------------------------------------
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static void pu128(char *buf, u128 v) {
  char t[48]; int i = 0;
  do { t[i++] = '0' + (int)(v % 10); v /= 10; } while (v);
  for (int j = 0; j < i; j++) buf[j] = t[i - 1 - j];
  buf[i] = 0;
}
static u128 W_of(const char *s) { u64 w = strtoull(s, 0, 0); return w ? (u128)w : ((u128)1 << 64); }
static void reset(void) { V_split = V_leaf = K_gen = 0; }
static void radix_sort(u64 *v, u64 n) {
  u64 *t = malloc(n * 8);
  for (int sh = 0; sh < 64; sh += 16) {
    static u64 cnt[65537];
    memset(cnt, 0, sizeof cnt);
    for (u64 i = 0; i < n; i++) cnt[((v[i] >> sh) & 0xffff) + 1]++;
    for (int i = 0; i < 65536; i++) cnt[i + 1] += cnt[i];
    for (u64 i = 0; i < n; i++) t[cnt[(v[i] >> sh) & 0xffff]++] = v[i];
    memcpy(v, t, n * 8);
  }
  free(t);
}
static u128 merge_join(const u64 *x, u64 nx, const u64 *y, u64 ny) {
  u64 p = 0, q = 0; u128 s = 0;
  while (p < nx && q < ny) {
    if (x[p] < y[q]) p++;
    else if (x[p] > y[q]) q++;
    else {
      u64 v = x[p], a = 0, b = 0;
      while (p < nx && x[p] == v) { p++; a++; }
      while (q < ny && y[q] == v) { q++; b++; }
      s += (u128)a * b;
    }
  }
  return s;
}

// ---- modes -----------------------------------------------------------------------------
static int verify(u64 N, u128 W, u128 shift) {
  col A = {0xA11CE ^ N}, B = {0xB0B ^ N};
  node ra = {0, W, N}, rb = {shift, shift + W, N};
  if (rb.b > ((u128)1 << 64)) { fprintf(stderr, "range overflow\n"); return 1; }
  char s1[48], s2[48];
  pu128(s1, W); pu128(s2, shift);
  printf("verify N=%llu W=%s shiftB=%s\n", (unsigned long long)N, s1, s2);
  u64 *xa = malloc(N * 8), *xb = malloc(N * 8), *ya = malloc(N * 8), pos;
  int ok = 1;
  // in-order materialization: must be sorted, exactly N values, within range
  pos = 0; materialize(&A, ra, xa, &pos);
  u64 unsorted = 0; for (u64 i = 1; i < pos; i++) unsorted += xa[i - 1] > xa[i];
  int inr = (xa[0] >= ra.a) && ((u128)xa[pos - 1] < ra.b);
  printf("  A in-order: count=%llu (want %llu) out_of_order_pairs=%llu in_range=%d\n",
         (unsigned long long)pos, (unsigned long long)N, (unsigned long long)unsorted, inr);
  ok &= pos == N && unsorted == 0 && inr;
  // reverse traversal + radix sort must reproduce the same multiset
  pos = 0; materialize_rev(&A, ra, ya, &pos); radix_sort(ya, pos);
  int same = pos == N && memcmp(xa, ya, N * 8) == 0;
  printf("  A reverse-traversal+sort identical: %d\n", same); ok &= same;
  pos = 0; materialize_rev(&B, rb, xb, &pos); radix_sort(xb, pos);
  u64 unsb = 0; for (u64 i = 1; i < pos; i++) unsb += xb[i - 1] > xb[i];
  ok &= pos == N && unsb == 0;
  // distribution: chi-square over 1024 equal-width buckets (df = 1023)
  double chi = 0, e = (double)N / 1024; static u64 bk[1024]; memset(bk, 0, sizeof bk);
  for (u64 i = 0; i < N; i++) bk[(u64)(((u128)(xa[i] - (u64)ra.a) * 1024) / W)]++;
  for (int i = 0; i < 1024; i++) chi += (bk[i] - e) * (bk[i] - e) / e;
  double z = (chi - 1023) / sqrt(2 * 1023.0);
  printf("  A chi2(1024 buckets)=%.1f (df 1023, z=%.2f)\n", chi, z); ok &= fabs(z) < 5;
  // (a) range count / sum vs materialized, 20 random ranges
  rng_t r = {0x5EED ^ N}; int rok = 1; u64 vmax = 0;
  for (int t = 0; t < 20; t++) {
    u128 qa = bounded(&r, W), qb = bounded(&r, W);
    if (qa > qb) { u128 tt = qa; qa = qb; qb = tt; }
    reset(); u64 cn = range_count(&A, ra, qa, qb); if (V_split + V_leaf > vmax) vmax = V_split + V_leaf;
    u128 sm = range_sum(&A, ra, qa, qb);
    u64 cr = 0; u128 sr = 0;
    for (u64 i = 0; i < N; i++) if (xa[i] >= qa && xa[i] < qb) { cr++; sr += xa[i]; }
    rok &= cn == cr && sm == sr;
  }
  printf("  range count+sum, 20 random ranges, match materialized: %d (max count visits %llu)\n", rok, (unsigned long long)vmax);
  ok &= rok;
  // (b) join
  double t0 = now(); u128 jm = merge_join(xa, N, xb, N); double tm = now() - t0;
  reset(); t0 = now(); u128 jt = join(&A, ra, &B, rb); double tt = now() - t0;
  pu128(s1, jm); pu128(s2, jt);
  printf("  join: sort-merge=%s tree=%s match=%d visits=%llu (splits %llu leaves %llu keys %llu) tree %.2fs merge %.2fs\n",
         s1, s2, jm == jt, (unsigned long long)(V_split + V_leaf), (unsigned long long)V_split,
         (unsigned long long)V_leaf, (unsigned long long)K_gen, tt, tm);
  ok &= jm == jt;
  printf("  VERIFY %s\n", ok ? "PASS" : "FAIL");
  free(xa); free(xb); free(ya);
  return !ok;
}

static void do_join(u64 N, u128 W) {
  col A = {0xA11CE ^ N}, B = {0xB0B ^ N};
  node ra = {0, W, N}, rb = {0, W, N};
  reset(); double t0 = now(); u128 j = join(&A, ra, &B, rb); double t = now() - t0;
  char s[48], w[48]; pu128(s, j); pu128(w, W);
  printf("join N=%llu W=%s count=%s visits=%llu splits=%llu leaves=%llu keys=%llu secs=%.3f\n",
         (unsigned long long)N, w, s, (unsigned long long)(V_split + V_leaf), (unsigned long long)V_split,
         (unsigned long long)V_leaf, (unsigned long long)K_gen, t);
}

// Estimate for huge N: exact top part to depth k (both columns aligned on the same range),
// then S uniformly sampled depth-k subtree pairs joined fully; scale by 2^k. Unbiased.
static u64 top_visits;
static void descend_top(const col *A, node x, const col *B, node y, int d, int k, node *xs, node *ys, u64 *nn) {
  if (d == k) { xs[*nn] = x; ys[*nn] = y; (*nn)++; return; }
  node xl, xr, yl, yr;
  split(A, x, &xl, &xr); split(B, y, &yl, &yr);
  descend_top(A, xl, B, yl, d + 1, k, xs, ys, nn);
  descend_top(A, xr, B, yr, d + 1, k, xs, ys, nn);
}
static void do_est(u64 N, u128 W, int k, int S) {
  col A = {0xA11CE ^ N}, B = {0xB0B ^ N};
  node ra = {0, W, N}, rb = {0, W, N};
  u64 K = 1ULL << k; node *xs = malloc(K * sizeof(node)), *ys = malloc(K * sizeof(node)); u64 nn = 0;
  reset(); descend_top(&A, ra, &B, rb, 0, k, xs, ys, &nn); top_visits = V_split;
  for (u64 i = 0; i < nn; i++) if (is_leaf(xs[i]) || is_leaf(ys[i])) { fprintf(stderr, "k too deep\n"); exit(1); }
  rng_t r = {0xE57 ^ N}; double sv = 0, sv2 = 0, sj = 0, sk = 0; double t0 = now();
  // sample without replacement (partial Fisher-Yates over indices)
  u64 *idx = malloc(K * 8); for (u64 i = 0; i < K; i++) idx[i] = i;
  for (int s = 0; s < S; s++) {
    u64 j = s + bounded(&r, K - s); u64 tt = idx[s]; idx[s] = idx[j]; idx[j] = tt;
    u64 i = idx[s];
    reset(); u128 jc = join(&A, xs[i], &B, ys[i]);
    double v = (double)(V_split + V_leaf);
    sv += v; sv2 += v * v; sj += (double)jc; sk += (double)K_gen;
  }
  double mv = sv / S, sd = sqrt(fmax(0, sv2 / S - mv * mv)), fpc = sqrt((double)(K - S) / (K - 1));
  double est = top_visits + K * mv, se = K * sd / sqrt(S) * fpc;
  char w[48]; pu128(w, W);
  printf("est N=%llu W=%s k=%d S=%d visits_est=%.6e se=%.2e (rel %.4f) join_count_est=%.4e keys_est=%.4e secs=%.2f\n",
         (unsigned long long)N, w, k, S, est, se, se / est, K * sj / S, K * sk / S, now() - t0);
  free(xs); free(ys); free(idx);
}

static void do_range(u64 N, u128 W) {
  col A = {0xA11CE ^ N}; node ra = {0, W, N}; rng_t r = {0x7A ^ N};
  double sc = 0, mc = 0, ss = 0; int T = 16;
  for (int t = 0; t < T; t++) {
    u128 qa = bounded(&r, W), qb = bounded(&r, W);
    if (qa > qb) { u128 tt = qa; qa = qb; qb = tt; }
    reset(); range_count(&A, ra, qa, qb); double v = V_split + V_leaf; sc += v; if (v > mc) mc = v;
    if (N <= (1ULL << 30)) { reset(); range_sum(&A, ra, qa, qb); ss += V_split + V_leaf; }
  }
  char w[48], sv[48] = "skipped"; pu128(w, W);
  if (N <= (1ULL << 30)) snprintf(sv, sizeof sv, "%.0f", ss / T);
  printf("range N=%llu W=%s count_visits_mean=%.1f count_visits_max=%.0f sum_visits_mean=%s\n",
         (unsigned long long)N, w, sc / T, mc, sv);
}

static void binomtest(void) {
  // moments at several (n, p) and a chi-square against the exact pmf at n=1000, p=0.5
  u64 ns[] = {20, 1000, 1ULL << 20, 1ULL << 40}; double ps[] = {0.5, 0.3, 0.5, 0.5};
  for (int i = 0; i < 4; i++) {
    rng_t r = {12345 + i}; double s = 0, s2 = 0; int T = 1000000;
    double em = ns[i] * ps[i], ev = em * (1 - ps[i]);  // centred to avoid cancellation
    for (int t = 0; t < T; t++) { double x = (double)binom(&r, ns[i], ps[i]) - em; s += x; s2 += x * x; }
    double m = s / T, v = s2 / T - m * m;
    printf("binom n=%llu p=%.2f mean z=%.2f var ratio=%.4f (se %.4f)\n", (unsigned long long)ns[i], ps[i],
           m / sqrt(ev / T), v / ev, sqrt(2.0 / T));
  }
  {
    int n = 1000, T = 4000000; static u64 h[1001]; rng_t r = {777};
    for (int t = 0; t < T; t++) h[binom(&r, n, 0.5)]++;
    double chi = 0; int df = 0;
    for (int k = 0; k <= n; k++) {
      double lp = lgamma(n + 1.0) - lgamma(k + 1.0) - lgamma(n - k + 1.0) + n * log(0.5), e = T * exp(lp);
      if (e < 20) continue;
      chi += (h[k] - e) * (h[k] - e) / e; df++;
    }
    printf("binom n=1000 p=0.5 chi2=%.1f over %d cells (z=%.2f)\n", chi, df, (chi - (df - 1)) / sqrt(2.0 * (df - 1)));
  }
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: see header\n"); return 2; }
  if (!strcmp(argv[1], "verify")) return verify(strtoull(argv[2], 0, 0), W_of(argv[3]), argc > 4 ? strtoull(argv[4], 0, 0) : 0);
  if (!strcmp(argv[1], "join")) { do_join(strtoull(argv[2], 0, 0), W_of(argv[3])); return 0; }
  if (!strcmp(argv[1], "est")) { do_est(strtoull(argv[2], 0, 0), W_of(argv[3]), atoi(argv[4]), atoi(argv[5])); return 0; }
  if (!strcmp(argv[1], "range")) { do_range(strtoull(argv[2], 0, 0), W_of(argv[3])); return 0; }
  if (!strcmp(argv[1], "binomtest")) { binomtest(); return 0; }
  return 2;
}
