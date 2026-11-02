// K6 streams 2 and 3: Lua 5.4.6 bytecode replayed from a trace (tracer/k6trace.h).
//
// convert TRACE OUT.k6p full|once
//   Builds a replay program from a trace. Register file = the Lua stack (absolute slots;
//   base + A is resolved by the converter), followed by a constant area (K constants and
//   immediates of LOADI/LOADF/ADDI/EQI/... become constant slots, so ADDK == ADD on a constant).
//   Supported subset, computed with Lua 5.4.6 semantics: MOVE LOADI LOADF LOADK LOADKX
//   LOADFALSE LFALSESKIP LOADTRUE LOADNIL ADD SUB MUL DIV ADDI ADDK SUBK MULK DIVK BAND BOR
//   BXOR BANDK BORK BXORK SHL SHR SHLI SHRI NOT FORLOOP, and EQ LT LE EQK EQI LTI LEI GTI GEI
//   TEST whose numeric outcome is accumulated (the jump itself is already resolved by the
//   trace; JMP is dropped). Every other instruction becomes EXT: it applies the stack writes
//   the real VM made (logged by the tracer). Writes a supported op's logged effect has outside
//   the slots it may write (metamethod side effects) follow it as an EXT.
//   "once": keep only instructions whose pc executed exactly once in the trace; each maximal
//   run of other instructions collapses to one EXT with its net writes.
//   The converter replays its own output with the reference semantics and checks every
//   supported op's result against the real VM's logged value (mismatches must be 0).
// MODE PROG.k6p, MODE in: cgoto hybrid jit gen check stats   (measured region via perf --control)
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
#include "lopcodes.h"
#include "lopnames.h"

typedef struct { uint64_t u; uint32_t tt, pad; } TV;  // pad of a W entry = destination slot
typedef struct { uint8_t k, x; uint16_t n; uint32_t a, b, c; } Rec;
enum { K_MOVE, K_EXT, K_LOADNIL, K_ADD, K_SUB, K_MUL, K_DIV, K_BAND, K_BOR, K_BXOR, K_SHL, K_SHR,
       K_NOT, K_FORLOOP, K_EQ, K_LT, K_LE, K_TEST, NK };
static const char *KN[NK] = {"MOVE","EXT","LOADNIL","ADD","SUB","MUL","DIV","BAND","BOR","BXOR",
                             "SHL","SHR","NOT","FORLOOP","EQ","LT","LE","TEST"};
#define TNIL 0x00
#define TFALSE 0x01
#define TTRUE 0x11
#define TINT 0x03
#define TFLT 0x13

static inline double fv(const TV *o) { double d; memcpy(&d, &o->u, 8); return d; }
static inline void setf(TV *o, double d) { memcpy(&o->u, &d, 8); o->tt = TFLT; }
static inline void seti(TV *o, uint64_t i) { o->u = i; o->tt = TINT; }
static inline int isfalse(const TV *o) { return o->tt == TFALSE || (o->tt & 0x0f) == 0; }
static inline int tonum(const TV *o, double *d) {
  if (o->tt == TINT) { *d = (double)(int64_t)o->u; return 1; }
  if (o->tt == TFLT) { *d = fv(o); return 1; }
  return 0;
}
static inline int toint(const TV *o, int64_t *i) {  // luaV_tointegerns, mode F2Ieq
  if (o->tt == TINT) { *i = (int64_t)o->u; return 1; }
  if (o->tt == TFLT) {
    double f = fv(o); if (floor(f) != f) return 0;
    if (!(f >= -9223372036854775808.0 && f < 9223372036854775808.0)) return 0;
    *i = (int64_t)f; return 1;
  }
  return 0;
}
static inline int64_t shiftl(int64_t x, int64_t y) {
  if (y < 0) { if (y <= -64) return 0; return (int64_t)((uint64_t)x >> -y); }
  if (y >= 64) return 0; return (int64_t)((uint64_t)x << y);
}
static inline long double ld(const TV *o) { return o->tt == TINT ? (long double)(int64_t)o->u : (long double)fv(o); }
static inline int isnum(const TV *o) { return o->tt == TINT || o->tt == TFLT; }

// ---- the semantics of each kind (shared by every implementation) ----
#define OPF static inline __attribute__((always_inline))
OPF void o_move(TV *R, const Rec *p) { R[p->a] = R[p->b]; }
OPF void o_ext(TV *R, const TV *W, const Rec *p) { const TV *w = W + p->a; for (uint32_t j = 0; j < p->b; j++) R[w[j].pad] = w[j]; }
OPF void o_loadnil(TV *R, const Rec *p) { for (uint32_t j = 0; j <= p->b; j++) R[p->a + j].tt = TNIL; }
#define ARITH(name, iop, fop) \
OPF void o_##name(TV *R, const Rec *p) { const TV *x = &R[p->b], *y = &R[p->c]; double a, b; \
  if (x->tt == TINT && y->tt == TINT) seti(&R[p->a], x->u iop y->u); \
  else if (tonum(x, &a) && tonum(y, &b)) setf(&R[p->a], a fop b); }
ARITH(add, +, +) ARITH(sub, -, -) ARITH(mul, *, *)
OPF void o_div(TV *R, const Rec *p) { double a, b; if (tonum(&R[p->b], &a) && tonum(&R[p->c], &b)) setf(&R[p->a], a / b); }
#define BITW(name, expr) \
OPF void o_##name(TV *R, const Rec *p) { int64_t x, y; \
  if (toint(&R[p->b], &x) && toint(&R[p->c], &y)) seti(&R[p->a], (uint64_t)(expr)); }
