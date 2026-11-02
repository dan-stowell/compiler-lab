// Q3: intensional results as piecewise-polynomial columns (journal/phase3-plan.md, Q3 + revision).
//
// A column of N int64 values is P pieces; piece k covers output rows [start, start+len) and holds a
// polynomial f(x), x = row - start, in the Newton (binomial) basis: f(x) = sum_k c[k] * C(x, k).
// Integer-valued polynomials have integer Newton coefficients, so every operation is exact:
//   forward difference  = shift of the coefficients (c[k] <- c[k+1]);
//   inclusive prefix sum: sum_{j<=x} C(j,k) = C(x,k+1) + C(x,k);
//   zip-mul / re-basing  = evaluate at deg+1 points, take forward differences.
// Arithmetic: coefficients and evaluations are __int128; check_piece() asserts
// sum_k |c[k]| * C(len,k) < 2^100 for every piece created, so no intermediate overflows, and every
// value that leaves the model as int64 is range-checked. Data bounds (asserted by the generator):
// price in [1, 2^20), |qty| < 2^19, |price*qty| < 2^39, |running total| < 2^59, piece len < 2^18.
//
// Modes (see run.sh):
//   params  N P seed                     -> lo hi (filter band from the pieces)
//   model   N P seed lo hi [var]         -> (iii) the model, with representation-size trajectory
//   fgen    N P seed lo hi [var]         -> (ii') structure-aware streaming: regenerate rows from
//                                            the pieces, fused chain, 2-pass histogram multiselect
//   fmat    N P seed lo hi               -> (i) fused C over materialized int32 columns, 2-pass
//   discover N P seed lo hi              -> regime (b): build pieces from materialized columns, then model
//   expand  N P seed lo hi model|conv    -> adversarial 5: sorted[i] - original[i], materialized
//   csv     N P seed file                -> pieces for DuckDB
//   verify  N P seed                     -> brute force (materialize + qsort) vs all paths, all variants
//   control iters | calib                -> noise floor (L2-resident loop) | 1 GiB read sweep
// var: 0 headline, 2 = 10^5 point updates to price, 4 = qty with independent breakpoints.
// Perf: if Q3_CTL/Q3_ACK name the fifos of `perf stat --control fifo:CTL,ACK -D -1`, only the
// measured region is counted.
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef __int128 i128;
typedef int64_t i64;
typedef uint64_t u64;
typedef uint32_t u32;
#define MAXD 6
#define DIE(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

// ---------------- perf region control ----------------
static int ctl = -1, ack = -1;
static double t0;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void perf_cmd(const char *c) {
  if (ctl < 0) return;
  if (write(ctl, c, strlen(c)) < 0) perror("ctl");
  char b[16]; if (read(ack, b, sizeof b) < 0) perror("ack");
}
static void region_begin(void) {
  const char *c = getenv("Q3_CTL"), *a = getenv("Q3_ACK");
  if (c && a && ctl < 0) { ctl = open(c, O_WRONLY); ack = open(a, O_RDONLY); }
  perf_cmd("enable\n"); t0 = now();
}
static double region_end(void) { double t = now() - t0; perf_cmd("disable\n"); return t; }

// ---------------- rng ----------------
static u64 rs;
static u64 rnd(void) { u64 z = (rs += 0x9e3779b97f4a7c15ULL); z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL; z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL; return z ^ (z >> 31); }

// ---------------- base data: linear pieces ----------------
typedef struct { i64 s, len, a, b; } Lin;  // value at row s+x is a + b*x
typedef struct { Lin *v; i64 n; } LinCol;

