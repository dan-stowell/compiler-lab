// K6 stream 1: synthetic op stream, executed once, 16 op kinds with uneven costs.
//
// Op = {op, d, a, b} (4 bytes): R[d] = f_op(R[a], R[b]) over a 16 x uint32 register file.
// The opcode and register numbers are D (known to the generator); register values are S.
// Kinds: 0 ADD 1 SUB 2 XOR 3 AND 4 OR 5 SHL 6 SHR 7 ROL 8 MIN 9 MAX 10 SLT 11 ADDI(R[a]+4b, imm)
//        12 MUL(b|1) 13 MULHI 14 DIV(b|1) 15 REM(b|1)
// Distribution: with probability Q (per mille) the op is ADD, otherwise uniform over 16.
// Q=0 is the synthetic stream (4 bits of entropy); the Q sweep is the predictability sweep.
//
// Modes (one per process; the measured region is bracketed by perf --control):
//   cgoto   computed-goto threaded interpreter
//   pred    branch-free: compute all 16 kinds, select by opcode
//   hybrid  branch-free over the 14 cheap kinds, branch on DIV/REM
//   jit     run-once: per chunk emit straight-line x86 into a ring slot, call once, overwrite
//   gen     the jit generator alone (emits, never executes)
//   check   run every implementation from the same start state, compare all 16 registers
//   stats   entropy, kind mix, dependency depth (unit and weighted latency)
// usage: k6syn MODE Q_PERMILLE [nops_log2=24] [chunk_ops=500]
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <immintrin.h>

typedef struct { uint8_t op, d, a, b; } Op;
static Op *P; static long NOPS;

static inline uint64_t sm(uint64_t *s) {
  uint64_t z = (*s += 0x9e3779b97f4a7c15ULL); z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL; return z ^ (z >> 31);
}
static void init_regs(uint32_t *r) { uint64_t s = 99; for (int i = 0; i < 16; i++) r[i] = (uint32_t)sm(&s); }
static uint64_t hash_regs(const uint32_t *r) { uint64_t h = 1469598103934665603ULL; for (int i = 0; i < 16; i++) h = (h ^ r[i]) * 1099511628211ULL; return h; }

static int ctl = -1, ack = -1;
static void perf_cmd(const char *c) {
  if (ctl < 0) return;
  if (write(ctl, c, strlen(c)) < 0) perror("ctl");
  char b[16]; if (read(ack, b, sizeof b) < 0) perror("ack");
}

static inline uint32_t rol32(uint32_t x, uint32_t s) { s &= 31; return (x << s) | (x >> ((32 - s) & 31)); }

// ---- computed goto ----
__attribute__((noinline)) static void run_cgoto(const Op *p, long n, uint32_t *r) {
  static void *L[16] = {&&ADD, &&SUB, &&XOR, &&AND, &&OR, &&SHL, &&SHR, &&ROL,
                        &&MIN, &&MAX, &&SLT, &&ADDI, &&MUL, &&MULHI, &&DIV, &&REM};
  const Op *e = p + n; uint32_t x, y;
#define NX if (p == e) return; goto *L[p->op];
  NX
ADD:  r[p->d] = r[p->a] + r[p->b]; p++; NX
SUB:  r[p->d] = r[p->a] - r[p->b]; p++; NX
XOR:  r[p->d] = r[p->a] ^ r[p->b]; p++; NX
AND:  r[p->d] = r[p->a] & r[p->b]; p++; NX
OR:   r[p->d] = r[p->a] | r[p->b]; p++; NX
SHL:  r[p->d] = r[p->a] << (r[p->b] & 31); p++; NX
SHR:  r[p->d] = r[p->a] >> (r[p->b] & 31); p++; NX
ROL:  r[p->d] = rol32(r[p->a], r[p->b]); p++; NX
MIN:  x = r[p->a]; y = r[p->b]; r[p->d] = y < x ? y : x; p++; NX
MAX:  x = r[p->a]; y = r[p->b]; r[p->d] = y > x ? y : x; p++; NX
SLT:  r[p->d] = r[p->a] < r[p->b]; p++; NX
ADDI: r[p->d] = r[p->a] + 4 * p->b; p++; NX
MUL:  r[p->d] = r[p->a] * (r[p->b] | 1); p++; NX
MULHI: r[p->d] = (uint32_t)(((uint64_t)r[p->a] * r[p->b]) >> 32); p++; NX
DIV:  r[p->d] = r[p->a] / (r[p->b] | 1); p++; NX
REM:  r[p->d] = r[p->a] % (r[p->b] | 1); p++; NX
#undef NX
}