BITW(band, x & y) BITW(bor, x | y) BITW(bxor, x ^ y) BITW(shl, shiftl(x, y)) BITW(shr, shiftl(x, (int64_t)(0 - (uint64_t)y)))
OPF void o_not(TV *R, const Rec *p) { R[p->a].tt = isfalse(&R[p->b]) ? TTRUE : TFALSE; }
OPF void o_forloop(TV *R, const Rec *p) {
  TV *ra = &R[p->a];
  if (ra[2].tt == TINT) {
    uint64_t count = ra[1].u;
    if (count > 0) { ra[1].u = count - 1; ra[0].u += ra[2].u; seti(&ra[3], ra[0].u); }
  } else {
    double step = fv(&ra[2]), limit = fv(&ra[1]), idx = fv(&ra[0]) + step;
    if (step > 0 ? idx <= limit : limit <= idx) { memcpy(&ra[0].u, &idx, 8); setf(&ra[3], idx); }
  }
}
OPF uint64_t o_eq(const TV *R, const Rec *p) {
  const TV *x = &R[p->b], *y = &R[p->c];
  if (isnum(x) && isnum(y)) return (x->tt == TINT && y->tt == TINT) ? x->u == y->u : ld(x) == ld(y);
  if (x->tt != y->tt) return 0;
  if ((x->tt & 0x0f) <= 1) return 1;  // nil, false, true
  return 2;                           // strings, tables, ...: not modelled
}
OPF uint64_t o_lt(const TV *R, const Rec *p) {
  const TV *x = &R[p->b], *y = &R[p->c];
  if (x->tt == TINT && y->tt == TINT) return (int64_t)x->u < (int64_t)y->u;
  if (isnum(x) && isnum(y)) return ld(x) < ld(y);
  return 2;
}
OPF uint64_t o_le(const TV *R, const Rec *p) {
  const TV *x = &R[p->b], *y = &R[p->c];
  if (x->tt == TINT && y->tt == TINT) return (int64_t)x->u <= (int64_t)y->u;
  if (isnum(x) && isnum(y)) return ld(x) <= ld(y);
  return 2;
}
OPF uint64_t o_test(const TV *R, const Rec *p) { return !isfalse(&R[p->a]); }

static void ref_exec(TV *R, const TV *W, const Rec *p, uint64_t *acc) {
  switch (p->k) {
    case K_MOVE: o_move(R, p); break; case K_EXT: o_ext(R, W, p); break; case K_LOADNIL: o_loadnil(R, p); break;
    case K_ADD: o_add(R, p); break; case K_SUB: o_sub(R, p); break; case K_MUL: o_mul(R, p); break;
    case K_DIV: o_div(R, p); break; case K_BAND: o_band(R, p); break; case K_BOR: o_bor(R, p); break;
    case K_BXOR: o_bxor(R, p); break; case K_SHL: o_shl(R, p); break; case K_SHR: o_shr(R, p); break;
    case K_NOT: o_not(R, p); break; case K_FORLOOP: o_forloop(R, p); break;
    case K_EQ: *acc += o_eq(R, p); break; case K_LT: *acc += o_lt(R, p); break;
    case K_LE: *acc += o_le(R, p); break; case K_TEST: *acc += o_test(R, p); break;
  }
}
static inline int canon_eq(const TV *x, const TV *y) { return x->tt == y->tt && ((x->tt & 0x0f) <= 1 || x->u == y->u); }
static uint64_t canon_hash(const TV *R, uint32_t n, uint64_t acc) {
  uint64_t h = 1469598103934665603ULL ^ acc;
  for (uint32_t i = 0; i < n; i++) { h = (h ^ R[i].tt) * 1099511628211ULL; if ((R[i].tt & 0x0f) > 1) h = (h ^ R[i].u) * 1099511628211ULL; }
  return h;
}

// ---- program file ----
typedef struct { uint64_t magic, nrec, nw, nregs, nslots, hash, acc, mismatches; } PHdr;
#define MAGIC 0x6b36707267000002ULL

// ================= converter =================
typedef struct { uint64_t pcid; uint32_t ins, base; uint64_t kv; uint32_t ktt, top; } Hdr;
typedef struct { uint64_t v; uint32_t tt, slot; } Wr;
static Rec *recs; static size_t nrec, caprec;
static TV *Wv; static size_t nw, capw;
static void push_rec(Rec r) { if (nrec == caprec) { caprec = caprec * 2 + 4096; recs = realloc(recs, caprec * sizeof(Rec)); } recs[nrec++] = r; }
static void push_w(uint64_t v, uint32_t tt, uint32_t slot) { if (nw == capw) { capw = capw * 2 + 4096; Wv = realloc(Wv, capw * sizeof(TV)); } Wv[nw++] = (TV){v, tt, slot}; }