static i64 *gen_breaks(i64 N, i64 P, u64 seed) {
  rs = seed; i64 *s = malloc((P + 1) * sizeof *s), avg = N / P;
  s[0] = 0; s[P] = N;
  for (i64 k = 1; k < P; k++) s[k] = k * N / P + (i64)(rnd() % (u64)(4 * avg / 5 + 1)) - 2 * avg / 5;
  for (i64 k = 0; k < P; k++) if (s[k + 1] - s[k] < 1 || s[k + 1] - s[k] >= (1 << 18)) DIE("bad piece length (need N/P < 2^17)");
  return s;
}
static LinCol gen_price(i64 N, i64 P, u64 seed) {
  i64 *s = gen_breaks(N, P, seed); rs = seed * 7 + 1;
  LinCol c = {malloc(P * sizeof(Lin)), P};
  for (i64 k = 0; k < P; k++) {
    i64 len = s[k + 1] - s[k], b = (i64)(rnd() % 3) - 1, a = (1 << 18) + (i64)(rnd() % (1 << 18));
    c.v[k] = (Lin){s[k], len, a, b};  // a >= 2^18 > len, so price >= 1; price < 2^19 + 2^18
  }
  free(s); return c;
}
// sum_{x=0}^{n-1} (A + Bx)(C + Dx)
static i128 sumprod(i128 A, i128 B, i128 C, i128 D, i128 n) {
  i128 s1 = n * (n - 1) / 2, s2 = (n - 1) * n * (2 * n - 1) / 6;
  return n * A * C + (A * D + B * C) * s1 + B * D * s2;
}
// qty with its own breakpoints; signed; steered by the exact running total so |T| < 2^58.
static LinCol gen_qty(i64 N, i64 P, u64 seed, const LinCol *pr) {
  i64 *s = gen_breaks(N, P, seed); rs = seed * 13 + 5;
  LinCol c = {malloc(P * sizeof(Lin)), P};
  i128 T = 0; i64 ip = 0; const i128 lim = (i128)1 << 57;
  for (i64 k = 0; k < P; k++) {
    i64 len = s[k + 1] - s[k], cc = (i64)(rnd() % ((1 << 17) + 1)) - (1 << 16), e = (i64)(rnd() % 3) - 1;
    if (T > lim) { e = -(i64)(rnd() % 2); cc = -1 - (i64)(rnd() % (1 << 16)); }
    if (T < -lim) { e = (i64)(rnd() % 2); cc = 1 + (i64)(rnd() % (1 << 16)); }
    c.v[k] = (Lin){s[k], len, cc, e};
    for (i64 u = s[k]; u < s[k + 1];) {  // exact contribution of this qty piece to T
      while (pr->v[ip].s + pr->v[ip].len <= u) ip++;
      const Lin *p = &pr->v[ip]; i64 w = p->s + p->len < s[k + 1] ? p->s + p->len : s[k + 1];
      T += sumprod(p->a + p->b * (u - p->s), p->b, cc + e * (u - s[k]), e, w - u);
      u = w;
    }
    if (T > ((i128)1 << 58) || T < -((i128)1 << 58)) DIE("steering failed");
  }
  free(s); return c;
}
// 10^5 point updates (variant 2): each splits a piece into <= 3 linear pieces.
static int cmp_i64(const void *a, const void *b) { i64 x = *(const i64 *)a, y = *(const i64 *)b; return (x > y) - (x < y); }
static LinCol apply_updates(const LinCol *c, i64 N, i64 U, u64 seed) {
  rs = seed * 31 + 7; i64 *idx = malloc(U * sizeof *idx);
  for (i64 k = 0; k < U; k++) idx[k] = (i64)(rnd() % (u64)N);
  qsort(idx, U, sizeof *idx, cmp_i64);
  LinCol o = {malloc((c->n + 2 * U) * sizeof(Lin)), 0}; i64 k = 0;
  for (i64 j = 0; j < c->n; j++) {
    Lin p = c->v[j]; i64 cur = p.s, end = p.s + p.len;
    while (k < U && idx[k] < end) {
      i64 u = idx[k++]; if (k < U && idx[k] == u) continue;  // duplicate index: last wins
      i64 val = 1 + (i64)(rnd() % (1 << 19));
      if (u > cur) o.v[o.n++] = (Lin){cur, u - cur, p.a + p.b * (cur - p.s), p.b};
      o.v[o.n++] = (Lin){u, 1, val, 0}; cur = u + 1;
    }
    if (cur < end) o.v[o.n++] = (Lin){cur, end - cur, p.a + p.b * (cur - p.s), p.b};
  }
  free(idx); return o;
}
static void gen_inputs(i64 N, i64 P, u64 seed, int var, LinCol *pr, LinCol *qt) {
  *pr = gen_price(N, P, seed);
  *qt = gen_qty(N, P, var == 4 ? seed ^ 0x5555 : seed, pr);  // var 4: independent breakpoints
  if (var == 2) { LinCol u = apply_updates(pr, N, 100000, seed); free(pr->v); *pr = u; }
}

// ---------------- the model: piecewise polynomials in the Newton basis ----------------
typedef struct { i64 start, len, src; int deg; i128 c[MAXD + 1]; } Piece;
typedef struct { Piece *p; i64 n, cap, N; } Col;
static u64 model_bytes_rw;  // computed bytes of piece arrays read+written by the model

