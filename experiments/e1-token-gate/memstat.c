// E1 allocation accounting, linked into chibicc by run.sh.
//
// Defines malloc/calloc/realloc/reallocarray/free (forwarding to glibc's
// __libc_* entry points) so every heap allocation of the process, including
// those libc makes for chibicc (strdup, open_memstream, stdio), is recorded in
// a pointer -> (size, kind, phase, mark) table kept in mmap'd memory.
//
// Byte accounting: "allocated" = sum of malloc/calloc sizes plus, for realloc,
// max(0, new - old). Sizes are requested sizes, not allocator-rounded ones.
//
// After parse() returns, memstat_walk() marks every Token directly pointed to
// by an Obj, Node, Type or Member reachable from the program, and memstat_report()
// prints key=value lines to stderr (or $MEMSTAT_OUT).
#define _GNU_SOURCE
#include "chibicc.h"
#include <sys/mman.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);

int memstat_kind;  // MS_* currently being allocated (set around calloc calls)
int memstat_phase; // MP_* phase of the compiler
void *memstat_tmp;

static const char *kind_names[MS_NKINDS] = {
  "other", "token", "node", "type", "obj", "member",
};
static const char *phase_names[MP_NPHASES] = {
  "startup", "lex_pp", "parse", "codegen", "write",
};

typedef struct {
  uintptr_t ptr;   // 0 = empty
  uint32_t size;
  uint8_t kind;
  uint8_t phase;
  uint8_t mark;
} Entry;

#define TABLE_BITS 23
#define TABLE_CAP (1UL << TABLE_BITS)
#define TABLE_MASK (TABLE_CAP - 1)

static Entry *table;
static unsigned long live_entries;

static uint64_t bytes_alloc[MS_NKINDS][MP_NPHASES];
static uint64_t count_alloc[MS_NKINDS][MP_NPHASES];
static uint64_t bytes_freed;
static uint64_t live_bytes, peak_live_bytes;
static uint64_t untracked_frees;

static void die(const char *msg) {
  write(2, msg, strlen(msg));
  _exit(99);
}

static void init_table(void) {
  table = mmap(NULL, TABLE_CAP * sizeof(Entry), PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (table == MAP_FAILED)
    die("memstat: mmap failed\n");
}

static inline unsigned long hash_ptr(uintptr_t p) {
  return (unsigned long)((p >> 4) * 0x9E3779B97F4A7C15ULL >> (64 - TABLE_BITS));
}

static Entry *lookup(uintptr_t p) {
  if (!table || !p)
    return NULL;
  for (unsigned long i = hash_ptr(p);; i = (i + 1) & TABLE_MASK) {
    if (table[i].ptr == p)
      return &table[i];
    if (table[i].ptr == 0)
      return NULL;
  }
}

static void insert(uintptr_t p, size_t size, int kind, int phase) {
  if (!table)
    init_table();
  if (live_entries * 4 > TABLE_CAP * 3)
    die("memstat: table full\n");
  if (size > UINT32_MAX)
    die("memstat: allocation too large\n");
  unsigned long i = hash_ptr(p);
  while (table[i].ptr)
    i = (i + 1) & TABLE_MASK;
  table[i] = (Entry){p, (uint32_t)size, (uint8_t)kind, (uint8_t)phase, 0};
  live_entries++;
}

// Linear-probing delete with backward shift (no tombstones).
static void remove_entry(Entry *e) {
  unsigned long i = e - table;
  unsigned long j = i;
  for (;;) {
    j = (j + 1) & TABLE_MASK;
    if (table[j].ptr == 0)
      break;
    unsigned long k = hash_ptr(table[j].ptr);
    if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) {
      table[i] = table[j];
      i = j;
    }
  }
  table[i].ptr = 0;
  live_entries--;
}

static void account_new(void *p, size_t size) {
  if (!p)
    return;
  insert((uintptr_t)p, size, memstat_kind, memstat_phase);
  bytes_alloc[memstat_kind][memstat_phase] += size;
  count_alloc[memstat_kind][memstat_phase]++;
  live_bytes += size;
  if (live_bytes > peak_live_bytes)
    peak_live_bytes = live_bytes;
}

