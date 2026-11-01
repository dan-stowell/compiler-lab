// E12: C port of E11's payload decoder (core only; the driver is in e12dec.c).
// Reads E11's payload.bin layout (see ../e11-exec-compression-measured/e11.py, class Decoder):
// header, skeleton table, Re-Pair rules, per-slot canonical Huffman tables, function entry
// table, then the bit stream. Decodes instructions into records (start bit, skeleton, values).
#include "dec.h"
#include <stdlib.h>
#include <string.h>

// Big-endian bit order (MSB first), as e11.py's BitWriter. buf has >= 8 bytes of zero padding.
static inline uint64_t peek57(const uint8_t *buf, uint64_t p) {
  const uint8_t *q = buf + (p >> 3);
  uint64_t x = ((uint64_t)q[0] << 56) | ((uint64_t)q[1] << 48) | ((uint64_t)q[2] << 40) |
               ((uint64_t)q[3] << 32) | ((uint64_t)q[4] << 24) | ((uint64_t)q[5] << 16) |
               ((uint64_t)q[6] << 8) | (uint64_t)q[7];
  return x << (p & 7);
}

static inline uint64_t get(const uint8_t *buf, uint64_t *p, int n) {  // n <= 57
  if (n == 0)
    return 0;
  uint64_t v = peek57(buf, *p) >> (64 - n);
  *p += n;
  return v;
}

static uint64_t get64(const uint8_t *buf, uint64_t *p, int n) {  // n <= 64
  if (n <= 57)
    return get(buf, p, n);
  uint64_t hi = get(buf, p, n - 32);
  return (hi << 32) | get(buf, p, 32);
}

static uint64_t gamma_(const uint8_t *buf, uint64_t *p) {
  int z = 0;
  while (get(buf, p, 1) == 0)
    z++;
  return (z ? get64(buf, p, z) : 0) | ((uint64_t)1 << z);  // leading 1 already consumed
}

static int clog2(uint32_t n) {  // e9.py bits(): ceil(log2 n), 0 for n <= 1
  int b = 0;
  if (n <= 1)
    return 0;
  n--;
  while (n) {
    b++;
    n >>= 1;
  }
  return b;
}

static char *cstr(const uint8_t *buf, uint64_t *p) {
  char tmp[256];
  int n = 0;
  int c;
  while ((c = (int)get(buf, p, 8)) != 0)
    tmp[n++] = (char)c;
  tmp[n] = 0;
  return strdup(tmp);
}

static int slot_kind(const char *t) {
  if (!strcmp(t, "L"))
    return K_LABEL;
  if (!strcmp(t, "$I"))
    return K_IMM;
  if (!strcmp(t, "$S") || t[0] == 'S')
    return K_SYM;
  if (!strncmp(t, "D(", 2))
    return K_DISP;
  return -1;
}

// simple bump arena so the resident table bytes can be counted
static void *arena_alloc(Decoder *d, size_t n) {
  n = (n + 7) & ~(size_t)7;
  d->table_bytes += n;
  void *q = calloc(1, n);
  return q;
}

