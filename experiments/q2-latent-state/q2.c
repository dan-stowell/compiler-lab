// Q2: shared latent state (exact low-rank collection). See journal/phase3-plan.md (Q2 + revision).
// X = U·Vᵀ, N = 2^LOGN rows, M = 1024 columns, r = 8, all integers: U, V in [-8, 8], |X| <= 512.
// Chain: scale columns (w in 1..4) -> select nS = 512 columns -> centre -> covariance -> top-K = 4
// principal directions -> project rows -> top-100 rows by squared projection norm.
//   conv   (i)   materialized X (int32, 4 GiB): pass 1 = exact XᵀX + column sums (int16 VPMADDWD,
//                int64 flush), subspace iteration on the in-cache covariance, pass 2 = projections.
//   fact   (ii)  factor-aware linear algebra on (U, V): UᵀU + Uᵀ1 (pass 1 over U), 8x8 eigenproblem,
//                scores = (U - 1ū)·(V'ᵀD) (pass 2 over U).
//   model  (iii) the latent-state API: each op rewrites (U, V, lazy centring) by a composition rule,
//                logging representation size. Same arithmetic as (ii).
//   recover regime (b): one pass over X recovers exact factors (skeleton X = X[:,J] A⁻¹ X[I,:]) with
//                Freivalds verification of every row, then runs the model chain on them.
//   reluconv / relumodel: adversarial op 5, ReLU(X') then covariance, over X vs regenerated from U.
//   verify: runs all and checks agreement. adv: adversarial size counts. control: noise floor.
#define _GNU_SOURCE
#include <fcntl.h>
#include <immintrin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define M 1024
#define R 8
#define NS 512
#define K 4
#define TOPN 100
#define MAXR 32
typedef __int128 i128;