static void push(Col *c, const Piece *p) {
  if (c->n == c->cap) { c->cap = c->cap ? 2 * c->cap : 1024; c->p = realloc(c->p, c->cap * sizeof(Piece)); }
  c->p[c->n++] = *p;
}
static inline i128 binom_term(i64 x, int k) {  // C(x, k), x >= 0, k <= 4
  switch (k) {
    case 0: return 1;
    case 1: return x;
    case 2: return (i128)x * (x - 1) / 2;
    case 3: return x < (1 << 20) ? (i128)(x * (x - 1) * (x - 2) / 6) : (i128)x * (x - 1) / 2 * (x - 2) / 3;
    case 4: return x < (1 << 15) ? (i128)(x * (x - 1) * (x - 2) * (x - 3) / 24) : (i128)x * (x - 1) / 2 * (x - 2) / 3 * (x - 3) / 4;
  }
  DIE("degree > 4");
}
static inline i128 ev_c(const i128 *c, int d, i64 x) { i128 r = 0; for (int k = 0; k <= d; k++) r += c[k] * binom_term(x, k); return r; }
static inline i128 ev(const Piece *p, i64 x) { return ev_c(p->c, p->deg, x); }
static void from_vals(i128 *y, int d, i128 *c) {  // c[k] = Delta^k y(0)
  for (int k = 0; k <= d; k++) { c[k] = y[0]; for (int j = 0; j < d - k; j++) y[j] = y[j + 1] - y[j]; }
}
static void trim(Piece *p) { while (p->deg > 0 && p->c[p->deg] == 0) p->deg--; }
static void check_piece(const Piece *p) {
  long double b = 0, bin = 1;
  for (int k = 0; k <= p->deg; k++) { b += fabsl((long double)p->c[k]) * bin; bin = bin * (p->len - k) / (k + 1); if (bin < 1) bin = 1; }
  if (b >= 0x1p100L) DIE("overflow bound exceeded");
}
static void rebase(const Piece *p, i64 off, Piece *o) {  // o(x) = p(off + x)
  i128 y[MAXD + 1]; for (int j = 0; j <= p->deg; j++) y[j] = ev(p, off + j);
  o->deg = p->deg; from_vals(y, p->deg, o->c); trim(o);
}
static Col from_lin(const LinCol *l, i64 N) {
  Col c = {0}; c.N = N;
  for (i64 k = 0; k < l->n; k++) { Piece p = {l->v[k].s, l->v[k].len, l->v[k].s, 1, {l->v[k].a, l->v[k].b}}; trim(&p); push(&c, &p); }
  return c;
}
// merge adjacent pieces that continue the same polynomial (and, if src_too, the same affine remap)
static void normalize(Col *c, int src_too) {
  i64 m = 0;
  for (i64 k = 0; k < c->n; k++) {
    if (m > 0) {
      Piece *a = &c->p[m - 1], *b = &c->p[k];
      int same = !src_too || a->src + a->len == b->src, d = a->deg > b->deg ? a->deg : b->deg;
      for (int j = 0; same && j <= d; j++) same = ev(a, a->len + j) == ev(b, j);
      if (same) { a->len += b->len; continue; }
    }
    c->p[m++] = c->p[k];
  }
  c->n = m;
}
static Col zip_mul(const Col *A, const Col *B) {
  Col o = {0}; o.N = A->N; i64 ia = 0, ib = 0;
  for (i64 s = 0; s < A->N;) {
    const Piece *a = &A->p[ia], *b = &B->p[ib];
    i64 ea = a->start + a->len, eb = b->start + b->len, e = ea < eb ? ea : eb;
    Piece p = {s, e - s, s, a->deg + b->deg, {0}}; i128 y[MAXD + 1];
    for (int j = 0; j <= p.deg; j++) y[j] = ev(a, s - a->start + j) * ev(b, s - b->start + j);
    from_vals(y, p.deg, p.c); trim(&p); check_piece(&p); push(&o, &p);
    s = e; if (s == ea) ia++; if (s == eb) ib++;
  }
  normalize(&o, 0); model_bytes_rw += (A->n + B->n + o.n) * sizeof(Piece); return o;
}
static Col prefix_sum(const Col *A) {
  Col o = {0}; o.N = A->N; i128 O = 0;
  for (i64 k = 0; k < A->n; k++) {
    const Piece *a = &A->p[k]; Piece p = {a->start, a->len, a->src, a->deg + 1, {0}};
    p.c[0] = O + a->c[0];
    for (int m = 1; m <= a->deg; m++) p.c[m] = a->c[m] + a->c[m - 1];
    p.c[a->deg + 1] = a->c[a->deg];
    trim(&p); check_piece(&p); push(&o, &p);
    O = ev(&p, p.len - 1);
    if (O >= ((i128)1 << 62) || O <= -((i128)1 << 62)) DIE("running total exceeds int64 bound");
  }
  model_bytes_rw += (A->n + o.n) * sizeof(Piece); return o;
}
// first k in [u, w) with pred(k) (pred monotone false->true); w if none
#define FIRST(u, w, PRED) ({ i64 _l = (u), _h = (w); while (_l < _h) { i64 k = _l + (_h - _l) / 2; if (PRED) _h = k; else _l = k + 1; } _l; })
// Breakpoints inside (a, b) such that f (Newton coefs c, degree d) is monotone on the integer
// points between consecutive breakpoints. Recursion on Delta f (degree d-1): inside a monotone
// segment of Delta f, Delta f changes sign at most once; that point (binary search) and the segment
// boundaries split f. At most 2^d - 1 breakpoints; cost O(2^d log len) evaluations.
static int mono(const i128 *c, int d, i64 a, i64 b, i64 *out) {
  if (d <= 1 || b - a <= 2) return 0;
  const i128 *g = c + 1; int dg = d - 1; i64 gb = b - 1, gbp[64];
  int ng = mono(g, dg, a, gb, gbp), no = 0; i64 u = a;
  for (int s = 0; s <= ng; s++) {
    i64 w = s < ng ? gbp[s] : gb, k = u;
    i128 gu = ev_c(g, dg, u), gw = ev_c(g, dg, w - 1);
    if (gu <= gw && gu <= 0 && gw > 0) k = FIRST(u, w, ev_c(g, dg, k) > 0);
    if (gu > gw && gu >= 0 && gw < 0) k = FIRST(u, w, ev_c(g, dg, k) < 0);
    if (k > u && k < w) out[no++] = k;
    if (s < ng) out[no++] = w;
    u = w;
  }
  return no;
}
static int segments(const Piece *p, i64 *bnd) {  // bnd[0]=0 < ... < bnd[n]=len; returns n
  i64 bp[64]; int nb = mono(p->c, p->deg, 0, p->len, bp);
  bnd[0] = 0; for (int i = 0; i < nb; i++) bnd[i + 1] = bp[i]; bnd[nb + 1] = p->len; return nb + 1;
}
// filter lo <= f <= hi: per monotone segment the kept rows are one interval (two binary searches);
// kept intervals become pieces, re-based, with src = input row (the piecewise-affine index remap).
static Col filter_band(const Col *A, i64 lo, i64 hi) {
  Col o = {0}; i64 outN = 0;
  for (i64 k = 0; k < A->n; k++) {
    const Piece *a = &A->p[k]; i64 bnd[66]; int ns = segments(a, bnd);
    i64 ks = -1, ke = -1;  // current kept interval within the piece
    for (int s = 0; s <= ns; s++) {
      i64 k1 = 0, k2 = 0;
      if (s < ns) {
        i64 x = bnd[s], y = bnd[s + 1];
        if (ev(a, x) <= ev(a, y - 1)) { k1 = FIRST(x, y, ev(a, k) >= lo); k2 = FIRST(x, y, ev(a, k) > hi); }
        else { k1 = FIRST(x, y, ev(a, k) <= hi); k2 = FIRST(x, y, ev(a, k) < lo); }
        if (k1 >= k2) continue;
        if (ks >= 0 && k1 == ke) { ke = k2; continue; }
      }
      if (ks >= 0) {
        Piece p = {outN, ke - ks, a->src + ks, 0, {0}}; rebase(a, ks, &p); check_piece(&p); push(&o, &p); outN += ke - ks;
      }
      ks = k1; ke = k2;
    }
  }
  o.N = outN; normalize(&o, 1); model_bytes_rw += (A->n + o.n) * sizeof(Piece); return o;
}
// D[j-1] = F[j] - F[j-1], j = 1..|F|-1. Inside a piece: Delta f (coefficient shift), degree-1.
// At each piece start: one point piece f_k(0) - f_{k-1}(last).
static Col lag_diff(const Col *F) {
  Col o = {0}; o.N = F->N > 0 ? F->N - 1 : 0;
  for (i64 k = 0; k < F->n; k++) {
    const Piece *f = &F->p[k];
    if (f->start >= 1) {
      const Piece *g = &F->p[k - 1]; Piece p = {f->start - 1, 1, f->start - 1, 0, {ev(f, 0) - ev(g, g->len - 1)}};
      push(&o, &p);
    }
    if (f->len >= 2) {
      Piece p = {f->start, f->len - 1, f->start, f->deg > 0 ? f->deg - 1 : 0, {0}};
      for (int m = 0; m < f->deg; m++) p.c[m] = f->c[m + 1];
      trim(&p); check_piece(&p); push(&o, &p);
    }
  }
  normalize(&o, 0); model_bytes_rw += (F->n + o.n) * sizeof(Piece); return o;
}
// "sort" = the multiset as a merge of monotone segments (the intensional sorted column).
typedef struct { const Piece *p; i64 x, y; int up; i64 mn, mx; } Seg;
typedef struct { Seg *s; i64 n; i64 count; i64 mn, mx; } Sorted;
static Sorted sort_segments(const Col *D) {
  Sorted S = {malloc(D->n * 8 * sizeof(Seg)), 0, D->N, INT64_MAX, INT64_MIN};
  for (i64 k = 0; k < D->n; k++) {
    const Piece *p = &D->p[k]; i64 bnd[66]; int ns = segments(p, bnd);
    for (int s = 0; s < ns; s++) {
      i128 a = ev(p, bnd[s]), b = ev(p, bnd[s + 1] - 1);
      if (a > INT64_MAX || a < INT64_MIN || b > INT64_MAX || b < INT64_MIN) DIE("D exceeds int64");
      Seg g = {p, bnd[s], bnd[s + 1], a <= b, a <= b ? (i64)a : (i64)b, a <= b ? (i64)b : (i64)a};
      if (g.mn < S.mn) S.mn = g.mn;
      if (g.mx > S.mx) S.mx = g.mx;
      S.s[S.n++] = g;
    }
  }
  model_bytes_rw += D->n * sizeof(Piece) + S.n * sizeof(Seg); return S;
}
static i64 count_le(const Sorted *S, i64 v) {
  i64 c = 0;
  for (i64 i = 0; i < S->n; i++) {
    const Seg *g = &S->s[i];
    if (g->mx <= v) c += g->y - g->x;
    else if (g->mn <= v) {
      if (g->up) c += FIRST(g->x, g->y, ev(g->p, k) > v) - g->x;
      else c += g->y - FIRST(g->x, g->y, ev(g->p, k) <= v);
    }
  }
  return c;
}
// Exact select: smallest v with count_le(v) >= r+1, binary search on the value range.
// Cost O(log2(max-min) * S * log len) evaluations per quantile, S = monotone segments.
static i64 select_rank(const Sorted *S, i64 r) {
  i64 lo = S->mn, hi = S->mx;
  while (lo < hi) { i64 mid = (i64)(((i128)lo + hi) >> 1); if (count_le(S, mid) >= r + 1) hi = mid; else lo = mid + 1; }
  return lo;
}
static i64 qrank(i64 n, int k) { return (i64)floor((double)(n - 1) * (k / 10.0)); }  // DuckDB quantile_disc