static uint64_t *ht_k; static uint32_t *ht_v; static size_t ht_mask;
static uint32_t *ht_get(uint64_t k) {
  size_t i = (k * 0x9e3779b97f4a7c15ULL) >> 20 & ht_mask;
  while (ht_k[i] && ht_k[i] != k) i = (i + 1) & ht_mask;
  if (!ht_k[i]) ht_k[i] = k;
  return &ht_v[i];
}
// constants
static TV *consts; static size_t nconst, capconst; static uint32_t NS;
static uint64_t *ck; static uint32_t *cv; static size_t cmask = (1 << 16) - 1;
static uint32_t konst(uint32_t tt, uint64_t v) {
  if ((tt & 0x0f) <= 1) v = 0;
  uint64_t key = v * 31 + tt + 1; size_t i = (key * 0x9e3779b97f4a7c15ULL) >> 40 & cmask;
  while (ck[i]) { if (ck[i] == key && consts[cv[i]].u == v && consts[cv[i]].tt == tt) return NS + cv[i]; i = (i + 1) & cmask; }
  if (nconst == capconst) { capconst = capconst * 2 + 256; consts = realloc(consts, capconst * sizeof(TV)); }
  if (nconst * 2 > cmask) { fprintf(stderr, "too many constants\n"); exit(1); }
  consts[nconst] = (TV){v, tt, 0}; ck[i] = key; cv[i] = nconst; return NS + nconst++;
}
static uint32_t kint(int64_t i) { return konst(TINT, (uint64_t)i); }
static uint32_t kflt(double d) { uint64_t u; memcpy(&u, &d, 8); return konst(TFLT, u); }
static uint32_t knum(int isf, int v) { return isf ? kflt((double)v) : kint(v); }

typedef struct { FILE *f; } TR;
static int rd(TR *t, void *p, size_t n) { return fread(p, 1, n, t->f) == n; }
static Wr *L; static size_t capL;
static int read_writes(TR *t, uint32_t *n) {
  if (!rd(t, n, 4)) return 0;
  if (*n > capL) { capL = *n * 2; L = realloc(L, capL * sizeof(Wr)); }
  return rd(t, L, (size_t)*n * sizeof(Wr));
}