static int LOGN = 20;
static long N;
static int S[NS];          // selected columns, ascending
static int32_t Wt[M];      // column weights
static int ctl = -1, ack = -1;
static void perf_cmd(const char *c) {
  if (ctl < 0) return;
  if (write(ctl, c, strlen(c)) < 0) perror("ctl");
  char b[16];
  if (read(ack, b, sizeof b) < 0) perror("ack");
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static uint64_t sm(uint64_t x) {
  x += 0x9e3779b97f4a7c15ull; x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull; return x ^ (x >> 31);
}
static void *big(size_t n) {
  void *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { perror("mmap"); exit(1); }
  madvise(p, n, MADV_HUGEPAGE); memset(p, 0, n); return p;
}

// ---------- data ----------
static void gen_factors(uint64_t seed, int32_t *U, int32_t *Vt) {  // U: N×R row-major, Vt: R×M
  for (long i = 0; i < N * R; i++) U[i] = (int32_t)(sm(seed ^ (2 * i + 1)) % 17) - 8;
  for (long t = 0; t < R; t++) for (long j = 0; j < M; j++) Vt[t * M + j] = (int32_t)(sm(~seed ^ (t * M + j) * 3) % 17) - 8;
}
static void materialize(const int32_t *U, const int32_t *Vt, int32_t *X) {
  for (long i = 0; i < N; i++) {
    int32_t *x = X + i * M; const int32_t *u = U + i * R;
    for (int j = 0; j < M; j++) x[j] = 0;
    for (int t = 0; t < R; t++) { int32_t a = u[t]; const int32_t *v = Vt + t * M; for (int j = 0; j < M; j++) x[j] += a * v[j]; }
  }
}
static void setup_query(uint64_t seed) {  // nS distinct columns and weights 1..4
  int perm[M];
  for (int j = 0; j < M; j++) perm[j] = j;
  for (int j = M - 1; j > 0; j--) { int k = sm(seed + j) % (j + 1); int t = perm[j]; perm[j] = perm[k]; perm[k] = t; }
  char in[M] = {0};
  for (int j = 0; j < NS; j++) in[perm[j]] = 1;
  for (int j = 0, n = 0; j < M; j++) if (in[j]) S[n++] = j;
  for (int j = 0; j < M; j++) Wt[j] = 1 + sm(seed * 7 + j) % 4;
}
static void flush_caches(void) {  // 256 MiB streaming read evicts the 32 MiB L3 (outside measured region)
  static volatile uint64_t *b; static uint64_t sink;
  size_t n = 256ul << 20;
  if (!b) { b = big(n); for (size_t i = 0; i < n / 8; i++) b[i] = i; }
  uint64_t s = 0; for (size_t i = 0; i < n / 8; i += 8) s += b[i];
  sink += s;
}

// ---------- results ----------
typedef struct { double lam[K]; double D[K][NS]; long top[TOPN]; double score[TOPN]; i128 *Ci; } Res;
typedef struct { double s; long i; } Ent;
static int better(Ent a, Ent b) { return a.s > b.s || (a.s == b.s && a.i < b.i); }
typedef struct { Ent h[TOPN]; int n; } Heap;  // min-heap: root = worst kept
static void hpush(Heap *H, double s, long i) {
  Ent e = {s, i};
  if (H->n < TOPN) {
    int c = H->n++; H->h[c] = e;
    while (c && better(H->h[(c - 1) / 2], H->h[c])) { Ent t = H->h[c]; H->h[c] = H->h[(c - 1) / 2]; H->h[(c - 1) / 2] = t; c = (c - 1) / 2; }
    return;
  }
  if (!better(e, H->h[0])) return;
  H->h[0] = e; int c = 0;
  for (;;) {
    int l = 2 * c + 1, r = l + 1, m = c;
    if (l < TOPN && better(H->h[m], H->h[l])) m = l;
    if (r < TOPN && better(H->h[m], H->h[r])) m = r;
    if (m == c) break;
    Ent t = H->h[c]; H->h[c] = H->h[m]; H->h[m] = t; c = m;
  }
}
static int cmpent(const void *a, const void *b) { return better(*(Ent *)a, *(Ent *)b) ? -1 : 1; }
static void heap_out(Heap *H, Res *o) {
  qsort(H->h, H->n, sizeof(Ent), cmpent);
  for (int k = 0; k < TOPN; k++) { o->top[k] = H->h[k].i; o->score[k] = H->h[k].s; }
}

// ---------- small dense linear algebra (double) ----------
static void jacobi(int n, double *A, double *E, double *lam) {  // A n×n symmetric (destroyed); E columns = eigvecs
  for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) E[i * n + j] = i == j;
  for (int sweep = 0; sweep < 100; sweep++) {
    double off = 0, tot = 0;
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) { tot += A[i * n + j] * A[i * n + j]; if (i != j) off += A[i * n + j] * A[i * n + j]; }
    if (off <= 1e-30 * tot) break;
    for (int p = 0; p < n; p++) for (int q = p + 1; q < n; q++) {
      double apq = A[p * n + q];
      if (fabs(apq) < 1e-300) continue;
      double th = (A[q * n + q] - A[p * n + p]) / (2 * apq);
      double t = (th >= 0 ? 1 : -1) / (fabs(th) + sqrt(th * th + 1)), c = 1 / sqrt(t * t + 1), s = t * c;
      for (int k = 0; k < n; k++) { double akp = A[k * n + p], akq = A[k * n + q]; A[k * n + p] = c * akp - s * akq; A[k * n + q] = s * akp + c * akq; }
      for (int k = 0; k < n; k++) { double apk = A[p * n + k], aqk = A[q * n + k]; A[p * n + k] = c * apk - s * aqk; A[q * n + k] = s * apk + c * aqk; }
      for (int k = 0; k < n; k++) { double ekp = E[k * n + p], ekq = E[k * n + q]; E[k * n + p] = c * ekp - s * ekq; E[k * n + q] = s * ekp + c * ekq; }
    }
  }
  for (int i = 0; i < n; i++) lam[i] = A[i * n + i];
}
// columns of Q (rows×cols, column c at Q[i*cols+c]) orthonormalized in place (MGS, twice); R optional cols×cols
static void mgs(int rows, int cols, double *Q, double *Rm) {
  if (Rm) memset(Rm, 0, sizeof(double) * cols * cols);
  for (int c = 0; c < cols; c++) for (int pass = 0; pass < 2; pass++) {
    for (int d = 0; d < c; d++) {
      double dot = 0; for (int i = 0; i < rows; i++) dot += Q[i * cols + d] * Q[i * cols + c];
      for (int i = 0; i < rows; i++) Q[i * cols + c] -= dot * Q[i * cols + d];
      if (Rm) Rm[d * cols + c] += dot;
    }
  }
  for (int c = 0; c < cols; c++) {
    double nr = 0; for (int i = 0; i < rows; i++) nr += Q[i * cols + c] * Q[i * cols + c];
    nr = sqrt(nr); if (Rm) Rm[c * cols + c] = nr;
    for (int i = 0; i < rows; i++) Q[i * cols + c] = nr > 0 ? Q[i * cols + c] / nr : 0;
  }
}
static void sort_eig(int n, double *lam, double *E, int *ord) {
  for (int i = 0; i < n; i++) ord[i] = i;
  for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (lam[ord[j]] > lam[ord[i]]) { int t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
  (void)E;
}
static void signfix(double *d, int n) {
  int m = 0; for (int i = 1; i < n; i++) if (fabs(d[i]) > fabs(d[m])) m = i;
  if (d[m] < 0) for (int i = 0; i < n; i++) d[i] = -d[i];
}

// ---------- exact Gram kernel over int16 row blocks (|value| <= 2048) ----------
// Block of 256 rows -> 128 pairs; per pair |a0 b0 + a1 b1| <= 2^23, 128 pairs <= 2^30 fits int32.
#define BR 256
static int32_t P2[BR / 2][NS] __attribute__((aligned(64)));
static void gram_block(int16_t (*a)[NS], int rows, int64_t *G /*NS×NS*/, int64_t *s) {
  if (rows & 1) { memset(a[rows], 0, sizeof a[0]); rows++; }
  int np = rows / 2;
  for (int q = 0; q < np; q++) for (int j = 0; j < NS; j++) {
    P2[q][j] = (uint16_t)a[2 * q][j] | ((uint32_t)(uint16_t)a[2 * q + 1][j] << 16);
    s[j] += a[2 * q][j] + a[2 * q + 1][j];
  }
  for (int j1 = 0; j1 < NS; j1 += 4)
    for (int j2 = j1 & ~63; j2 < NS; j2 += 64) {
      __m512i c[4][4];
      for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) c[x][y] = _mm512_setzero_si512();
      for (int q = 0; q < np; q++) {
        __m512i v0 = _mm512_load_si512(&P2[q][j2]), v1 = _mm512_load_si512(&P2[q][j2 + 16]);
        __m512i v2 = _mm512_load_si512(&P2[q][j2 + 32]), v3 = _mm512_load_si512(&P2[q][j2 + 48]);
        for (int x = 0; x < 4; x++) {
          __m512i b = _mm512_set1_epi32(P2[q][j1 + x]);
          c[x][0] = _mm512_add_epi32(c[x][0], _mm512_madd_epi16(b, v0));
          c[x][1] = _mm512_add_epi32(c[x][1], _mm512_madd_epi16(b, v1));
          c[x][2] = _mm512_add_epi32(c[x][2], _mm512_madd_epi16(b, v2));
          c[x][3] = _mm512_add_epi32(c[x][3], _mm512_madd_epi16(b, v3));
        }
      }
      for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) {
        int64_t *g = G + (long)(j1 + x) * NS + j2 + 16 * y;
        __m512i lo = _mm512_cvtepi32_epi64(_mm512_castsi512_si256(c[x][y]));
        __m512i hi = _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(c[x][y], 1));
        _mm512_storeu_si512(g, _mm512_add_epi64(_mm512_loadu_si512(g), lo));
        _mm512_storeu_si512(g + 8, _mm512_add_epi64(_mm512_loadu_si512(g + 8), hi));
      }
    }
}
// exact N·cov = N·GᵀG - s sᵀ (int128), symmetric
static i128 *cov_from_gram(const int64_t *G, const int64_t *s) {
  i128 *C = malloc(sizeof(i128) * NS * NS);
  for (int a = 0; a < NS; a++) for (int b = a; b < NS; b++) {
    i128 v = (i128)N * G[(long)a * NS + b] - (i128)s[a] * s[b];
    C[a * NS + b] = C[b * NS + a] = v;
  }
  return C;
}

// row sources for the Gram kernel
typedef struct { const int32_t *X, *U; int32_t VS[R][NS]; int relu; } Src;
static void fill_rows(const Src *z, long i0, int rows, int16_t (*a)[NS]) {
  for (int b = 0; b < rows; b++) {
    long i = i0 + b;
    if (z->X) {
      const int32_t *x = z->X + i * M;
      for (int j = 0; j < NS; j++) { int32_t v = Wt[S[j]] * x[S[j]]; a[b][j] = z->relu && v < 0 ? 0 : v; }
    } else {
      const int32_t *u = z->U + i * R; int32_t acc[NS];
      for (int j = 0; j < NS; j++) acc[j] = 0;
      for (int t = 0; t < R; t++) { int32_t c = u[t]; for (int j = 0; j < NS; j++) acc[j] += c * z->VS[t][j]; }
      for (int j = 0; j < NS; j++) { int32_t v = acc[j]; a[b][j] = z->relu && v < 0 ? 0 : v; }
    }
  }
}