static void traj(const char *op, const Col *c) {
  int md = 0; u64 bytes = 0;
  for (i64 k = 0; k < c->n; k++) { if (c->p[k].deg > md) md = c->p[k].deg; bytes += 24 + 8 * (c->p[k].deg + 1); }
  printf("traj op=%-12s pieces=%-9ld maxdeg=%d rows=%-11ld bytes=%lu\n", op, (long)c->n, md, (long)c->N, (unsigned long)bytes);
}
typedef struct { i64 n; i64 q[10]; } Res;
static void print_res(const char *who, const Res *r) {
  printf("result %s n=%ld q=", who, (long)r->n);
  for (int k = 1; k <= 9; k++) printf("%ld%c", (long)r->q[k], k < 9 ? ',' : '\n');
}
static Col model_D(const LinCol *pr, const LinCol *qt, i64 N, i64 lo, i64 hi, int verbose) {
  Col P = from_lin(pr, N), Q = from_lin(qt, N);
  if (verbose) { traj("price", &P); traj("qty", &Q); }
  Col V = zip_mul(&P, &Q); if (verbose) traj("zip-mul", &V);
  Col T = prefix_sum(&V); if (verbose) traj("prefix-sum", &T);
  Col F = filter_band(&T, lo, hi); if (verbose) traj("filter", &F);
  Col D = lag_diff(&F); if (verbose) traj("lag-diff", &D);
  free(P.p); free(Q.p); free(V.p); free(T.p); free(F.p);
  return D;
}
static Res model_chain(const LinCol *pr, const LinCol *qt, i64 N, i64 lo, i64 hi, int verbose) {
  Col D = model_D(pr, qt, N, lo, hi, verbose);
  Sorted S = sort_segments(&D);
  if (verbose) printf("traj op=%-12s segments=%-7ld maxdeg=%d rows=%-11ld bytes=%lu\n", "sort(merge)", (long)S.n, 2, (long)S.count, (unsigned long)(S.n * 40));
  Res r = {S.count, {0}};
  for (int k = 1; k <= 9; k++) r.q[k] = select_rank(&S, qrank(r.n, k));
  if (verbose) printf("traj op=%-12s values=9\n", "quantiles");
  free(S.s); free(D.p); return r;
}