int dec_init(Decoder *d, const uint8_t *buf) {
  memset(d, 0, sizeof *d);
  d->buf = buf;
  uint64_t p = 0;
  uint32_t nmn = get(buf, &p, 32), nop = get(buf, &p, 32);
  d->nsk = get(buf, &p, 32);
  d->nrule = get(buf, &p, 32);
  d->nsym = get(buf, &p, 32);
  d->nfunc = get(buf, &p, 32);
  d->W = get(buf, &p, 32);
  d->stream_bits = get(buf, &p, 32);
  // mnemonic and operand-template strings: needed only to derive the slot kinds and for
  // rendering; not counted as decoder tables (they are part of the payload).
  char **mns = malloc(nmn * sizeof *mns), **ops = malloc(nop * sizeof *ops);
  for (uint32_t i = 0; i < nmn; i++)
    mns[i] = cstr(buf, &p);
  for (uint32_t i = 0; i < nop; i++)
    ops[i] = cstr(buf, &p);
  d->sk = arena_alloc(d, d->nsk * sizeof *d->sk);
  d->mn = malloc(d->nsk * sizeof *d->mn);
  d->ops = malloc(d->nsk * sizeof *d->ops);
  d->nops = malloc(d->nsk * sizeof *d->nops);
  for (uint32_t s = 0; s < d->nsk; s++) {
    d->mn[s] = mns[get(buf, &p, clog2(nmn))];
    int no = (int)gamma_(buf, &p) - 1;
    d->nops[s] = no;
    d->ops[s] = malloc((no ? no : 1) * sizeof(char *));
    int nk = 0;
    for (int j = 0; j < no; j++) {
      char *t = ops[get(buf, &p, clog2(nop))];
      d->ops[s][j] = t;
      int k = slot_kind(t);
      if (k >= 0) {
        if (nk >= MAXV)
          return -1;
        d->sk[s].kind[nk++] = (uint8_t)k;
      }
    }
    d->sk[s].nk = (uint8_t)nk;
    const char *m = d->mn[s];
    d->sk[s].term = (m[0] == 'j') || !strcmp(m, "call") || !strcmp(m, "ret");
  }
  d->w = clog2(d->nsk + d->nrule);
  d->rules = arena_alloc(d, d->nrule * 2 * sizeof *d->rules);
  for (uint32_t i = 0; i < 2 * d->nrule; i++)
    d->rules[i] = (uint16_t)get(buf, &p, d->w);
  for (uint32_t s = 0; s < d->nsk; s++) {
    for (int j = 0; j < d->sk[s].nk; j++) {
      if (d->sk[s].kind[j] == K_LABEL)
        continue;
      uint32_t n = (uint32_t)gamma_(buf, &p);
      int64_t *vals = malloc(n * sizeof *vals);
      uint64_t u = gamma_(buf, &p) - 1;
      vals[0] = (u & 1) ? -(int64_t)((u - 1) / 2) - 1 : (int64_t)(u / 2);
      for (uint32_t i = 1; i < n; i++)
        vals[i] = vals[i - 1] + (int64_t)gamma_(buf, &p);
      int lw = (int)gamma_(buf, &p) - 1;
      uint8_t *lens = malloc(n);
      int maxlen = 0;
      for (uint32_t i = 0; i < n; i++) {
        lens[i] = (uint8_t)get(buf, &p, lw);
        if (lens[i] > maxlen)
          maxlen = lens[i];
      }
      if (maxlen > MAXLEN)
        return -2;
      Huff *h = arena_alloc(d, sizeof *h);
      h->sym = arena_alloc(d, n * sizeof *h->sym);
      h->maxlen = (uint8_t)maxlen;
      // canonical order: by (length, value); vals are already sorted by value
      uint32_t k = 0;
      for (int l = 0; l <= maxlen; l++)
        for (uint32_t i = 0; i < n; i++)
          if (lens[i] == l) {
            h->sym[k++] = vals[i];
            h->count[l]++;
          }
      d->sk[s].tab[j] = h;
      free(vals);
      free(lens);
    }
  }
  d->entry = arena_alloc(d, d->nfunc * sizeof *d->entry);
  for (uint32_t i = 0; i < d->nfunc; i++)
    d->entry[i] = (uint32_t)get(buf, &p, d->W);
  p = (p + 7) & ~(uint64_t)7;
  d->base = p;
  free(mns);
  free(ops);
  return 0;
}

static inline int64_t huff(const Huff *h, const uint8_t *buf, uint64_t *p) {
  if (h->maxlen == 0)
    return h->sym[0];
  uint64_t win = peek57(buf, *p);
  int code = 0, first = 0, index = 0;
  for (int l = 1; l <= h->maxlen; l++) {
    code |= (int)(win >> 63);
    win <<= 1;
    int c = h->count[l];
    if (code - first < c) {
      *p += l;
      return h->sym[index + code - first];
    }
    index += c;
    first = (first + c) << 1;
    code <<= 1;
  }
  return 0;  // not reached for a valid stream
}

// Decode from stream bit address pos to the end of the stream (stop_at_term = 0) or through
// the first terminator (stop_at_term = 1). Returns the number of records written.
size_t dec_run(const Decoder *d, uint64_t pos, int stop_at_term, Insn *out, size_t cap) {
  const uint8_t *buf = d->buf;
  uint64_t p = d->base + pos, end = d->base + d->stream_bits;
  uint16_t stack[64];
  size_t n = 0;
  while (p < end) {
    int sp = 0;
    uint64_t sym_start = p;
    stack[sp++] = (uint16_t)get(buf, &p, d->w);
    while (sp) {
      uint32_t y = stack[--sp];
      if (y >= d->nsk) {
        stack[sp++] = d->rules[2 * (y - d->nsk) + 1];
        stack[sp++] = d->rules[2 * (y - d->nsk)];
        continue;
      }
      if (n >= cap)
        return n;
      Insn *r = &out[n++];
      const Skel *s = &d->sk[y];
      r->start = (uint32_t)(sym_start - d->base);
      r->si = (uint16_t)y;
      for (int j = 0; j < s->nk; j++)
        r->v[j] = s->kind[j] == K_LABEL ? (int64_t)get(buf, &p, d->W) : huff(s->tab[j], buf, &p);
      sym_start = p;
      if (stop_at_term && s->term)
        return n;
    }
  }
  return n;
}