void *malloc(size_t size) {
  void *p = __libc_malloc(size);
  account_new(p, size);
  return p;
}

void *calloc(size_t n, size_t size) {
  void *p = __libc_calloc(n, size);
  account_new(p, n * size);
  return p;
}

void free(void *p) {
  Entry *e = lookup((uintptr_t)p);
  if (e) {
    live_bytes -= e->size;
    bytes_freed += e->size;
    remove_entry(e);
  } else if (p) {
    untracked_frees++;
  }
  __libc_free(p);
}

void *realloc(void *old, size_t size) {
  if (!old)
    return malloc(size);
  Entry *e = lookup((uintptr_t)old);
  void *p = __libc_realloc(old, size);
  if (!p)
    return p;
  if (!e) {
    account_new(p, size);
    return p;
  }
  size_t oldsize = e->size;
  int kind = e->kind, phase = e->phase;
  remove_entry(e);
  insert((uintptr_t)p, size, kind, phase);
  if (size > oldsize) {
    bytes_alloc[kind][memstat_phase] += size - oldsize;
    live_bytes += size - oldsize;
  } else {
    bytes_freed += oldsize - size;
    live_bytes -= oldsize - size;
  }
  if (live_bytes > peak_live_bytes)
    peak_live_bytes = live_bytes;
  return p;
}

void *reallocarray(void *old, size_t n, size_t size) {
  return realloc(old, n * size);
}

//
// Post-parse reachability walk.
//

static uint64_t marked_tokens, marked_tokens_origin;
static uint64_t visited_objs;

// Returns true the first time a heap object is seen. Non-heap objects
// (chibicc's static ty_int etc.) are never "first seen" via the table, so we
// visit them each time; they hold no pointers that lead back into the heap.
static bool first_visit(void *p) {
  Entry *e = lookup((uintptr_t)p);
  if (!e)
    return true;
  if (e->mark & 1)
    return false;
  e->mark |= 1;
  return true;
}

static void mark_token(Token *tok) {
  // mark bit 2: directly referenced; bit 4: reached via ->origin chain.
  bool direct = true;
  for (; tok; tok = tok->origin, direct = false) {
    Entry *e = lookup((uintptr_t)tok);
    if (!e || e->kind != MS_TOKEN)
      return;
    if (direct && !(e->mark & 2)) {
      e->mark |= 2;
      marked_tokens++;
    }
    if (e->mark & 4)
      return;
    e->mark |= 4;
    marked_tokens_origin++;
  }
}

static void walk_node(Node *node);
static void walk_type(Type *ty);
static void walk_obj(Obj *var);

static void walk_member(Member *m) {
  for (; m; m = m->next) {
    if (!first_visit(m))
      return;
    mark_token(m->tok);
    mark_token(m->name);
    walk_type(m->ty);
  }
}

static void walk_type(Type *ty) {
  for (; ty; ty = ty->next) {
    if (!first_visit(ty))
      return;
    mark_token(ty->name);
    mark_token(ty->name_pos);
    walk_type(ty->origin);
    walk_type(ty->base);
    walk_node(ty->vla_len);
    walk_obj(ty->vla_size);
    walk_member(ty->members);
    walk_type(ty->return_ty);
    walk_type(ty->params);
  }
}

static void walk_obj(Obj *var) {
  for (; var; var = var->next) {
    if (!first_visit(var))
      return;
    mark_token(var->tok);
    walk_type(var->ty);
    walk_obj(var->params);
    walk_node(var->body);
    walk_obj(var->locals);
    walk_obj(var->va_area);
    walk_obj(var->alloca_bottom);
    visited_objs++;
  }
}