static int convert(const char *trace, const char *out, int once) {
  // pass 1: pc counts, register-file size
  TR t = {fopen(trace, "rb")}; if (!t.f) { perror(trace); return 1; }
  setvbuf(t.f, NULL, _IOFBF, 1 << 22);
  ht_mask = (1 << 22) - 1; ht_k = calloc(ht_mask + 1, 8); ht_v = calloc(ht_mask + 1, 4);
  uint32_t n; Hdr h; uint64_t ninstr = 0; uint32_t maxslot = 0;
  read_writes(&t, &n); read_writes(&t, &n);
  for (uint32_t j = 0; j < n; j++) if (L[j].slot >= maxslot) maxslot = L[j].slot + 1;
  while (rd(&t, &h, sizeof h)) {
    ninstr++; (*ht_get(h.pcid))++;
    if (h.top > maxslot) maxslot = h.top;
    if (!read_writes(&t, &n)) { fprintf(stderr, "truncated\n"); return 1; }
    for (uint32_t j = 0; j < n; j++) if (L[j].slot >= maxslot) maxslot = L[j].slot + 1;
  }
  NS = maxslot + 8;
  uint64_t distinct = 0, once_pcs = 0;
  for (size_t i = 0; i <= ht_mask; i++) if (ht_k[i]) { distinct++; once_pcs += ht_v[i] == 1; }
  // pass 2
  rewind(t.f);
  ck = calloc(cmask + 1, 8); cv = calloc(cmask + 1, 4);
  size_t RC = NS + 70000;
  TV *S = calloc(RC, sizeof(TV)), *R = calloc(RC, sizeof(TV)), *init = calloc(NS, sizeof(TV));
  int32_t *pend = malloc(NS * sizeof(int32_t)); memset(pend, -1, NS * sizeof(int32_t));
  Wr *pl = malloc(NS * sizeof(Wr)); uint32_t npl = 0;
  read_writes(&t, &n); read_writes(&t, &n);
  for (uint32_t j = 0; j < n; j++) { TV v = {L[j].v, L[j].tt, 0}; S[L[j].slot] = R[L[j].slot] = init[L[j].slot] = v; }
  uint64_t acc = 0, mism = 0, kept = 0, supported = 0, extra_ext = 0, dropped_jmp = 0, op_hist[NUM_OPCODES] = {0};
  uint32_t may[4]; size_t synced = 0;
  while (rd(&t, &h, sizeof h)) {
    read_writes(&t, &n);
    if (once && *ht_get(h.pcid) != 1) {  // collapse
      for (uint32_t j = 0; j < n; j++) {
        uint32_t s = L[j].slot; S[s] = (TV){L[j].v, L[j].tt, 0};
        if (pend[s] < 0) { pend[s] = npl; pl[npl++] = L[j]; } else pl[pend[s]] = L[j];
      }
      continue;
    }
    if (npl) {  // flush the collapsed run
      Rec r = {K_EXT, 0, 0, (uint32_t)nw, npl, 0};
      for (uint32_t j = 0; j < npl; j++) { push_w(pl[j].v, pl[j].tt, pl[j].slot); pend[pl[j].slot] = -1; }
      push_rec(r); ref_exec(R, Wv, &recs[nrec - 1], &acc); npl = 0;
    }
    kept++;
    uint32_t i = h.ins, base = h.base; OpCode op = GET_OPCODE(i); op_hist[op]++;
    uint32_t A = base + GETARG_A(i), B = base + GETARG_B(i), C = base + GETARG_C(i);
    int sB = GETARG_sB(i), sC = GETARG_sC(i), isf = GETARG_C(i);
    Rec r = {NK, 0, 0, A, 0, 0}; int nmay = 0;
    switch (op) {
      case OP_MOVE: r.k = K_MOVE; r.b = B; break;
      case OP_LOADI: r.k = K_MOVE; r.b = kint(GETARG_sBx(i)); break;
      case OP_LOADF: r.k = K_MOVE; r.b = kflt((double)GETARG_sBx(i)); break;
      case OP_LOADK: case OP_LOADKX: r.k = K_MOVE; r.b = konst(h.ktt, h.kv); break;
      case OP_LOADFALSE: case OP_LFALSESKIP: r.k = K_MOVE; r.b = konst(TFALSE, 0); break;
      case OP_LOADTRUE: r.k = K_MOVE; r.b = konst(TTRUE, 0); break;
      case OP_LOADNIL: r.k = K_LOADNIL; r.b = GETARG_B(i); break;
      case OP_ADD: r.k = K_ADD; r.b = B; r.c = C; break;
      case OP_SUB: r.k = K_SUB; r.b = B; r.c = C; break;
      case OP_MUL: r.k = K_MUL; r.b = B; r.c = C; break;
      case OP_DIV: r.k = K_DIV; r.b = B; r.c = C; break;
      case OP_BAND: r.k = K_BAND; r.b = B; r.c = C; break;
      case OP_BOR: r.k = K_BOR; r.b = B; r.c = C; break;
      case OP_BXOR: r.k = K_BXOR; r.b = B; r.c = C; break;
      case OP_SHL: r.k = K_SHL; r.b = B; r.c = C; break;
      case OP_SHR: r.k = K_SHR; r.b = B; r.c = C; break;
      case OP_ADDI: r.k = K_ADD; r.b = B; r.c = kint(sC); break;
      case OP_ADDK: r.k = K_ADD; r.b = B; r.c = konst(h.ktt, h.kv); break;
      case OP_SUBK: r.k = K_SUB; r.b = B; r.c = konst(h.ktt, h.kv); break;
      case OP_MULK: r.k = K_MUL; r.b = B; r.c = konst(h.ktt, h.kv); break;
      case OP_DIVK: r.k = K_DIV; r.b = B; r.c = konst(h.ktt, h.kv); break;
      case OP_BANDK: r.k = K_BAND; r.b = B; r.c = konst(h.ktt, h.kv); break;
      case OP_BORK: r.k = K_BOR; r.b = B; r.c = konst(h.ktt, h.kv); break;
      case OP_BXORK: r.k = K_BXOR; r.b = B; r.c = konst(h.ktt, h.kv); break;
      case OP_SHRI: r.k = K_SHR; r.b = B; r.c = kint(sC); break;
      case OP_SHLI: r.k = K_SHL; r.b = kint(sC); r.c = B; break;
      case OP_NOT: r.k = K_NOT; r.b = B; break;
      case OP_FORLOOP: r.k = K_FORLOOP; break;
      case OP_EQ: r.k = K_EQ; r.b = A; r.c = B; break;
      case OP_LT: r.k = K_LT; r.b = A; r.c = B; break;
      case OP_LE: r.k = K_LE; r.b = A; r.c = B; break;
      case OP_EQK: r.k = K_EQ; r.b = A; r.c = konst(h.ktt, h.kv); break;
      case OP_EQI: r.k = K_EQ; r.b = A; r.c = knum(isf, sB); break;
      case OP_LTI: r.k = K_LT; r.b = A; r.c = knum(isf, sB); break;
      case OP_LEI: r.k = K_LE; r.b = A; r.c = knum(isf, sB); break;
      case OP_GTI: r.k = K_LT; r.b = knum(isf, sB); r.c = A; break;
      case OP_GEI: r.k = K_LE; r.b = knum(isf, sB); r.c = A; break;
      case OP_TEST: r.k = K_TEST; break;
      case OP_JMP: r.k = NK + 1; break;
      default: break;
    }
    if (NS + nconst + 2 > RC) { fprintf(stderr, "constant area overflow\n"); return 1; }
    for (; synced < nconst; synced++) R[NS + synced] = consts[synced];
    switch (r.k) {
      case K_MOVE: case K_ADD: case K_SUB: case K_MUL: case K_DIV: case K_BAND: case K_BOR:
      case K_BXOR: case K_SHL: case K_SHR: case K_NOT: may[0] = A; nmay = 1; break;
      case K_FORLOOP: may[0] = A; may[1] = A + 1; may[2] = A + 3; nmay = 3; break;
      default: break;
    }
    if (r.k == NK) {  // unsupported: EXT with the real VM's writes
      Rec e = {K_EXT, 0, 0, (uint32_t)nw, n, 0};
      for (uint32_t j = 0; j < n; j++) { push_w(L[j].v, L[j].tt, L[j].slot); S[L[j].slot] = (TV){L[j].v, L[j].tt, 0}; }
      push_rec(e); ref_exec(R, Wv, &recs[nrec - 1], &acc);
      continue;
    }
    if (r.k == NK + 1) dropped_jmp++; else { supported++; push_rec(r); ref_exec(R, Wv, &r, &acc); }
    // apply the logged effect to the shadow; writes the op cannot make become an EXT
    uint32_t extra0 = nw, nextra = 0;
    for (uint32_t j = 0; j < n; j++) {
      uint32_t s = L[j].slot; S[s] = (TV){L[j].v, L[j].tt, 0};
      int in = 0; for (int m = 0; m < nmay; m++) in |= may[m] == s;
      if (r.k == K_LOADNIL) in = s >= A && s <= A + r.b;
      if (!in) { push_w(L[j].v, L[j].tt, s); nextra++; }
    }
    if (nextra) { Rec e = {K_EXT, 0, 0, extra0, nextra, 0}; push_rec(e); ref_exec(R, Wv, &e, &acc); extra_ext++; }
    // check the computed result against the real VM
    uint32_t lo = A, hi = A; if (r.k == K_LOADNIL) hi = A + r.b; else if (r.k == K_FORLOOP) hi = A + 3; else if (nmay == 0) hi = A - 1;
    for (uint32_t s = lo; s <= hi && s + 1 > lo; s++)
      if (!canon_eq(&R[s], &S[s])) {
        if (mism < 5) fprintf(stderr, "mismatch op=%s slot=%u got tt=%x u=%lx want tt=%x u=%lx\n", opnames[op], s, R[s].tt, R[s].u, S[s].tt, S[s].u);
        mism++; R[s] = S[s];
      }
  }
  if (npl) {
    Rec r = {K_EXT, 0, 0, (uint32_t)nw, npl, 0};
    for (uint32_t j = 0; j < npl; j++) push_w(pl[j].v, pl[j].tt, pl[j].slot);
    push_rec(r); ref_exec(R, Wv, &recs[nrec - 1], &acc);
  }
  fclose(t.f);
  // full-state check: replay state == real VM shadow on every stack slot
  uint64_t final_diff = 0; for (uint32_t s = 0; s < NS; s++) final_diff += !canon_eq(&R[s], &S[s]);
  uint32_t nregs = NS + nconst;
  PHdr ph = {MAGIC, nrec, nw, nregs, NS, canon_hash(R, nregs, acc), acc, mism};
  FILE *o = fopen(out, "wb");
  fwrite(&ph, sizeof ph, 1, o); fwrite(init, sizeof(TV), NS, o); fwrite(consts, sizeof(TV), nconst, o);
  fwrite(recs, sizeof(Rec), nrec, o); fwrite(Wv, sizeof(TV), nw, o); fclose(o);
  printf("convert %s mode=%s instructions=%lu distinct_pcs=%lu once_pcs=%lu kept=%lu (%.1f%%) recs=%zu writes=%zu "
         "supported=%lu dropped_jmp=%lu side_effect_ext=%lu slots=%u consts=%zu mismatches=%lu final_slot_diffs=%lu\n",
         trace, once ? "once" : "full", ninstr, distinct, once_pcs, kept, 100.0 * kept / ninstr, nrec, nw,
         supported, dropped_jmp, extra_ext, NS, nconst, mism, final_diff);
  printf("opcodes kept:"); for (int k = 0; k < NUM_OPCODES; k++) if (op_hist[k]) printf(" %s=%lu", opnames[k], op_hist[k]); printf("\n");
  return mism || final_diff;
}