// ---- branch-free: all kinds computed, selected by opcode ----
#define CHEAP14(v, x, y, bi) \
  v[0] = x + y; v[1] = x - y; v[2] = x ^ y; v[3] = x & y; v[4] = x | y; \
  v[5] = x << (y & 31); v[6] = x >> (y & 31); v[7] = rol32(x, y); \
  v[8] = y < x ? y : x; v[9] = y > x ? y : x; v[10] = x < y; v[11] = x + 4 * (bi); \
  v[12] = x * (y | 1); v[13] = (uint32_t)(((uint64_t)x * y) >> 32);
__attribute__((noinline)) static void run_pred(const Op *p, long n, uint32_t *r) {
  for (long i = 0; i < n; i++) {
    Op o = p[i]; uint32_t x = r[o.a], y = r[o.b], v[16];
    CHEAP14(v, x, y, o.b)
    v[14] = x / (y | 1); v[15] = x % (y | 1);
    r[o.d] = v[o.op];
  }
}
__attribute__((noinline)) static void run_hybrid(const Op *p, long n, uint32_t *r) {
  for (long i = 0; i < n; i++) {
    Op o = p[i]; uint32_t x = r[o.a], y = r[o.b], v[16], res;
    if (__builtin_expect(o.op >= 14, 0)) res = o.op == 14 ? x / (y | 1) : x % (y | 1);
    else { CHEAP14(v, x, y, o.b) res = v[o.op]; }
    r[o.d] = res;
  }
}