static void walk_node(Node *node) {
  for (; node; node = node->next) {
    if (!first_visit(node))
      return;
    mark_token(node->tok);
    walk_type(node->ty);
    walk_type(node->func_ty);
    walk_node(node->lhs);
    walk_node(node->rhs);
    walk_node(node->cond);
    walk_node(node->then);
    walk_node(node->els);
    walk_node(node->init);
    walk_node(node->inc);
    walk_node(node->body);
    walk_member(node->member);
    walk_node(node->args);
    walk_obj(node->ret_buffer);
    walk_node(node->goto_next);
    walk_node(node->case_next);
    walk_node(node->default_case);
    walk_node(node->cas_addr);
    walk_node(node->cas_old);
    walk_node(node->cas_new);
    walk_obj(node->atomic_addr);
    walk_node(node->atomic_expr);
    walk_obj(node->var);
  }
}

static uint64_t live_token_bytes_at_walk;

void memstat_walk(Obj *prog) {
  walk_obj(prog);
  for (unsigned long i = 0; i < TABLE_CAP; i++)
    if (table[i].ptr && table[i].kind == MS_TOKEN)
      live_token_bytes_at_walk += table[i].size;
}

void memstat_report(size_t outbuf_len, void *outbuf) {
  FILE *f = stderr;
  char *path = getenv("MEMSTAT_OUT");
  memstat_kind = MS_OTHER;
  if (path && !(f = fopen(path, "w")))
    die("memstat: cannot open MEMSTAT_OUT\n");

  uint64_t total = 0, total_count = 0;
  uint64_t by_kind[MS_NKINDS] = {0}, by_phase[MP_NPHASES] = {0};
  for (int k = 0; k < MS_NKINDS; k++)
    for (int p = 0; p < MP_NPHASES; p++) {
      total += bytes_alloc[k][p];
      total_count += count_alloc[k][p];
      by_kind[k] += bytes_alloc[k][p];
      by_phase[p] += bytes_alloc[k][p];
    }

  Entry *ob = lookup((uintptr_t)outbuf);

  fprintf(f, "sizeof_token=%zu\nsizeof_node=%zu\nsizeof_type=%zu\nsizeof_obj=%zu\nsizeof_member=%zu\n",
          sizeof(Token), sizeof(Node), sizeof(Type), sizeof(Obj), sizeof(Member));
  fprintf(f, "total_bytes=%llu\ntotal_allocs=%llu\n",
          (unsigned long long)total, (unsigned long long)total_count);
  for (int k = 0; k < MS_NKINDS; k++) {
    uint64_t c = 0;
    for (int p = 0; p < MP_NPHASES; p++)
      c += count_alloc[k][p];
    fprintf(f, "kind_%s_bytes=%llu\nkind_%s_allocs=%llu\n", kind_names[k],
            (unsigned long long)by_kind[k], kind_names[k], (unsigned long long)c);
  }
  for (int p = 0; p < MP_NPHASES; p++)
    fprintf(f, "phase_%s_bytes=%llu\n", phase_names[p], (unsigned long long)by_phase[p]);
  for (int p = 0; p < MP_NPHASES; p++)
    fprintf(f, "phase_%s_token_bytes=%llu\n", phase_names[p],
            (unsigned long long)bytes_alloc[MS_TOKEN][p]);
  fprintf(f, "retained_tokens=%llu\nretained_token_bytes=%llu\n",
          (unsigned long long)marked_tokens,
          (unsigned long long)(marked_tokens * sizeof(Token)));
  fprintf(f, "retained_tokens_with_origin=%llu\nretained_token_bytes_with_origin=%llu\n",
          (unsigned long long)marked_tokens_origin,
          (unsigned long long)(marked_tokens_origin * sizeof(Token)));
  fprintf(f, "live_token_bytes_after_parse=%llu\n", (unsigned long long)live_token_bytes_at_walk);
  fprintf(f, "objs_walked=%llu\n", (unsigned long long)visited_objs);
  fprintf(f, "outbuf_len=%zu\noutbuf_alloc_bytes=%llu\n", outbuf_len,
          (unsigned long long)(ob ? ob->size : 0));
  fprintf(f, "bytes_freed=%llu\npeak_live_bytes=%llu\nuntracked_frees=%llu\n",
          (unsigned long long)bytes_freed, (unsigned long long)peak_live_bytes,
          (unsigned long long)untracked_frees);
  if (path)
    fclose(f);
}
