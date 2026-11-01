// K1: grammar-resident data vs a deduplicating chunk-summary store.
// See journal/phase2-plan.md, K1.
//
//   k1 synth N V seed L c1,c2,...   synthetic: N-byte base, V versions of 1..10 overwrites
//   k1 files PATH Lg c1,c2,...      real history: PATH = versions file (u32 count, {u32 len, bytes}*)
//
// Storage phase (no regex known): grammar nodes / chunk store built from edits.
// Query phase: the regex DFA is built, summaries are computed lazily in version
// order and memoized by node / unique-chunk id; every version gets accept/reject.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}
static void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) { fprintf(stderr, "oom %zu\n", n); exit(1); }
  return p;
}
static void *xcalloc(size_t n, size_t s) {
  void *p = calloc(n ? n : 1, s);
  if (!p) { fprintf(stderr, "oom\n"); exit(1); }
  return p;
}
static void *xrealloc(void *p, size_t n) {
  p = realloc(p, n);
  if (!p) { fprintf(stderr, "oom %zu\n", n); exit(1); }
  return p;
}
static u64 rs = 1;
static u64 rnd(void) {  // splitmix64
  u64 z = (rs += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
static u64 hbytes(const u8 *p, size_t n) {
  u64 h = 0x243F6A8885A308D3ull ^ n;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    u64 w;
    memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
  }
  for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
  h ^= h >> 32;
  h *= 0xD6E8FEB86659FD93ull;
  return h ^ (h >> 32);
}

// ---------------------------------------------------------------- DFA
// Language: the number of (overlapping) occurrences of any of PATS is odd.
// Aho-Corasick automaton x parity bit. Chosen after storage; |Q| = 46.
static const char *PATS[] = {"xyzzy", "plugh", "luaV_", "vmfetch"};
#define NPAT 4
static int Q;
static u8 T[64][256];
static u8 ACC[64];

static void build_dfa(void) {
  int go[64][256], fail[64], out[64], S = 1;
  memset(go, -1, sizeof go);
  memset(out, 0, sizeof out);
  for (int p = 0; p < NPAT; p++) {
    int s = 0;
    for (const char *c = PATS[p]; *c; c++) {
      u8 b = (u8)*c;
      if (go[s][b] < 0) go[s][b] = S++;
      s = go[s][b];
    }
    out[s]++;
  }
  int qu[64], h = 0, t = 0;
  fail[0] = 0;
  for (int b = 0; b < 256; b++) {
    if (go[0][b] < 0) go[0][b] = 0;
    else { fail[go[0][b]] = 0; qu[t++] = go[0][b]; }
  }
  while (h < t) {
    int s = qu[h++];
    out[s] += out[fail[s]];
    for (int b = 0; b < 256; b++) {
      int u = go[s][b];
      if (u < 0) go[s][b] = go[fail[s]][b];
      else { fail[u] = go[fail[s]][b]; qu[t++] = u; }
    }
  }
  Q = 2 * S;
  if (Q > 64) { fprintf(stderr, "Q too big\n"); exit(1); }
  for (int s = 0; s < S; s++)
    for (int p = 0; p < 2; p++) {
      ACC[2 * s + p] = (u8)p;
      for (int b = 0; b < 256; b++) {
        int u = go[s][b];
        T[2 * s + p][b] = (u8)(2 * u + (p ^ (out[u] & 1)));
      }
    }
}
static int direct_scan(const u8 *d, size_t n) {
  u8 s = 0;
  for (size_t i = 0; i < n; i++) s = T[s][d[i]];
  return ACC[s];
}

// ---------------------------------------------------------------- counters
typedef struct {
  u64 sumops;   // DFA transitions + dedupe work while building leaf/chunk summaries
  u64 nleafsum; // leaf/chunk summaries built
  u64 comps;    // |Q|-wide compositions (out[q] = b[a[q]])
  u64 steps;    // 1-wide applications (state = s[state])
  u64 bdata;    // data bytes scanned into summaries
  u64 bsum;     // summary bytes read+written (|Q| per summary touched; a step touches one |Q|-byte record;
                // bytes_lo in the report counts a step as 1 summary byte instead)
  u64 bmeta;    // child ids / manifest entries read
} C;
static void cadd(C *a, const C *b, int sgn) {
  a->sumops += sgn * b->sumops; a->nleafsum += sgn * b->nleafsum; a->comps += sgn * b->comps;
  a->steps += sgn * b->steps; a->bdata += sgn * b->bdata; a->bsum += sgn * b->bsum;
  a->bmeta += sgn * b->bmeta;
}
static u64 cbytes(const C *c) { return c->bdata + c->bsum + c->bmeta; }
static u64 cops(const C *c) { return c->sumops + c->comps * (u64)Q + c->steps; }

// Summary of a byte string: out[q] = state after reading d from q.
// All |Q| start states advance together; identical states are merged
// (convergence), which both systems use. With the parity bit at least 2 survive.
static void leaf_summary(const u8 *d, size_t n, u8 *out, C *c) {
  u8 cur[64], rep[64];
  int k = Q;
  for (int q = 0; q < Q; q++) cur[q] = rep[q] = (u8)q;
  size_t next = 8, iv = 8;
  u64 ops = 0;
  for (size_t i = 0; i < n; i++) {
    const u8 b = d[i];
    for (int j = 0; j < k; j++) cur[j] = T[cur[j]][b];
    ops += k;
    if (i + 1 == next && k > 1) {
      signed char idx[64];
      u8 remap[64];
      memset(idx, -1, Q);
      int nk = 0;
      for (int j = 0; j < k; j++) {
        u8 s = cur[j];
        if (idx[s] < 0) { idx[s] = (signed char)nk; cur[nk++] = s; }
        remap[j] = (u8)idx[s];
      }
      if (nk < k) for (int q = 0; q < Q; q++) rep[q] = remap[rep[q]];
      ops += k + (nk < k ? Q : 0);
      if (nk == k) iv *= 2; // no progress: back off
      k = nk;
      next = i + 1 + iv;
    }
  }
  for (int q = 0; q < Q; q++) out[q] = cur[rep[q]];
  c->sumops += ops + Q;
  c->nleafsum++;
  c->bdata += n;
  c->bsum += Q;
}
static inline void compose(const u8 *a, const u8 *b, u8 *out, C *c) {
  for (int q = 0; q < Q; q++) out[q] = b[a[q]];
  c->comps++;
  c->bsum += 3 * (u64)Q;
}

// ---------------------------------------------------------------- hash map u64 -> int
typedef struct { u64 *k; int *v; size_t cap, n; } HM;
static void hm_init(HM *m, size_t cap) {
  m->cap = 1; while (m->cap < 2 * cap) m->cap <<= 1;
  m->k = xmalloc(m->cap * 8); m->v = xmalloc(m->cap * sizeof(int)); m->n = 0;
  for (size_t i = 0; i < m->cap; i++) m->v[i] = -1;
}
static void hm_grow(HM *m);
static int *hm_slot(HM *m, u64 key) {  // returns slot; *slot==-1 if absent (insert by setting)
  if (2 * (m->n + 1) > m->cap) hm_grow(m);
  size_t i = key & (m->cap - 1);
  while (m->v[i] >= 0 && m->k[i] != key) i = (i + 1) & (m->cap - 1);
  m->k[i] = key;
  return &m->v[i];
}
static void hm_grow(HM *m) {
  HM o = *m;
  hm_init(m, o.cap);
  for (size_t i = 0; i < o.cap; i++) if (o.v[i] >= 0) { *hm_slot(m, o.k[i]) = o.v[i]; m->n++; }
  free(o.k); free(o.v);
}
static void hm_free(HM *m) { free(m->k); free(m->v); }

// ---------------------------------------------------------------- report
static void report(const char *name, int nv, const C *tot, const C *v0, double tstore, double tquery,
                   u64 nodes_or_chunks, u64 store_bytes) {
  C m = *tot;
  cadd(&m, v0, -1);
  double k = nv > 1 ? 1.0 / (nv - 1) : 0;
  printf("%-22s | v0: comps %llu steps %llu leafsum %llu bytes %llu | per-version(v>=1): comps %.1f steps %.1f "
         "leafsum %.2f sumB(new) %.0f ops %.0f bytes %.0f bytes_lo %.0f | total: comps %llu steps %llu sumB %llu ops %llu bytes %llu "
         "| store: units %llu bytes %llu | wall store %.2fs query %.2fs\n",
         name, (unsigned long long)v0->comps, (unsigned long long)v0->steps, (unsigned long long)v0->nleafsum,
         (unsigned long long)cbytes(v0), m.comps * k, m.steps * k, m.nleafsum * k,
         (double)(m.comps + m.nleafsum) * Q * k, cops(&m) * k, cbytes(&m) * k,
         (cbytes(&m) - m.steps * (double)(Q - 1)) * k, (unsigned long long)tot->comps,
         (unsigned long long)tot->steps, (unsigned long long)((tot->comps + tot->nleafsum) * Q),
         (unsigned long long)cops(tot), (unsigned long long)cbytes(tot), (unsigned long long)nodes_or_chunks,
         (unsigned long long)store_bytes, tstore, tquery);
  fflush(stdout);
}
static int check(const char *name, int nv, const u8 *res, const u8 *ref) {
  int bad = 0, acc = 0;
  for (int v = 0; v < nv; v++) { if (res[v] != ref[v]) bad++; acc += res[v]; }
  printf("CHECK %-22s vs reference: %d/%d versions differ; %d accept\n", name, bad, nv, acc);
  return bad;
}

// ================================================================ synthetic
static size_t N;
static int V;
static u64 *epos; static u32 *elen; static u64 *eoff; static u8 *edata; static u64 *vstart;
static u8 *base;

static void gen_synth(u64 seed) {
  rs = seed;
  static const char AL[] = "abcdefghijklmnopqrstuvwxyz      \n";
  base = xmalloc(N);
  for (size_t i = 0; i < N; i++) base[i] = (u8)AL[rnd() % 33];
  vstart = xmalloc((V + 2) * 8);
  size_t cap = (size_t)V * 10 + 1, dcap = cap * 64;
  epos = xmalloc(cap * 8); elen = xmalloc(cap * 4); eoff = xmalloc(cap * 8); edata = xmalloc(dcap);
  size_t ne = 0, nd = 0;
  vstart[0] = vstart[1] = 0;  // version 0 = base, no edits
  for (int v = 1; v <= V; v++) {
    int k = 1 + (int)(rnd() % 10);
    for (int e = 0; e < k; e++) {
      u32 len = 1 + (u32)(rnd() % 64);
      epos[ne] = rnd() % (N - len); elen[ne] = len; eoff[ne] = nd;
      for (u32 i = 0; i < len; i++) edata[nd + i] = (u8)AL[rnd() % 33];
      if (rnd() % 10 == 0) {  // plant a pattern
        const char *p = PATS[rnd() % NPAT];
        u32 pl = (u32)strlen(p);
        if (pl <= len) memcpy(edata + nd + rnd() % (len - pl + 1), p, pl);
      }
      nd += len; ne++;
    }
    vstart[v + 1] = ne;
  }
}

// ---- grammar: persistent balanced binary tree over L-byte leaves, path copying
static int *gl, *gr; static u32 *gstamp; static u64 *goff; static size_t gn, gcap;
static u8 *gpool; static size_t gpn, gpcap;
static size_t GL; static int GD;

static int gnew(int l, int r, u64 off, u32 st) {
  if (gn == gcap) {
    gcap = gcap * 3 / 2 + 16;
    gl = xrealloc(gl, gcap * 4); gr = xrealloc(gr, gcap * 4);
    gstamp = xrealloc(gstamp, gcap * 4); goff = xrealloc(goff, gcap * 8);
  }
  gl[gn] = l; gr[gn] = r; goff[gn] = off; gstamp[gn] = st;
  return (int)gn++;
}
static u64 gleafcopy(u64 off) {
  if (gpn + GL > gpcap) { gpcap = gpcap * 3 / 2 + GL; gpool = xrealloc(gpool, gpcap); }
  memcpy(gpool + gpn, gpool + off, GL);
  gpn += GL;
  return gpn - GL;
}
// returns new node for this subtree; *leaf = writable leaf for idx
static int gcow(int n, int depth, size_t idx, u32 v, int *leaf) {
  if (gstamp[n] != v) n = gl[n] < 0 ? gnew(-1, -1, gleafcopy(goff[n]), v) : gnew(gl[n], gr[n], 0, v);
  if (depth == 0) { *leaf = n; return n; }
  size_t half = (size_t)1 << (depth - 1);
  if (idx < half) { int c = gcow(gl[n], depth - 1, idx, v, leaf); gl[n] = c; }
  else { int c = gcow(gr[n], depth - 1, idx - half, v, leaf); gr[n] = c; }
  return n;
}
static u8 *gsum; static u8 *ghave;
static const u8 *gsummary(int n, C *c) {
  u8 *o = gsum + (size_t)n * Q;
  if (ghave[n]) return o;
  if (gl[n] < 0) leaf_summary(gpool + goff[n], GL, o, c);
  else {
    const u8 *a = gsummary(gl[n], c), *b = gsummary(gr[n], c);
    compose(a, b, o, c);
    c->bmeta += 8;
  }
  ghave[n] = 1;
  return o;
}

static u8 *run_grammar_synth(size_t L, u8 *ref_out) {
  GL = L;
  size_t nleaf = N / L;
  GD = 0; while (((size_t)1 << GD) < nleaf) GD++;
  if (((size_t)1 << GD) != nleaf) { fprintf(stderr, "N/L must be a power of two\n"); exit(1); }
  double t0 = now();
  gn = 0; gcap = 2 * nleaf + 1024; gl = xmalloc(gcap * 4); gr = xmalloc(gcap * 4);
  gstamp = xmalloc(gcap * 4); goff = xmalloc(gcap * 8);
  gpcap = N + N / 8; gpool = xmalloc(gpcap); memcpy(gpool, base, N); gpn = N;
  int *lev = xmalloc(nleaf * sizeof(int));
  for (size_t i = 0; i < nleaf; i++) lev[i] = gnew(-1, -1, i * L, 0);
  for (size_t w = nleaf; w > 1; w /= 2)
    for (size_t i = 0; i < w / 2; i++) lev[i] = gnew(lev[2 * i], lev[2 * i + 1], 0, 0);
  int *root = xmalloc((V + 1) * sizeof(int));
  root[0] = lev[0];
  free(lev);
  for (int v = 1; v <= V; v++) {
    int r = root[v - 1];
    for (u64 e = vstart[v]; e < vstart[v + 1]; e++) {
      u64 p = epos[e], end = p + elen[e];
      while (p < end) {
        size_t li = p / L, lo = p % L, take = L - lo;
        if (take > end - p) take = end - p;
        int leaf;
        r = gcow(r, GD, li, (u32)v, &leaf);
        memcpy(gpool + goff[leaf] + lo, edata + eoff[e] + (p - epos[e]), take);
        p += take;
      }
    }
    root[v] = r;
  }
  double t1 = now();
  // ---- query
  build_dfa();
  gsum = xmalloc(gn * (size_t)Q); ghave = xcalloc(gn, 1);
  u8 *res = xmalloc(V + 1);
  C tot = {0}, v0 = {0};
  for (int v = 0; v <= V; v++) {
    const u8 *s = gsummary(root[v], &tot);
    res[v] = ACC[s[0]];
    if (v == 0) v0 = tot;
  }
  double t2 = now();
  char name[64];
  snprintf(name, sizeof name, "grammar L=%zu", L);
  report(name, V + 1, &tot, &v0, t1 - t0, t2 - t1, gn, gpn + gn * 16);
  (void)ref_out;
  free(gsum); free(ghave); free(gl); free(gr); free(gstamp); free(goff); free(gpool); free(root);
  gl = gr = NULL; gstamp = NULL; goff = NULL; gpool = NULL;
  return res;
}

// ---- chunk store: fixed-size chunks, exact dedup by hash, per-version manifest
static u8 *run_chunks_synth(size_t c) {
  double t0 = now();
  size_t nch = N / c;
  u8 *cur = xmalloc(N); memcpy(cur, base, N);
  size_t pcap = N + N / 4, pn = 0; u8 *pool = xmalloc(pcap);
  size_t nu = 0, ucap = nch + 1024; u64 *uoff = xmalloc(ucap * 8);
  HM hm; hm_init(&hm, nch + 1024);
  u32 *man0 = xmalloc(nch * 4);
  // change log: per version, (chunk index, chunk id)
  size_t lcap = (size_t)V * 12 + 16, ln = 0; u32 *lidx = xmalloc(lcap * 4), *lid = xmalloc(lcap * 4);
  u64 *lstart = xmalloc((V + 2) * 8);
  u32 *mark = xcalloc(nch, 4); u32 *dirty = xmalloc((size_t)10 * (64 / c + 2) * 4 + 64);
#define INTERN(ptr, out)                                                       \
  do {                                                                         \
    u64 h = hbytes(ptr, c);                                                    \
    int *sl;                                                                   \
    for (;;) {                                                                 \
      sl = hm_slot(&hm, h);                                                    \
      if (*sl < 0 || memcmp(pool + uoff[*sl], ptr, c) == 0) break;             \
      h = h * 0x9E3779B97F4A7C15ull + 1; /* true collision: probe next key */  \
    }                                                                          \
    if (*sl < 0) {                                                             \
      if (pn + c > pcap) { pcap = pcap * 3 / 2 + c; pool = xrealloc(pool, pcap); } \
      memcpy(pool + pn, ptr, c);                                               \
      if (nu == ucap) { ucap = ucap * 3 / 2; uoff = xrealloc(uoff, ucap * 8); } \
      uoff[nu] = pn; pn += c; *sl = (int)nu++; hm.n++;                         \
    }                                                                          \
    out = (u32)*sl;                                                            \
  } while (0)
  for (size_t i = 0; i < nch; i++) INTERN(cur + i * c, man0[i]);
  lstart[0] = lstart[1] = 0;
  for (int v = 1; v <= V; v++) {
    int nd = 0;
    for (u64 e = vstart[v]; e < vstart[v + 1]; e++) {
      memcpy(cur + epos[e], edata + eoff[e], elen[e]);
      for (size_t k = epos[e] / c; k <= (epos[e] + elen[e] - 1) / c; k++)
        if (mark[k] != (u32)v) { mark[k] = (u32)v; dirty[nd++] = (u32)k; }
    }
    for (int j = 0; j < nd; j++) {
      if (ln == lcap) { lcap *= 2; lidx = xrealloc(lidx, lcap * 4); lid = xrealloc(lid, lcap * 4); }
      lidx[ln] = dirty[j];
      INTERN(cur + (size_t)dirty[j] * c, lid[ln]);
      ln++;
    }
    lstart[v + 1] = ln;
  }
#undef INTERN
  free(cur); hm_free(&hm); free(mark); free(dirty);
  double t1 = now();
  // ---- query
  build_dfa();
  u8 *sum = xmalloc(nu * (size_t)Q), *have = xcalloc(nu, 1);
  u32 *man = xmalloc(nch * 4); memcpy(man, man0, nch * 4);
  u8 *res = xmalloc(V + 1);
  C tot = {0}, v0 = {0};
  for (int v = 0; v <= V; v++) {
    for (u64 j = lstart[v]; j < lstart[v + 1]; j++) man[lidx[j]] = lid[j];
    u8 st = 0;
    for (size_t i = 0; i < nch; i++) {
      u32 id = man[i];
      u8 *s = sum + (size_t)id * Q;
      if (!have[id]) { leaf_summary(pool + uoff[id], c, s, &tot); have[id] = 1; }
      st = s[st];
    }
    tot.steps += nch; tot.bsum += nch * (u64)Q; tot.bmeta += nch * 4;
    res[v] = ACC[st];
    if (v == 0) v0 = tot;
  }
  double t2 = now();
  char name[64];
  snprintf(name, sizeof name, "chunks c=%zu", c);
  report(name, V + 1, &tot, &v0, t1 - t0, t2 - t1, nu, pn + ((u64)nch + ln) * 4);
  free(sum); free(have); free(man); free(man0); free(pool); free(uoff); free(lidx); free(lid); free(lstart);
  return res;
}

static void synth_ref(int *vs, int nvs, u8 *ref) {
  // direct scans of materialized versions (verification only)
  build_dfa();
  u8 *cur = xmalloc(N); memcpy(cur, base, N);
  int j = 0;
  for (int v = 0; v <= V && j < nvs; v++) {
    for (u64 e = vstart[v]; e < vstart[v + 1]; e++) memcpy(cur + epos[e], edata + eoff[e], elen[e]);
    if (v == vs[j]) ref[j++] = (u8)direct_scan(cur, N);
  }
  free(cur);
}

static int parse_list(char *s, size_t *out) {
  int n = 0;
  for (char *t = strtok(s, ","); t; t = strtok(NULL, ",")) out[n++] = strtoull(t, NULL, 10);
  return n;
}

static int main_synth(int argc, char **argv) {
  if (argc < 7) return 2;
  N = strtoull(argv[2], NULL, 10); V = atoi(argv[3]);
  u64 seed = strtoull(argv[4], NULL, 10);
  size_t Ls[16], cs[16];
  int nL = parse_list(argv[5], Ls), nc = parse_list(argv[6], cs);
  double t0 = now();
  gen_synth(seed);
  build_dfa();
  printf("synthetic: N=%zu V=%d seed=%llu edits=%llu edit-bytes=%llu |Q|=%d gen %.2fs\n", N, V,
         (unsigned long long)seed, (unsigned long long)vstart[V + 1], (unsigned long long)(eoff[vstart[V + 1] - 1] + elen[vstart[V + 1] - 1]),
         Q, now() - t0);
  int vs[] = {0, 1, 2, V / 3, V / 2, V - 1, V};
  int nvs = 7;
  u8 ref[7];
  synth_ref(vs, nvs, ref);
  int bad = 0;
  u8 *gres = NULL;
  for (int i = 0; i < nL; i++) {
    u8 *r = run_grammar_synth(Ls[i], NULL);
    for (int j = 0; j < nvs; j++) if (r[vs[j]] != ref[j]) { bad++; printf("DIRECT MISMATCH grammar v=%d\n", vs[j]); }
    if (!gres) gres = r;
    else { char nm[64]; snprintf(nm, sizeof nm, "grammar L=%zu", Ls[i]); bad += check(nm, V + 1, r, gres); free(r); }
  }
  printf("CHECK direct scans of versions");
  for (int j = 0; j < nvs; j++) printf(" %d:%d", vs[j], ref[j]);
  printf(" -> grammar %s\n", bad ? "MISMATCH" : "match");
  for (int i = 0; i < nc; i++) {
    u8 *r = run_chunks_synth(cs[i]);
    char nm[64]; snprintf(nm, sizeof nm, "chunks c=%zu", cs[i]);
    bad += check(nm, V + 1, r, gres);
    free(r);
  }
  printf("ALL CHECKS %s\n", bad ? "FAILED" : "PASSED");
  return bad ? 1 : 0;
}

// ================================================================ real history
static int FV; static u8 **fdat; static size_t *flen;
static u64 GEAR[256];
static size_t cdc_cut(const u8 *p, size_t n, size_t avg) {  // gear-hash content-defined cut
  size_t mn = avg / 4, mx = avg * 4;
  u64 mask = avg - 1, h = 0;  // avg is a power of two
  if (n <= mn) return n;
  if (n > mx) n = mx;
  for (size_t i = 0; i < n; i++) {
    h = (h << 1) + GEAR[p[i]];
    if (i >= mn && (h >> 40 & mask) == 0) return i + 1;
  }
  return n;
}

// unique-chunk store shared by the chunk store and the grammar's leaves
typedef struct { HM hm; u8 *pool; size_t pn, pcap; u64 *off; u32 *len; size_t nu, ucap; } US;
static void us_init(US *u) { memset(u, 0, sizeof *u); hm_init(&u->hm, 1024); }
static u32 us_intern(US *u, const u8 *p, size_t n) {
  u64 h = hbytes(p, n);
  int *sl;
  for (;;) {
    sl = hm_slot(&u->hm, h);
    if (*sl < 0 || (u->len[*sl] == n && memcmp(u->pool + u->off[*sl], p, n) == 0)) break;
    h = h * 0x9E3779B97F4A7C15ull + 1;
  }
  if (*sl < 0) {
    if (u->pn + n > u->pcap) { u->pcap = u->pcap * 2 + n; u->pool = xrealloc(u->pool, u->pcap); }
    memcpy(u->pool + u->pn, p, n);
    if (u->nu == u->ucap) { u->ucap = u->ucap * 2 + 16; u->off = xrealloc(u->off, u->ucap * 8); u->len = xrealloc(u->len, u->ucap * 4); }
    u->off[u->nu] = u->pn; u->len[u->nu] = (u32)n; u->pn += n; *sl = (int)u->nu++; u->hm.n++;
  }
  return (u32)*sl;
}
static u32 *chunk_version(US *u, const u8 *d, size_t n, size_t avg, size_t *cnt) {
  size_t cap = n / (avg / 4) + 2, k = 0;
  u32 *ids = xmalloc(cap * 4);
  for (size_t p = 0; p < n;) {
    size_t l = cdc_cut(d + p, n - p, avg);
    ids[k++] = us_intern(u, d + p, l);
    p += l;
  }
  *cnt = k;
  return ids;
}

static u8 *run_chunks_files(size_t avg) {
  double t0 = now();
  US u; us_init(&u);
  u32 **man = xmalloc(FV * sizeof *man); size_t *mn = xmalloc(FV * sizeof *mn); u64 mtot = 0;
  for (int v = 0; v < FV; v++) { man[v] = chunk_version(&u, fdat[v], flen[v], avg, &mn[v]); mtot += mn[v]; }
  double t1 = now();
  build_dfa();
  u8 *sum = xmalloc(u.nu * (size_t)Q), *have = xcalloc(u.nu, 1), *res = xmalloc(FV);
  C tot = {0}, v0 = {0};
  for (int v = 0; v < FV; v++) {
    u8 st = 0;
    for (size_t i = 0; i < mn[v]; i++) {
      u32 id = man[v][i];
      u8 *s = sum + (size_t)id * Q;
      if (!have[id]) { leaf_summary(u.pool + u.off[id], u.len[id], s, &tot); have[id] = 1; }
      st = s[st];
    }
    tot.steps += mn[v]; tot.bsum += mn[v] * (u64)Q; tot.bmeta += mn[v] * 4;
    res[v] = ACC[st];
    if (v == 0) v0 = tot;
  }
  double t2 = now();
  char name[64]; snprintf(name, sizeof name, "chunks cdc=%zu", avg);
  report(name, FV, &tot, &v0, t1 - t0, t2 - t1, u.nu, u.pn + mtot * 4);
  for (int v = 0; v < FV; v++) free(man[v]);
  free(man); free(mn); free(sum); free(have); free(u.pool); free(u.off); free(u.len); hm_free(&u.hm);
  return res;
}

// grammar for real history: hash-consed content-defined balanced tree (prolly tree)
// over CDC leaves. Persistence comes from hash-consing instead of path copying
// (path copying needs positional edits; real diffs insert and delete).
typedef struct { u32 *kid; u64 kn, kcap; u64 *ks; u32 *kc; u64 *h; size_t n, cap; HM hm; } PT;
// node ids: leaves are unique chunks 0..nu-1 (tagged), internal nodes in PT
static u8 *run_grammar_files(size_t avg) {
  double t0 = now();
  US u; us_init(&u);
  PT t; memset(&t, 0, sizeof t); hm_init(&t.hm, 1024);
  // node ref: (id<<1)|isinternal
  u32 *roots = xmalloc(FV * 4);
  for (int v = 0; v < FV; v++) {
    size_t cnt;
    u32 *ids = chunk_version(&u, fdat[v], flen[v], avg, &cnt);
    u64 *hs = xmalloc((cnt + 1) * 8);
    for (size_t i = 0; i < cnt; i++) { hs[i] = hbytes(u.pool + u.off[ids[i]], u.len[ids[i]]); ids[i] <<= 1; }
    if (cnt == 0) { fprintf(stderr, "empty version\n"); exit(1); }
    while (cnt > 1) {
      size_t w = 0, s = 0;
      for (size_t i = 0; i < cnt; i++) {
        int cut = ((hs[i] * 0xD6E8FEB86659FD93ull) >> 62) == 0 || i + 1 == cnt || i + 1 - s == 32;
        if (!cut) continue;
        size_t k = i + 1 - s;
        u64 h = 0x51ED27ull;
        for (size_t j = s; j <= i; j++) h = (h ^ hs[j]) * 0x9E3779B97F4A7C15ull, h ^= h >> 31;
        int *sl;
        for (;;) {
          sl = hm_slot(&t.hm, h);
          if (*sl < 0 || (t.kc[*sl] == k && memcmp(t.kid + t.ks[*sl], ids + s, k * 4) == 0)) break;
          h = h * 0x9E3779B97F4A7C15ull + 1;
        }
        if (*sl < 0) {
          if (t.kn + k > t.kcap) { t.kcap = t.kcap * 2 + k; t.kid = xrealloc(t.kid, t.kcap * 4); }
          memcpy(t.kid + t.kn, ids + s, k * 4);
          if (t.n == t.cap) { t.cap = t.cap * 2 + 16; t.ks = xrealloc(t.ks, t.cap * 8); t.kc = xrealloc(t.kc, t.cap * 4); }
          t.ks[t.n] = t.kn; t.kc[t.n] = (u32)k; t.kn += k; *sl = (int)t.n++; t.hm.n++;
        }
        ids[w] = ((u32)*sl << 1) | 1; hs[w] = h; w++;
        s = i + 1;
      }
      cnt = w;
    }
    roots[v] = ids[0];
    free(ids); free(hs);
  }
  double t1 = now();
  build_dfa();
  u8 *lsum = xmalloc(u.nu * (size_t)Q), *lhave = xcalloc(u.nu, 1);
  u8 *isum = xmalloc(t.n * (size_t)Q + 1), *ihave = xcalloc(t.n, 1);
  u8 *res = xmalloc(FV);
  C tot = {0}, v0 = {0};
  // iterative post-order with memo
  u32 *stk = xmalloc(64 * 1024 * 4);
  for (int v = 0; v < FV; v++) {
    int sp = 0;
    stk[sp++] = roots[v];
    while (sp) {
      u32 r = stk[sp - 1], id = r >> 1;
      if (!(r & 1)) {
        if (!lhave[id]) { leaf_summary(u.pool + u.off[id], u.len[id], lsum + (size_t)id * Q, &tot); lhave[id] = 1; }
        sp--; continue;
      }
      if (ihave[id]) { sp--; continue; }
      int pending = 0;
      for (u32 j = 0; j < t.kc[id]; j++) {
        u32 c = t.kid[t.ks[id] + j];
        if (!((c & 1) ? ihave[c >> 1] : lhave[c >> 1])) { stk[sp++] = c; pending = 1; }
      }
      if (pending) continue;
      u8 *o = isum + (size_t)id * Q;
      for (u32 j = 0; j < t.kc[id]; j++) {
        u32 c = t.kid[t.ks[id] + j];
        const u8 *cs = ((c & 1) ? isum : lsum) + (size_t)(c >> 1) * Q;
        if (j == 0) { memcpy(o, cs, Q); tot.bsum += 2 * (u64)Q; }
        else compose(o, cs, o, &tot);
        tot.bmeta += 4;
      }
      ihave[id] = 1; sp--;
    }
    u32 r = roots[v];
    res[v] = ACC[(((r & 1) ? isum : lsum) + (size_t)(r >> 1) * Q)[0]];
    if (v == 0) v0 = tot;
  }
  double t2 = now();
  char name[64]; snprintf(name, sizeof name, "grammar prolly cdc=%zu", avg);
  report(name, FV, &tot, &v0, t1 - t0, t2 - t1, u.nu + t.n, u.pn + t.kn * 4 + t.n * 12);
  free(stk); free(lsum); free(lhave); free(isum); free(ihave); free(roots);
  return res;
}

static int main_files(int argc, char **argv) {
  if (argc < 5) return 2;
  FILE *f = fopen(argv[2], "rb");
  if (!f) { perror(argv[2]); return 1; }
  u32 n;
  if (fread(&n, 4, 1, f) != 1) return 1;
  FV = (int)n; fdat = xmalloc(FV * sizeof *fdat); flen = xmalloc(FV * sizeof *flen);
  u64 tot = 0;
  for (int v = 0; v < FV; v++) {
    u32 l;
    if (fread(&l, 4, 1, f) != 1) return 1;
    fdat[v] = xmalloc(l); flen[v] = l; tot += l;
    if (l && fread(fdat[v], 1, l, f) != l) return 1;
  }
  fclose(f);
  rs = 12345; for (int i = 0; i < 256; i++) GEAR[i] = rnd();
  build_dfa();
  u8 *ref = xmalloc(FV);
  for (int v = 0; v < FV; v++) ref[v] = (u8)direct_scan(fdat[v], flen[v]);
  int acc = 0; for (int v = 0; v < FV; v++) acc += ref[v];
  printf("files: %d versions, %llu raw bytes, last version %zu bytes, |Q|=%d, direct scan: %d accept\n", FV,
         (unsigned long long)tot, flen[FV - 1], Q, acc);
  size_t Ls[16], cs[16];
  int nL = parse_list(argv[3], Ls), nc = parse_list(argv[4], cs), bad = 0;
  for (int i = 0; i < nL; i++) {
    u8 *r = run_grammar_files(Ls[i]);
    char nm[64]; snprintf(nm, sizeof nm, "grammar prolly cdc=%zu", Ls[i]);
    bad += check(nm, FV, r, ref); free(r);
  }
  for (int i = 0; i < nc; i++) {
    u8 *r = run_chunks_files(cs[i]);
    char nm[64]; snprintf(nm, sizeof nm, "chunks cdc=%zu", cs[i]);
    bad += check(nm, FV, r, ref); free(r);
  }
  printf("(files mode: every version is checked against a direct scan)\nALL CHECKS %s\n", bad ? "FAILED" : "PASSED");
  return bad ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "synth")) return main_synth(argc, argv);
  if (argc > 1 && !strcmp(argv[1], "files")) return main_files(argc, argv);
  fprintf(stderr, "usage: k1 synth N V seed L1,L2 c1,c2 | k1 files PATH Lg1,Lg2 c1,c2\n");
  return 2;
}