// ---------------- conventional fused pipelines: 2-pass histogram multiselect ----------------
// Order-preserving bucket of an int64: sign, exponent, top 10 mantissa bits (55296 per sign).
#define HB 55296
#define NB (2 * HB)
static inline u32 bpos(u64 m) { if (m < 1024) return (u32)m; int e = 63 - __builtin_clzll(m); return 1024 + (u32)(e - 10) * 1024 + (u32)((m >> (e - 10)) & 1023); }
static inline u32 bucket(i64 d) { return d >= 0 ? HB + bpos((u64)d) : HB - 1 - bpos((u64)(-(d + 1))); }
typedef struct { u32 *hist; i64 n; unsigned char *tgt; i64 *buf[10]; i64 fill[10]; } Sink;
static inline __attribute__((always_inline)) void sink(Sink *s, int pass, i64 d) {
  u32 b = bucket(d);
  if (pass == 1) s->hist[b]++;
  else { unsigned t = s->tgt[b]; if (t) s->buf[t - 1][s->fill[t - 1]++] = d; }
}
// Fused chain body. src: 0 = regenerate from linear pieces (structure-aware), 1 = materialized arrays.
static inline __attribute__((always_inline)) i64 run_pass(int pass, int src, const LinCol *pr, const LinCol *qt,
                                                         const int32_t *pa, const int32_t *qa, i64 N, i64 lo, i64 hi, Sink *s) {
  i64 T = 0, prev = 0, nk = 0; int have = 0;
  const u64 span = (u64)hi - (u64)lo;
  if (src == 1) {
    for (i64 i = 0; i < N; i++) {
      T += (i64)pa[i] * qa[i];
      if ((u64)T - (u64)lo <= span) { if (have) sink(s, pass, T - prev); prev = T; have = 1; nk++; }
    }
    return nk;
  }
  i64 ip = 0, iq = 0;
  for (i64 i = 0; i < N;) {
    const Lin *p = &pr->v[ip], *q = &qt->v[iq];
    i64 pe = p->s + p->len, qe = q->s + q->len, w = pe < qe ? pe : qe;
    i64 pv = p->a + p->b * (i - p->s), qv = q->a + q->b * (i - q->s), pb = p->b, qb = q->b;
    for (; i < w; i++) {
      T += pv * qv; pv += pb; qv += qb;
      if ((u64)T - (u64)lo <= span) { if (have) sink(s, pass, T - prev); prev = T; have = 1; nk++; }
    }
    if (i == pe) ip++;
    if (i == qe) iq++;
  }
  return nk;
}
static i64 quickselect(i64 *a, i64 n, i64 r) {
  i64 l = 0, h = n - 1;
  while (l < h) {
    i64 x = a[l + (h - l) / 2], i = l, j = h;
    while (i <= j) { while (a[i] < x) i++; while (a[j] > x) j--; if (i <= j) { i64 t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; } }
    if (r <= j) h = j; else if (r >= i) l = i; else return a[r];
  }
  return a[r];
}
static u64 fused_bytes_w;
static Res fused(int src, const LinCol *pr, const LinCol *qt, const int32_t *pa, const int32_t *qa, i64 N, i64 lo, i64 hi) {
  Sink s = {calloc(NB, sizeof(u32)), 0, calloc(NB, 1), {0}, {0}};
  i64 nk = src ? run_pass(1, 1, pr, qt, pa, qa, N, lo, hi, &s) : run_pass(1, 0, pr, qt, pa, qa, N, lo, hi, &s);
  Res r = {nk > 0 ? nk - 1 : 0, {0}};
  i64 rb[10]; int nt = 0; i64 bstart[10];
  for (int k = 1; k <= 9; k++) rb[k] = qrank(r.n, k);
  int k = 1; i64 cum = 0;
  for (u32 b = 0; b < NB && k <= 9; b++) {
    while (k <= 9 && rb[k] < cum + s.hist[b]) {
      if (!s.tgt[b]) { s.tgt[b] = (unsigned char)(++nt); bstart[nt - 1] = cum; s.buf[nt - 1] = malloc((s.hist[b] + 1) * sizeof(i64)); fused_bytes_w += s.hist[b] * 8; }
      k++;
    }
    cum += s.hist[b];
  }
  if (src) run_pass(2, 1, pr, qt, pa, qa, N, lo, hi, &s); else run_pass(2, 0, pr, qt, pa, qa, N, lo, hi, &s);
  for (k = 1; k <= 9; k++) {
    int b;  // the candidate bucket that holds rank rb[k]
    for (b = 0; b < nt; b++) if (rb[k] >= bstart[b] && rb[k] < bstart[b] + s.fill[b]) break;
    if (b == nt) DIE("multiselect bucket missing");
    r.q[k] = quickselect(s.buf[b], s.fill[b], rb[k] - bstart[b]);
  }
  fused_bytes_w += NB * sizeof(u32);
  for (int b = 0; b < nt; b++) free(s.buf[b]);
  free(s.hist); free(s.tgt); return r;
}
static void materialize(const LinCol *c, int32_t *a) {
  for (i64 k = 0; k < c->n; k++) { const Lin *p = &c->v[k]; for (i64 x = 0; x < p->len; x++) a[p->s + x] = (int32_t)(p->a + p->b * x); }
}