// ---- run-once generator (K4 design: precomputed templates and lengths, one 16-byte
// store per op; the register fields are placed by one pshufb, no byte patching) ----
// Generated code: rdi = &R[0]; R[k] at [rdi+4k].
static const uint8_t TB[16][16] = {
  {0x8B,0x47,0, 0x03,0x47,0, 0x89,0x47,0},                         // ADD
  {0x8B,0x47,0, 0x2B,0x47,0, 0x89,0x47,0},                         // SUB
  {0x8B,0x47,0, 0x33,0x47,0, 0x89,0x47,0},                         // XOR
  {0x8B,0x47,0, 0x23,0x47,0, 0x89,0x47,0},                         // AND
  {0x8B,0x47,0, 0x0B,0x47,0, 0x89,0x47,0},                         // OR
  {0x8B,0x47,0, 0x8B,0x4F,0, 0xD3,0xE0, 0x89,0x47,0},              // SHL
  {0x8B,0x47,0, 0x8B,0x4F,0, 0xD3,0xE8, 0x89,0x47,0},              // SHR
  {0x8B,0x47,0, 0x8B,0x4F,0, 0xD3,0xC0, 0x89,0x47,0},              // ROL
  {0x8B,0x47,0, 0x8B,0x4F,0, 0x39,0xC1, 0x0F,0x42,0xC1, 0x89,0x47,0}, // MIN
  {0x8B,0x47,0, 0x8B,0x4F,0, 0x39,0xC1, 0x0F,0x47,0xC1, 0x89,0x47,0}, // MAX
  {0x31,0xD2, 0x8B,0x47,0, 0x3B,0x47,0, 0x0F,0x92,0xC2, 0x89,0x57,0}, // SLT
  {0x8B,0x47,0, 0x83,0xC0,0, 0x89,0x47,0},                         // ADDI
  {0x8B,0x47,0, 0x8B,0x4F,0, 0x83,0xC9,0x01, 0x0F,0xAF,0xC1, 0x89,0x47,0}, // MUL
  {0x8B,0x47,0, 0xF7,0x67,0, 0x89,0x57,0},                         // MULHI
  {0x8B,0x47,0, 0x8B,0x4F,0, 0x83,0xC9,0x01, 0x31,0xD2, 0xF7,0xF1, 0x89,0x47,0}, // DIV
  {0x8B,0x47,0, 0x8B,0x4F,0, 0x83,0xC9,0x01, 0x31,0xD2, 0xF7,0xF1, 0x89,0x57,0}, // REM
};
// patch positions: a (disp of first load), b (disp or imm), d (disp of store)
static const uint8_t TLEN[16] = {9,9,9,9,9,11,11,11,14,14,14,9,15,9,16,16};
static const int8_t PA[16] = {2,2,2,2,2,2,2,2,2,2,4,2,2,2,2,2};
static const int8_t PBp[16] = {5,5,5,5,5,5,5,5,5,5,7,-1,5,5,5,5};
static const int8_t PD[16] = {8,8,8,8,8,10,10,10,13,13,13,8,14,8,15,15};
typedef struct { __m128i tm, sh; uint64_t len, pad; } Tmpl;
static Tmpl TT[16];
static void build_templates(void) {
  for (int k = 0; k < 16; k++) {
    uint8_t s[16]; memset(s, 0x80, 16);
    // patch word = op word << 2, bytes: 0 = op*4, 1 = d*4, 2 = a*4, 3 = b*4 (disp or ADDI imm)
    s[PA[k]] = 2; s[PD[k]] = 1;
    s[PBp[k] >= 0 ? PBp[k] : 5] = 3;  // ADDI immediate at offset 5
    TT[k].tm = _mm_loadu_si128((const __m128i *)TB[k]);
    TT[k].sh = _mm_loadu_si128((const __m128i *)s);
    TT[k].len = TLEN[k];
  }
}
static inline uint8_t *emit_chunk(uint8_t *c, const Op *p, long m) {
  const uint32_t *w = (const uint32_t *)p;
  for (long i = 0; i < m; i++) {
    const Tmpl *t = &TT[p[i].op];
    __m128i pw = _mm_slli_epi32(_mm_cvtsi32_si128((int)w[i]), 2);
    _mm_storeu_si128((__m128i *)c, _mm_or_si128(t->tm, _mm_shuffle_epi8(pw, t->sh)));
    c += t->len;
  }
  *c++ = 0xC3; return c;
}
static uint8_t *RING; static long CHUNK = 500; static int NSLOT = 2; static long SLOT;
__attribute__((noinline)) static void run_jit(const Op *p, long n, uint32_t *r, int exec, uint64_t *bytes) {
  int k = 0; uint64_t gb = 0;
  for (long base = 0; base < n; base += CHUNK) {
    long m = n - base < CHUNK ? n - base : CHUNK;
    uint8_t *s = RING + k * SLOT; if (++k == NSLOT) k = 0;
    uint8_t *e = emit_chunk(s, p + base, m); gb += e - s;
    if (exec) ((void (*)(uint32_t *))s)(r);
  }
  if (!exec) r[0] ^= RING[(n * 7) % (NSLOT * SLOT)];  // keep the stores observable
  *bytes = gb;
}

