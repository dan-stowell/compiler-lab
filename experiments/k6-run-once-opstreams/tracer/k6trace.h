/* K6 bytecode tracer, #included into lvm.c (see tracer/build.sh).
 * At every instruction fetch it diffs the live stack [0, ci->top) against a shadow copy and
 * logs the changed slots as the effect of the previous instruction, then logs the new
 * instruction: pc identity, instruction word, base slot, and the K constant it reads (if any).
 * File layout: u32 n0, then per instruction { Hdr, u32 n, n x Wr } (n = slots it changed). */
#include <stdio.h>
#include <stdlib.h>
typedef struct { unsigned long long pcid; unsigned int ins, base; unsigned long long kv; unsigned int ktt, top; } K6Hdr;
typedef struct { unsigned long long v; unsigned int tt, slot; } K6Wr;
static FILE *k6f; static K6Wr *k6sh; static size_t k6cap; static int k6init;
static K6Wr *k6buf;
static void k6_end(void) { if (k6f) { unsigned int z = 0; fwrite(&z, 4, 1, k6f); fclose(k6f); k6f = NULL; } }
static void k6_trace(lua_State *L, CallInfo *ci, StkId base, const Instruction *pc, Instruction i, TValue *k) {
  if (!k6init) {
    k6init = 1; const char *fn = getenv("K6_TRACE");
    if (fn) { k6f = fopen(fn, "wb"); setvbuf(k6f, NULL, _IOFBF, 1 << 22); unsigned int z = 0; fwrite(&z, 4, 1, k6f); atexit(k6_end); }
  }
  if (!k6f) return;
  size_t top = (size_t)(ci->top.p - L->stack.p);
  if (top > k6cap) {
    size_t nc = top * 2 + 1024; k6sh = realloc(k6sh, nc * sizeof(K6Wr));
    for (size_t j = k6cap; j < nc; j++) { k6sh[j].v = 0; k6sh[j].tt = 0xff; k6sh[j].slot = (unsigned)j; }
    k6cap = nc; k6buf = realloc(k6buf, nc * sizeof(K6Wr));
  }
  unsigned int n = 0;
  for (size_t j = 0; j < top; j++) {
    TValue *o = s2v(L->stack.p + j); unsigned long long v; memcpy(&v, &o->value_, 8);
    unsigned int tt = rawtt(o);
    if (tt != k6sh[j].tt || v != k6sh[j].v) { k6sh[j].v = v; k6sh[j].tt = tt; k6buf[n++] = k6sh[j]; }
  }
  fwrite(&n, 4, 1, k6f); fwrite(k6buf, sizeof(K6Wr), n, k6f);
  K6Hdr h; h.pcid = (unsigned long long)(size_t)pc; h.ins = i; h.base = (unsigned int)(base - L->stack.p);
  h.kv = 0; h.ktt = 0xffff; h.top = (unsigned int)top;
  const TValue *kc = NULL;
  switch (GET_OPCODE(i)) {
    case OP_LOADK: kc = k + GETARG_Bx(i); break;
    case OP_LOADKX: kc = k + GETARG_Ax(*pc); break;
    case OP_ADDK: case OP_SUBK: case OP_MULK: case OP_MODK: case OP_POWK: case OP_DIVK:
    case OP_IDIVK: case OP_BANDK: case OP_BORK: case OP_BXORK: kc = k + GETARG_C(i); break;
    case OP_EQK: kc = k + GETARG_B(i); break;
    default: break;
  }
  if (kc) { memcpy(&h.kv, &kc->value_, 8); h.ktt = rawtt(kc); }
  fwrite(&h, sizeof h, 1, k6f);
}