// ================= replay =================
static TV *REG; static const TV *WB; static Rec *P; static PHdr H; static uint64_t ACC;
static int ctl = -1, ack = -1;
static void perf_cmd(const char *c) {
  if (ctl < 0) return;
  if (write(ctl, c, strlen(c)) < 0) perror("ctl");
  char b[16]; if (read(ack, b, sizeof b) < 0) perror("ack");
}
static TV *INIT;
static void load(const char *fn) {
  FILE *f = fopen(fn, "rb"); if (!f || fread(&H, sizeof H, 1, f) != 1 || H.magic != MAGIC) { fprintf(stderr, "bad program %s\n", fn); exit(1); }
  // one arena: registers then W, so generated code can reach both from one base
  size_t rb = (H.nregs * sizeof(TV) + 4095) & ~4095UL, wb = H.nw * sizeof(TV);
  uint8_t *ar = mmap(0, rb + wb + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  REG = (TV *)ar; WB = (TV *)(ar + rb); INIT = malloc(H.nregs * sizeof(TV));
  P = mmap(0, H.nrec * sizeof(Rec) + 64, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (fread(INIT, sizeof(TV), H.nregs, f) != H.nregs || fread(P, sizeof(Rec), H.nrec, f) != H.nrec ||
      fread((void *)WB, sizeof(TV), H.nw, f) != H.nw) { fprintf(stderr, "short program\n"); exit(1); }
  fclose(f);
}
static void reset(void) { memcpy(REG, INIT, H.nregs * sizeof(TV)); ACC = 0; }

__attribute__((noinline)) static void run_cgoto(TV *R, const TV *W, const Rec *p, size_t n) {
  static void *Lb[NK] = {&&MOVE, &&EXT, &&LOADNIL, &&ADD, &&SUB, &&MUL, &&DIV, &&BAND, &&BOR, &&BXOR,
                         &&SHL, &&SHR, &&NOT, &&FORLOOP, &&EQ, &&LT, &&LE, &&TEST};
  const Rec *e = p + n; uint64_t acc = 0;
#define NX if (++p == e) goto END; goto *Lb[p->k];
  if (p == e) goto END; goto *Lb[p->k];
MOVE: o_move(R, p); NX
EXT: o_ext(R, W, p); NX
LOADNIL: o_loadnil(R, p); NX
ADD: o_add(R, p); NX
SUB: o_sub(R, p); NX
MUL: o_mul(R, p); NX
DIV: o_div(R, p); NX
BAND: o_band(R, p); NX
BOR: o_bor(R, p); NX
BXOR: o_bxor(R, p); NX
SHL: o_shl(R, p); NX
SHR: o_shr(R, p); NX
NOT: o_not(R, p); NX
FORLOOP: o_forloop(R, p); NX
EQ: acc += o_eq(R, p); NX
LT: acc += o_lt(R, p); NX
LE: acc += o_le(R, p); NX
TEST: acc += o_test(R, p); NX
END: ACC = acc;
#undef NX
}
// hybrid: MOVE and the integer fast path of ADD/SUB/MUL/BAND/BOR/BXOR are one branch-free
// block (values computed for all, selected by kind); one well-predicted branch guards it,
// everything else dispatches through the computed-goto table.
__attribute__((noinline)) static void run_hybrid(TV *R, const TV *W, const Rec *p, size_t n) {
  static void *Lb[NK] = {&&MOVE, &&EXT, &&LOADNIL, &&ADD, &&SUB, &&MUL, &&DIV, &&BAND, &&BOR, &&BXOR,
                         &&SHL, &&SHR, &&NOT, &&FORLOOP, &&EQ, &&LT, &&LE, &&TEST};
  const Rec *e = p + n; uint64_t acc = 0;
  // kinds in the predicated group: MOVE(0) ADD(3) SUB(4) MUL(5) BAND(7) BOR(8) BXOR(9)
  const uint32_t grp = 1u << K_MOVE | 1u << K_ADD | 1u << K_SUB | 1u << K_MUL | 1u << K_BAND | 1u << K_BOR | 1u << K_BXOR;
#define NX if (++p == e) goto END; goto TOP;
TOP:
  if (p == e) goto END;
  {
    unsigned k = p->k; const TV *x = &R[p->b], *y = &R[p->c];
    int fast = (grp >> k & 1) && (k == K_MOVE || (x->tt == TINT && y->tt == TINT));
    if (__builtin_expect(fast, 1)) {
      uint64_t a = x->u, b = y->u, v[NK];
      v[K_MOVE] = a; v[K_ADD] = a + b; v[K_SUB] = a - b; v[K_MUL] = a * b;
      v[K_BAND] = a & b; v[K_BOR] = a | b; v[K_BXOR] = a ^ b;
      uint32_t tt = k == K_MOVE ? x->tt : TINT;
      R[p->a].u = v[k]; R[p->a].tt = tt; NX
    }
    goto *Lb[k];
  }
MOVE: o_move(R, p); NX
EXT: o_ext(R, W, p); NX
LOADNIL: o_loadnil(R, p); NX
ADD: o_add(R, p); NX
SUB: o_sub(R, p); NX
MUL: o_mul(R, p); NX
DIV: o_div(R, p); NX
BAND: o_band(R, p); NX
BOR: o_bor(R, p); NX
BXOR: o_bxor(R, p); NX
SHL: o_shl(R, p); NX
SHR: o_shr(R, p); NX
NOT: o_not(R, p); NX
FORLOOP: o_forloop(R, p); NX
EQ: acc += o_eq(R, p); NX
LT: acc += o_lt(R, p); NX
LE: acc += o_le(R, p); NX
TEST: acc += o_test(R, p); NX
END: ACC = acc;
#undef NX
}

// ---- run-once: MOVE inlined (two 16-byte moves, register base in rbx); every other kind is a
// direct call to its handler with rsi = &record (call-threaded code: no indirect dispatch).
// Generator (K4 design): every record is one 16-byte template at a fixed 16-byte stride; the
// variable fields are added by one pshufb of {i*16, a*16, b*16, -(next ip)} chosen per kind:
//   MOVE: c5 f8 10 83 [b*16]  c5 f8 11 83 [a*16]        vmovups xmm0,[rbx+b]; vmovups [rbx+a],xmm0
//   call: 49 8d b4 24 [i*16]  0f 1f 00  e8 [h - next]    lea rsi,[r12+i*16]; nop3; call h
#define HANDLER(name, body) __attribute__((noinline)) static void h_##name(void *u, const Rec *p) { (void)u; body; }
HANDLER(EXT, o_ext(REG, WB, p)) HANDLER(LOADNIL, o_loadnil(REG, p)) HANDLER(ADD, o_add(REG, p))
HANDLER(SUB, o_sub(REG, p)) HANDLER(MUL, o_mul(REG, p)) HANDLER(DIV, o_div(REG, p))
HANDLER(BAND, o_band(REG, p)) HANDLER(BOR, o_bor(REG, p)) HANDLER(BXOR, o_bxor(REG, p))
HANDLER(SHL, o_shl(REG, p)) HANDLER(SHR, o_shr(REG, p)) HANDLER(NOT, o_not(REG, p))
HANDLER(FORLOOP, o_forloop(REG, p)) HANDLER(EQ, ACC += o_eq(REG, p)) HANDLER(LT, ACC += o_lt(REG, p))
HANDLER(LE, ACC += o_le(REG, p)) HANDLER(TEST, ACC += o_test(REG, p)) HANDLER(MOVE, o_move(REG, p))
static void (*HND[NK])(void *, const Rec *) = {h_MOVE, h_EXT, h_LOADNIL, h_ADD, h_SUB, h_MUL, h_DIV, h_BAND,
  h_BOR, h_BXOR, h_SHL, h_SHR, h_NOT, h_FORLOOP, h_EQ, h_LT, h_LE, h_TEST};
static uint8_t *RING; static long CHUNK = 500; static long SLOT; static int NSLOT = 2;
// template index: the kind, except EXT with 0 writes -> EMPTY (nothing emitted) and EXT with
// exactly 1 write -> EXT1 (inline vmovups from the W entry to its slot; W follows the registers)
enum { T_EMPTY = NK, T_EXT1, NT };
static uint8_t TMAP[NK * 3];
static __m128i GT[NT], GS[NT], GW[NT], GSTEP[NT]; static uint64_t GLEN[NT]; static uint32_t WOFF16;
static void build_gen(void) {
  static const uint8_t mv[16] = {0xC5,0xF8,0x10,0x83,0,0,0,0, 0xC5,0xF8,0x11,0x83,0,0,0,0};
  static const uint8_t cl[16] = {0x49,0x8D,0xB4,0x24,0,0,0,0, 0x0F,0x1F,0x00,0xE8,0,0,0,0};
  for (int k = 0; k < NK; k++) for (int j = 0; j < 3; j++) TMAP[k * 3 + j] = k == K_EXT ? (j == 0 ? T_EMPTY : j == 1 ? T_EXT1 : K_EXT) : k;
  WOFF16 = (uint32_t)((const uint8_t *)WB - (const uint8_t *)REG);
  for (int k = 0; k < NT; k++) {
    uint8_t t[16], sh[16], sw[16]; memset(sh, 0x80, 16); memset(sw, 0x80, 16); memset(t, 0, 16);
    // source dwords: 0 = i*16, 1 = a*16, 2 = b*16, 3 = -(address after this template);
    // second source dword 0 = (slot of W[a]) * 16
    if (k == K_MOVE) { memcpy(t, mv, 16); for (int j = 0; j < 4; j++) { sh[4 + j] = 8 + j; sh[12 + j] = 4 + j; } }
    else if (k == T_EXT1) { memcpy(t, mv, 16); memcpy(t + 4, &WOFF16, 4);
      for (int j = 0; j < 4; j++) { sh[4 + j] = 4 + j; sw[12 + j] = j; } }
    else if (k != T_EMPTY) { memcpy(t, cl, 16); uint32_t h = (uint32_t)(uintptr_t)HND[k]; memcpy(t + 12, &h, 4);
      for (int j = 0; j < 4; j++) { sh[4 + j] = j; sh[12 + j] = 12 + j; } }
    GT[k] = _mm_loadu_si128((const __m128i *)t); GS[k] = _mm_loadu_si128((const __m128i *)sh);
    GW[k] = _mm_loadu_si128((const __m128i *)sw);
    GLEN[k] = k == T_EMPTY ? 0 : 16; GSTEP[k] = _mm_set_epi32(-(int)GLEN[k], 0, 0, 16);
  }
}
static inline uint8_t *emit_chunk(uint8_t *c, const Rec *p, long m) {
  static const uint8_t pro[13] = {0x53, 0x41,0x54, 0x48,0x83,0xEC,0x08, 0x48,0x89,0xFB, 0x49,0x89,0xF4};
  static const uint8_t epi[8] = {0x48,0x83,0xC4,0x08, 0x41,0x5C, 0x5B, 0xC3};
  memcpy(c, pro, 13); c += 13;
  __m128i iv = _mm_set_epi32(-(int)(uint32_t)(uintptr_t)c, 0, 0, 0);
  for (long i = 0; i < m; i++) {
    __m128i rv = _mm_slli_epi32(_mm_loadu_si128((const __m128i *)&p[i]), 4);
    unsigned k = p[i].k, nb = p[i].b, nbc = nb > 2 ? 2 : nb;
    unsigned t = TMAP[k * 3 + nbc]; uint32_t xm = -(uint32_t)(k == K_EXT);
    __m128i ivn = _mm_add_epi32(iv, GSTEP[t]);           // d3 = -(address after this template)
    __m128i src = _mm_blend_epi32(rv, ivn, 0x8);
    src = _mm_blend_epi32(src, iv, 0x1);                  // d0 = i*16
    __m128i wv = _mm_cvtsi32_si128((int)(WB[p[i].a & xm].pad << 4));
    __m128i v = _mm_add_epi32(GT[t], _mm_shuffle_epi8(src, GS[t]));
    _mm_storeu_si128((__m128i *)c, _mm_add_epi32(v, _mm_shuffle_epi8(wv, GW[t])));
    c += GLEN[t]; iv = ivn;
  }
  memcpy(c, epi, 8); return c + 8;
}
__attribute__((noinline)) static void run_jit(const Rec *p, size_t n, int exec, uint64_t *bytes) {
  int k = 0; uint64_t gb = 0;
  for (size_t base = 0; base < n; base += CHUNK) {
    long m = n - base < (size_t)CHUNK ? (long)(n - base) : CHUNK;
    uint8_t *s = RING + k * SLOT; if (++k == NSLOT) k = 0;
    uint8_t *e = emit_chunk(s, p + base, m); gb += e - s;
    if (exec) ((void (*)(TV *, const Rec *))s)(REG, p + base);
  }
  if (!exec) ACC ^= RING[(n * 7) % (NSLOT * SLOT)];
  *bytes = gb;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: k6lua convert TRACE OUT full|once | k6lua MODE PROG [chunk]\n"); return 2; }
  if (!strcmp(argv[1], "convert")) return convert(argv[2], argv[3], argc > 4 && !strcmp(argv[4], "once"));
  const char *mode = argv[1];
  if (argc > 3) CHUNK = atol(argv[3]);
  if (getenv("K6_CTL")) { ctl = open(getenv("K6_CTL"), O_WRONLY); ack = open(getenv("K6_ACK"), O_RDONLY); }
  load(argv[2]);
  SLOT = (CHUNK * 16 + 64 + 63) & ~63L;
  // the ring must be within rel32 reach of the handlers: ask for an address 256 MiB below the text
  void *hint = (void *)(((uintptr_t)&h_EXT & ~0xfffffUL) - (256UL << 20));
  RING = mmap(hint, NSLOT * SLOT, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_FIXED_NOREPLACE, -1, 0);
  if (RING == MAP_FAILED || llabs((intptr_t)RING - (intptr_t)&h_EXT) > (1L << 30)) { fprintf(stderr, "ring not near text\n"); return 1; }
  memset(RING, 0xC3, NSLOT * SLOT); build_gen();
  if (H.nregs * 16 + H.nw * 16 + 8192 > 0x7fffffffUL) { fprintf(stderr, "arena too large for disp32\n"); return 1; }
  if (!strcmp(mode, "stats")) {
    uint64_t cnt[NK] = {0}, extw = 0; for (size_t i = 0; i < H.nrec; i++) { cnt[P[i].k]++; if (P[i].k == K_EXT) extw += P[i].b; }
    // dependency depth through registers (unit latency per record; EXT writes depend on nothing)
    uint32_t *lv = calloc(H.nregs, 4); uint64_t depth = 0;
    for (size_t i = 0; i < H.nrec; i++) {
      const Rec *p = &P[i]; uint32_t d = 0;
      switch (p->k) {
        case K_EXT: for (uint32_t j = 0; j < p->b; j++) lv[WB[p->a + j].pad] = 1; d = 1; break;
        case K_LOADNIL: for (uint32_t j = 0; j <= p->b; j++) lv[p->a + j] = 1; d = 1; break;
        case K_MOVE: case K_NOT: d = lv[p->b] + 1; lv[p->a] = d; break;
        case K_FORLOOP: { uint32_t m = lv[p->a] > lv[p->a + 1] ? lv[p->a] : lv[p->a + 1]; if (lv[p->a + 2] > m) m = lv[p->a + 2];
          d = m + 1; lv[p->a] = lv[p->a + 1] = lv[p->a + 3] = d; break; }
        case K_TEST: d = lv[p->a] + 1; break;
        case K_EQ: case K_LT: case K_LE: d = (lv[p->b] > lv[p->c] ? lv[p->b] : lv[p->c]) + 1; break;
        default: d = (lv[p->b] > lv[p->c] ? lv[p->b] : lv[p->c]) + 1; lv[p->a] = d; break;
      }
      if (d > depth) depth = d;
    }
    printf("stats %s recs=%lu writes=%lu ext_writes_per_ext=%.2f depth=%lu recs_per_level=%.1f mismatches=%lu\n  kinds:",
           argv[2], H.nrec, H.nw, cnt[K_EXT] ? (double)extw / cnt[K_EXT] : 0, depth, (double)H.nrec / depth, H.mismatches);
    double Hn = 0;
    for (int k = 0; k < NK; k++) if (cnt[k]) { double f = (double)cnt[k] / H.nrec; Hn -= f * log2(f); printf(" %s=%.3f", KN[k], f); }
    printf("\n  kind_entropy_bits=%.3f\n", Hn);
    return 0;
  }
  if (!strcmp(mode, "check")) {
    uint64_t h[3]; int ok = 1; uint64_t gb;
    for (int m = 0; m < 3; m++) {
      reset();
      if (m == 0) run_cgoto(REG, WB, P, H.nrec); else if (m == 1) run_hybrid(REG, WB, P, H.nrec); else run_jit(P, H.nrec, 1, &gb);
      h[m] = canon_hash(REG, H.nregs, ACC); ok &= h[m] == H.hash;
    }
    printf("check %s expected=%016lx cgoto=%016lx hybrid=%016lx jit=%016lx acc=%lu %s\n", argv[2], H.hash, h[0], h[1], h[2], H.acc, ok ? "OK" : "FAIL");
    return !ok;
  }
  reset(); uint64_t gb = 0;
  perf_cmd("enable\n");
  if (!strcmp(mode, "cgoto")) run_cgoto(REG, WB, P, H.nrec);
  else if (!strcmp(mode, "hybrid")) run_hybrid(REG, WB, P, H.nrec);
  else if (!strcmp(mode, "jit")) run_jit(P, H.nrec, 1, &gb);
  else if (!strcmp(mode, "gen")) run_jit(P, H.nrec, 0, &gb);
  else { fprintf(stderr, "bad mode\n"); return 2; }
  perf_cmd("disable\n");
  int ok = strcmp(mode, "gen") ? canon_hash(REG, H.nregs, ACC) == H.hash : 1;
  printf("mode=%s prog=%s recs=%lu chunk=%ld gen_bytes=%lu state=%s\n", mode, argv[2], H.nrec, CHUNK, gb, ok ? "OK" : "FAIL");
  return !ok;
}