static void build_stream(int q) {
  P = mmap(0, NOPS * sizeof(Op), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  uint64_t s = 1;
  for (long i = 0; i < NOPS; i++) {
    uint64_t z = sm(&s);
    unsigned op = ((z >> 32) % 1000) < (unsigned)q ? 0 : z & 15;
    P[i] = (Op){op, (z >> 8) & 15, (z >> 16) & 15, (z >> 24) & 15};
  }
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: k6syn MODE Q [log2n] [chunk]\n"); return 2; }
  const char *mode = argv[1]; int q = atoi(argv[2]);
  NOPS = 1L << (argc > 3 ? atoi(argv[3]) : 24);
  if (argc > 4) CHUNK = atol(argv[4]);
  if (getenv("K6_CTL")) { ctl = open(getenv("K6_CTL"), O_WRONLY); ack = open(getenv("K6_ACK"), O_RDONLY); }
  build_stream(q); build_templates();
  SLOT = (CHUNK * 16 + 64 + 63) & ~63L;
  RING = mmap(0, NSLOT * SLOT, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  memset(RING, 0xC3, NSLOT * SLOT);

  if (!strcmp(mode, "stats")) {
    long cnt[16] = {0}; for (long i = 0; i < NOPS; i++) cnt[P[i].op]++;
    double H = 0; for (int k = 0; k < 16; k++) if (cnt[k]) { double f = (double)cnt[k] / NOPS; H -= f * log2(f); }
    static const double lat[16] = {1,1,1,1,1,1,1,1,2,2,2,1,3,4,12,12};  // assumed latencies (estimate)
    long lu[16] = {0}; double lw[16] = {0}; long du = 0; double dw = 0;
    for (long i = 0; i < NOPS; i++) {
      Op o = P[i]; long u = 1 + (lu[o.a] > lu[o.b] ? lu[o.a] : lu[o.b]);
      double w = lat[o.op] + (lw[o.a] > lw[o.b] ? lw[o.a] : lw[o.b]);
      lu[o.d] = u; lw[o.d] = w; if (u > du) du = u; if (w > dw) dw = w;
    }
    printf("stats Q=%d nops=%ld entropy_bits=%.3f add_frac=%.3f divrem_frac=%.4f depth_unit=%ld ops_per_level=%.2f depth_weighted=%.0f weighted_cyc_per_op=%.3f\n",
           q, NOPS, H, (double)cnt[0] / NOPS, (double)(cnt[14] + cnt[15]) / NOPS, du, (double)NOPS / du, dw, dw / NOPS);
    return 0;
  }
  if (!strcmp(mode, "check")) {
    uint32_t r[16]; uint64_t h[4], gb; const char *nm[4] = {"cgoto", "pred", "hybrid", "jit"};
    init_regs(r); run_cgoto(P, NOPS, r); h[0] = hash_regs(r);
    uint32_t ref[16]; memcpy(ref, r, sizeof r);
    init_regs(r); run_pred(P, NOPS, r); h[1] = hash_regs(r); int ok = !memcmp(r, ref, sizeof r);
    init_regs(r); run_hybrid(P, NOPS, r); h[2] = hash_regs(r); ok &= !memcmp(r, ref, sizeof r);
    init_regs(r); run_jit(P, NOPS, r, 1, &gb); h[3] = hash_regs(r); ok &= !memcmp(r, ref, sizeof r);
    int distinct = 0; for (int i = 0; i < 16; i++) { int u = 1; for (int j = 0; j < i; j++) u &= ref[i] != ref[j]; distinct += u; }
    printf("check Q=%d nops=%ld", q, NOPS);
    for (int i = 0; i < 4; i++) printf(" %s=%016lx", nm[i], h[i]);
    printf(" distinct_regs=%d %s\n", distinct, ok ? "OK" : "FAIL");
    return !ok;
  }
  uint32_t r[16]; init_regs(r); uint64_t gb = 0;
  perf_cmd("enable\n");
  if (!strcmp(mode, "cgoto")) run_cgoto(P, NOPS, r);
  else if (!strcmp(mode, "pred")) run_pred(P, NOPS, r);
  else if (!strcmp(mode, "hybrid")) run_hybrid(P, NOPS, r);
  else if (!strcmp(mode, "jit")) run_jit(P, NOPS, r, 1, &gb);
  else if (!strcmp(mode, "gen")) run_jit(P, NOPS, r, 0, &gb);
  else { fprintf(stderr, "bad mode\n"); return 2; }
  perf_cmd("disable\n");
  printf("mode=%s Q=%d nops=%ld chunk=%ld hash=%016lx gen_bytes=%lu\n", mode, q, NOPS, CHUNK, hash_regs(r), gb);
  return 0;
}