// ---------------- regime (b): discover linear pieces from materialized columns ----------------
static LinCol discover_lin(const int32_t *a, i64 N) {
  LinCol c = {malloc(1024 * sizeof(Lin)), 0}; i64 cap = 1024;
  for (i64 s = 0; s < N;) {
    i64 b = s + 1 < N ? (i64)a[s + 1] - a[s] : 0, v = a[s], i = s + 1;
    while (i < N && (v += b, a[i] == v)) i++;
    if (c.n == cap) { cap *= 2; c.v = realloc(c.v, cap * sizeof(Lin)); }
    c.v[c.n++] = (Lin){s, i - s, a[s], b}; s = i;
  }
  return c;
}

// ---------------- adversarial 5: sorted[i] - original[i] ----------------
static void radix_sort(i64 *a, i64 *tmp, i64 n) {
  u64 (*h)[256] = calloc(8, sizeof *h);
  for (i64 i = 0; i < n; i++) { u64 u = (u64)a[i] ^ (1ULL << 63); for (int d = 0; d < 8; d++) h[d][(u >> (8 * d)) & 255]++; }
  i64 *src = a, *dst = tmp;
  for (int d = 0; d < 8; d++) {
    int trivial = 0; for (int b = 0; b < 256; b++) if (h[d][b] == (u64)n) trivial = 1;
    if (trivial) continue;
    u64 off[256], c = 0; for (int b = 0; b < 256; b++) { off[b] = c; c += h[d][b]; }
    for (i64 i = 0; i < n; i++) { u64 u = (u64)src[i] ^ (1ULL << 63); dst[off[(u >> (8 * d)) & 255]++] = src[i]; }
    i64 *t = src; src = dst; dst = t;
  }
  if (src != a) memcpy(a, src, n * sizeof(i64));
  free(h);
}
// Greedy minimum number of exact pieces of degree <= d (greedy is optimal: sub-runs of a valid run are valid).
typedef struct { int d; i64 pieces, run; i128 w[6]; } Greedy;
static inline void greedy_push(Greedy *g, i64 v) {
  if (g->run == 0) { g->pieces++; g->run = 1; g->w[0] = v; return; }
  if (g->run <= g->d) { g->w[g->run++] = v; return; }
  // window w[0..d] holds the last d+1 values; extend iff the (d+1)-th difference is 0
  i128 t[6]; for (int j = 0; j <= g->d; j++) t[j] = g->w[j]; t[g->d + 1] = v;
  for (int k = 0; k <= g->d; k++) for (int j = 0; j <= g->d - k; j++) t[j] = t[j + 1] - t[j];
  if (t[0] != 0) { g->pieces++; g->run = 1; g->w[0] = v; return; }
  for (int j = 0; j < g->d; j++) g->w[j] = g->w[j + 1];
  g->w[g->d] = v; g->run++;
}
static i64 enumerate_D(const Col *D, i64 *out) {  // exact, Newton forward stepping
  i64 n = 0;
  for (i64 k = 0; k < D->n; k++) {
    const Piece *p = &D->p[k]; i128 y[MAXD + 1]; for (int j = 0; j <= p->deg; j++) y[j] = p->c[j];
    for (i64 x = 0; x < p->len; x++) { out[n++] = (i64)y[0]; for (int j = 0; j < p->deg; j++) y[j] += y[j + 1]; }
  }
  return n;
}

