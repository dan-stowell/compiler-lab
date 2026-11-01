// E12: C port of E11's payload decoder (see dec.c).
#include <stddef.h>
#include <stdint.h>

enum { K_LABEL, K_IMM, K_SYM, K_DISP };
#define MAXV 4    // operand values per instruction (E11: at most 2)
#define MAXLEN 31 // longest Huffman code (E11: 12)

typedef struct {
  uint8_t maxlen;
  uint16_t count[MAXLEN + 1];
  int64_t *sym;  // values in canonical order (length, value)
} Huff;

typedef struct {
  uint8_t nk, term;
  uint8_t kind[MAXV];
  Huff *tab[MAXV];
} Skel;

typedef struct {
  uint32_t start;  // stream bit address of the instruction (of its symbol, for the first one)
  uint16_t si;     // skeleton index
  int64_t v[MAXV]; // operand values: label = stream bit address, others = coded value
} Insn;

typedef struct {
  const uint8_t *buf;
  uint32_t nsk, nrule, nsym, nfunc, W, stream_bits;
  int w;
  uint64_t base;
  Skel *sk;
  uint16_t *rules;
  uint32_t *entry;
  size_t table_bytes;  // resident decoder tables: skeleton descriptors, rules, Huffman, entries
  // rendering only (not counted): skeleton strings
  const char **mn;
  char ***ops;
  int *nops;
} Decoder;

int dec_init(Decoder *d, const uint8_t *buf);
size_t dec_run(const Decoder *d, uint64_t pos, int stop_at_term, Insn *out, size_t cap);