// ---------------- brute force (verify) ----------------
static i64 val_at(const LinCol *c, i64 i) {
  i64 l = 0, h = c->n - 1; while (l < h) { i64 m = (l + h + 1) / 2; if (c->v[m].s <= i) l = m; else h = m - 1; }
  return c->v[l].a + c->v[l].b * (i - c->v[l].s);
}

static void params(i64 N, i64 P, u64 seed, i64 *lo, i64 *hi) {
  LinCol pr, qt; gen_inputs(N, P, seed, 0, &pr, &qt);
  Col A = from_lin(&pr, N), B = from_lin(&qt, N), V = zip_mul(&A, &B), T = prefix_sum(&V);
  i128 mn = 0, mx = 0;
  for (i64 k = 0; k < T.n; k++) {
    i64 bnd[66]; int ns = segments(&T.p[k], bnd);
    for (int s = 0; s <= ns; s++) { i128 v = ev(&T.p[k], s < ns ? bnd[s] : bnd[ns] - 1); if (v < mn) mn = v; if (v > mx) mx = v; }
  }
  *lo = (i64)(mn + (mx - mn) * 2 / 5); *hi = (i64)(mn + (mx - mn) * 3 / 5);
}

int main(int argc, char **argv) {
  if (argc < 2) DIE("usage: see header");
  const char *m = argv[1];
  if (!strcmp(m, "control")) {  // L2-resident loop: the VM's DRAM noise floor per cycle
    i64 it = atoll(argv[2]); size_t n = 256 * 1024 / 8; volatile u64 *a = calloc(n, 8); u64 s = 0;
    region_begin(); for (i64 r = 0; r < it; r++) for (size_t i = 0; i < n; i++) s += a[i];
    double t = region_end(); printf("control iters=%ld sum=%lu wall=%.2fs\n", (long)it, (unsigned long)s, t); return 0;
  }
  if (!strcmp(m, "calib")) {
    size_t n = (1UL << 30) / 8; u64 *a = malloc(n * 8); for (size_t i = 0; i < n; i++) a[i] = i;
    u64 s = 0; region_begin(); for (size_t i = 0; i < n; i++) s += a[i]; region_end();
    printf("calib read 1073741824 B sum=%lu\n", (unsigned long)s); return 0;
  }
  if (argc < 5) DIE("usage: see header");
  i64 N = atoll(argv[2]), P = atoll(argv[3]); u64 seed = strtoull(argv[4], 0, 10);
  if (!strcmp(m, "params")) { i64 lo, hi; params(N, P, seed, &lo, &hi); printf("%ld %ld\n", (long)lo, (long)hi); return 0; }
  if (!strcmp(m, "csv")) {
    LinCol pr, qt; gen_inputs(N, P, seed, 0, &pr, &qt); FILE *f = fopen(argv[5], "w");
    fprintf(f, "s,len,pa,pb,qc,qe\n");
    for (i64 k = 0; k < P; k++) fprintf(f, "%ld,%ld,%ld,%ld,%ld,%ld\n", (long)pr.v[k].s, (long)pr.v[k].len, (long)pr.v[k].a, (long)pr.v[k].b, (long)qt.v[k].a, (long)qt.v[k].b);
    fclose(f); return 0;
  }
  if (!strcmp(m, "verify")) {
    int bad = 0;
    for (int var = 0; var <= 4; var += 2) {
      i64 lo, hi; params(N, P, seed, &lo, &hi);
      LinCol pr, qt; gen_inputs(N, P, seed, var, &pr, &qt);
      int32_t *pa = malloc(N * 4), *qa = malloc(N * 4); i64 *D = malloc(N * 8), nd = 0, T = 0, prev = 0; int have = 0;
      for (i64 i = 0; i < N; i++) { pa[i] = (int32_t)val_at(&pr, i); qa[i] = (int32_t)val_at(&qt, i); }
      for (i64 i = 0; i < N; i++) { T += (i64)pa[i] * qa[i]; if (T >= lo && T <= hi) { if (have) D[nd++] = T - prev; prev = T; have = 1; } }
      Col MD = model_D(&pr, &qt, N, lo, hi, 0); i64 *E = malloc(N * 8), ne = enumerate_D(&MD, E);
      int same = ne == nd; for (i64 i = 0; same && i < nd; i++) same = E[i] == D[i];
      qsort(D, nd, 8, cmp_i64); Res b = {nd, {0}}; for (int k = 1; k <= 9; k++) b.q[k] = D[qrank(nd, k)];
      Res rm = model_chain(&pr, &qt, N, lo, hi, 0), rg = fused(0, &pr, &qt, 0, 0, N, lo, hi), rf = fused(1, 0, 0, pa, qa, N, lo, hi);
      int ok = same && !memcmp(&b, &rm, sizeof b) && !memcmp(&b, &rg, sizeof b) && !memcmp(&b, &rf, sizeof b);
      printf("verify N=%ld P=%ld seed=%lu var=%d n=%ld D-elementwise=%s model/fgen/fmat=%s\n", (long)N, (long)P, (unsigned long)seed, var, (long)nd,
             same ? "ok" : "MISMATCH", ok ? "ok" : "MISMATCH");
      if (!ok) { print_res("brute", &b); print_res("model", &rm); print_res("fgen", &rg); print_res("fmat", &rf); bad = 1; }
      free(pa); free(qa); free(D); free(E); free(MD.p); free(pr.v); free(qt.v);
    }
    return bad;
  }
  if (argc < 7) DIE("need lo hi");
  i64 lo = atoll(argv[5]), hi = atoll(argv[6]); int var = argc > 7 ? atoi(argv[7]) : 0;
  if (!strcmp(m, "model") || !strcmp(m, "fgen")) {
    LinCol pr, qt; gen_inputs(N, P, seed, var, &pr, &qt);
    printf("inputs var=%d price_pieces=%ld qty_pieces=%ld\n", var, (long)pr.n, (long)qt.n);
    int mod = !strcmp(m, "model");
    region_begin();
    Res r = mod ? model_chain(&pr, &qt, N, lo, hi, 1) : fused(0, &pr, &qt, 0, 0, N, lo, hi);
    double t = region_end();
    print_res(m, &r);
    if (mod) printf("computed model piece bytes read+written=%lu wall=%.3fs\n", (unsigned long)model_bytes_rw, t);
    else printf("computed DRAM read=0 (rows regenerated) write<=%lu (histogram + candidate buffers) wall=%.3fs\n", (unsigned long)fused_bytes_w, t);
    return 0;
  }
  if (!strcmp(m, "fmat") || !strcmp(m, "discover")) {
    LinCol pr, qt; gen_inputs(N, P, seed, 0, &pr, &qt);
    int32_t *pa = malloc(N * 4), *qa = malloc(N * 4); materialize(&pr, pa); materialize(&qt, qa);
    Res r; double t;
    if (!strcmp(m, "fmat")) {
      region_begin(); r = fused(1, 0, 0, pa, qa, N, lo, hi); t = region_end();
      print_res(m, &r);
      printf("computed DRAM read=%ld (2 passes x 8 B/row) write<=%lu wall=%.3fs\n", (long)(16 * N), (unsigned long)fused_bytes_w, t);
    } else {
      region_begin();
      LinCol dp = discover_lin(pa, N), dq = discover_lin(qa, N);
      r = model_chain(&dp, &dq, N, lo, hi, 0); t = region_end();
      print_res(m, &r);
      printf("discovered price_pieces=%ld qty_pieces=%ld computed DRAM read=%ld (1 pass x 8 B/row) wall=%.3fs\n", (long)dp.n, (long)dq.n, (long)(8 * N), t);
    }
    return 0;
  }
  if (!strcmp(m, "expand")) {
    int conv = argc > 7 && !strcmp(argv[7], "conv");
    LinCol pr, qt; gen_inputs(N, P, seed, 0, &pr, &qt);
    Col MD = model_D(&pr, &qt, N, lo, hi, 0); i64 n = MD.N;
    int32_t *pa = 0, *qa = 0;
    if (conv) { pa = malloc(N * 4); qa = malloc(N * 4); materialize(&pr, pa); materialize(&qt, qa); }
    i64 *S = malloc(n * 8), *tmp = malloc(n * 8), *Dm = conv ? malloc(n * 8) : 0;
    Greedy ge2 = {.d = 2}, ge3 = {.d = 3}, gs2 = {.d = 2}, gd2 = {.d = 2};
    region_begin();
    if (conv) {  // D from materialized columns, copy, sort, subtract
      i64 T = 0, prev = 0, nd = 0; int have = 0;
      for (i64 i = 0; i < N; i++) { T += (i64)pa[i] * qa[i]; if (T >= lo && T <= hi) { if (have) Dm[nd++] = T - prev; prev = T; have = 1; } }
      memcpy(S, Dm, n * 8); radix_sort(S, tmp, n);
      for (i64 i = 0; i < n; i++) { i64 e = S[i] - Dm[i]; greedy_push(&ge2, e); greedy_push(&ge3, e); greedy_push(&gs2, S[i]); greedy_push(&gd2, Dm[i]); }
    } else {  // model: enumerate D from pieces, sort, subtract D regenerated from pieces
      enumerate_D(&MD, S); radix_sort(S, tmp, n);
      i64 i = 0;
      for (i64 k = 0; k < MD.n; k++) {
        const Piece *p = &MD.p[k]; i128 y[MAXD + 1]; for (int j = 0; j <= p->deg; j++) y[j] = p->c[j];
        for (i64 x = 0; x < p->len; x++, i++) {
          i64 d = (i64)y[0], e = S[i] - d; for (int j = 0; j < p->deg; j++) y[j] += y[j + 1];
          greedy_push(&ge2, e); greedy_push(&ge3, e); greedy_push(&gs2, S[i]); greedy_push(&gd2, d);
        }
      }
    }
    double t = region_end();
    printf("expand %s n=%ld model_D_pieces=%ld greedy_pieces: D(deg<=2)=%ld sorted(deg<=2)=%ld sorted-original(deg<=2)=%ld (deg<=3)=%ld wall=%.2fs\n",
           conv ? "conv" : "model", (long)n, (long)MD.n, (long)gd2.pieces, (long)gs2.pieces, (long)ge2.pieces, (long)ge3.pieces, t);
    printf("computed DRAM %s: read~%ld write~%ld (radix passes not included)\n", conv ? "conv" : "model",
           (long)(conv ? 8 * N + 8 * n + 16 * n : 8 * n), (long)(conv ? 8 * n + 8 * n : 8 * n));
    return 0;
  }
  DIE("unknown mode %s", m);
}
