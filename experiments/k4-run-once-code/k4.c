// K4 (R1): run-once generated code on a data-dependent program.
//
// DFA: Aho-Corasick automaton (unanchored search, ".*(w1|w2|...)") over a fixed
// keyword list cut so that |Q| = 256 exactly. Output: final state and the number
// of input positions after which the DFA is in an accepting state.
// State numbering: accepting states are 0..A-1, so "accepting" is s < A.
// Table layout is transposed, T[b*256 + s], so the input byte selects a column.
//
// Modes (one per run; the measured region is bracketed by perf --control):
//   table   tight table-driven loop (baseline, the strongest)
//   interp  switch interpreter over per-chunk bytecode (STEP b, ACC) -- the same
//           specialized program, interpreted instead of emitted as x86
//   jit     per chunk of n input bytes, emit straight-line x86 into a RWX ring
//           (ring R bytes, split into R/slot slots), call it once, move on and
//           overwrite. Per input byte: movzx eax,byte[rax+T+256*b]; cmp al,A; adc ecx,0
//   gen     the jit generator alone (emits into the ring, never executes)
//   control fixed splitmix chain (noise control; input not built)
//   selftest check the automaton against a naive matcher on a prefix
//
// usage: k4 MODE INPUT [ring_bytes] [chunk_bytes] [input_MiB]
//   INPUT = seeded | text:FILELIST (files concatenated, repeated to size)
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define NQ 256
static uint8_t *T;          // transposed table, mapped below 2 GiB (disp32)
static int A;               // number of accepting states (0..A-1)
static int S0;              // start state (root)
static const char *words[NQ];
static int nwords;

static inline uint64_t sm(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL; x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL; return x ^ (x >> 31);
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

// Candidate words: C and Lua keywords, then common identifiers in both code bases.
static const char *cand[] = {
  "auto","break","case","char","const","continue","default","do","double","else","enum",
  "extern","float","for","goto","if","int","long","register","return","short","signed",
  "sizeof","static","struct","switch","typedef","union","unsigned","void","volatile","while",
  "and","elseif","end","false","function","in","local","nil","not","or","repeat","then",
  "true","until","lua_State","luaL_","TValue","setobj","Token","Node","Type","malloc",
  "include","define","NULL","uint8_t","size_t","printf","error","assert","strcmp",
  "memcpy","calloc","index","value","table","string","number","integer","buffer",
  "pointer","struct Node","next","count","length","offset","result","object","closure",
  NULL};

static int build_dfa(void) {
  static int go[NQ][256], fail[NQ], term[NQ], acc[NQ], d[NQ][256], ren[NQ], q[NQ];
  memset(go, -1, sizeof go);
  int n = 1;
  for (int w = 0; cand[w] && n < NQ; w++) {
    const char *s = cand[w]; int need = 0, u = 0, k = 0, len = strlen(s);
    for (k = 0; k < len; k++) { if (go[u][(uint8_t)s[k]] < 0) break; u = go[u][(uint8_t)s[k]]; }
    need = len - k;
    int take = len;
    if (n + need > NQ) take = k + (NQ - n);  // last word cut so |Q| = 256 exactly
    u = 0;
    for (int i = 0; i < take; i++) {
      int c = (uint8_t)s[i];
      if (go[u][c] < 0) go[u][c] = n++;
      u = go[u][c];
    }
    term[u] = 1;
    char *cp = strndup(s, take); words[nwords++] = cp;
  }
  if (n != NQ) { fprintf(stderr, "only %d states\n", n); exit(1); }
  // BFS: fail links, full transition function, accept = output nonempty
  int h = 0, t = 0;
  for (int c = 0; c < 256; c++) {
    if (go[0][c] >= 0) { fail[go[0][c]] = 0; q[t++] = go[0][c]; d[0][c] = go[0][c]; }
    else d[0][c] = 0;
  }
  acc[0] = 0;
  while (h < t) {
    int u = q[h++];
    acc[u] = term[u] || acc[fail[u]];
    for (int c = 0; c < 256; c++) {
      int v = go[u][c];
      if (v >= 0) { fail[v] = d[fail[u]][c]; d[u][c] = v; q[t++] = v; }
      else d[u][c] = d[fail[u]][c];
    }
  }
  A = 0; for (int s = 0; s < NQ; s++) if (acc[s]) ren[s] = A++;
  int j = A; for (int s = 0; s < NQ; s++) if (!acc[s]) ren[s] = j++;
  T = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  if (T == MAP_FAILED || (uintptr_t)T + 65536 > 0x7fffffff) { perror("mmap T"); exit(1); }
  for (int s = 0; s < NQ; s++) for (int c = 0; c < 256; c++) T[c * 256 + ren[s]] = ren[d[s][c]];
  S0 = ren[0];
  return n;
}

// ---- input ----
static uint8_t *IN; static size_t N;
static void build_input(const char *spec, size_t mib) {
  N = mib << 20;
  IN = mmap(0, N, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (IN == MAP_FAILED) { perror("mmap IN"); exit(1); }
  madvise(IN, N, MADV_HUGEPAGE);
  if (!strcmp(spec, "seeded")) {
    // text-like: keywords (35%), random lowercase words, separators; all from splitmix(seed=1)
    uint64_t ctr = 1; size_t i = 0;
    static const char sep[] = "  \n\t(){};,.=*+-_";
    while (i < N) {
      uint64_t r = sm(ctr++);
      const char *w; char tmp[16]; int len;
      if ((r & 255) < 90) { w = words[(r >> 8) % nwords]; len = strlen(w); }
      else { len = 1 + (r >> 8) % 9; for (int k = 0; k < len; k++) tmp[k] = 'a' + ((r >> (16 + 4 * k)) % 26); w = tmp; }
      for (int k = 0; k < len && i < N; k++) IN[i++] = w[k];
      if (i < N) IN[i++] = sep[(r >> 56) % (sizeof sep - 1)];
    }
  } else if (!strncmp(spec, "text:", 5)) {
    FILE *fl = fopen(spec + 5, "r"); if (!fl) { perror(spec + 5); exit(1); }
    char path[4096]; size_t m = 0;
    while (m < N && fgets(path, sizeof path, fl)) {
      path[strcspn(path, "\n")] = 0; if (!*path) continue;
      FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
      m += fread(IN + m, 1, N - m, f); fclose(f);
    }
    fclose(fl);
    if (m == 0) { fprintf(stderr, "empty text\n"); exit(1); }
    for (size_t i = m; i < N; i++) IN[i] = IN[i - m];  // repeat
  } else { fprintf(stderr, "bad input %s\n", spec); exit(1); }
}

// ---- perf control (perf stat --control fifo:CTL,ACK -D -1) ----
static int ctl = -1, ack = -1;
static void perf_cmd(const char *c) {
  if (ctl < 0) return;
  if (write(ctl, c, strlen(c)) < 0) perror("ctl");
  char b[16]; if (read(ack, b, sizeof b) < 0) perror("ack");
}

// ---- baselines ----
static uint64_t run_table(uint32_t *st) {
  const uint8_t *p = IN, *e = IN + N; uint32_t s = *st; uint64_t cnt = 0; const uint32_t a = A;
  for (; p + 8 <= e; p += 8) {
#define STEP(k) s = T[(uint32_t)p[k] << 8 | s]; cnt += s < a;
    STEP(0) STEP(1) STEP(2) STEP(3) STEP(4) STEP(5) STEP(6) STEP(7)
  }
  for (; p < e; p++) { s = T[(uint32_t)*p << 8 | s]; cnt += s < a; }
  *st = s; return cnt;
}

enum { OP_STEP, OP_ACC, OP_END };
static uint64_t run_interp(uint32_t *st, size_t n, uint64_t *ops) {
  uint8_t *bc = malloc(3 * n + 1); uint32_t s = *st; uint64_t cnt = 0, nops = 0; const uint32_t a = A;
  for (size_t off = 0; off < N; off += n) {
    size_t m = N - off < n ? N - off : n; uint8_t *q = bc;
    for (size_t i = 0; i < m; i++) { *q++ = OP_STEP; *q++ = IN[off + i]; *q++ = OP_ACC; }
    *q++ = OP_END; nops += 2 * m + 1;
    for (const uint8_t *pc = bc;;) {
      switch (*pc) {
        case OP_STEP: s = T[(uint32_t)pc[1] << 8 | s]; pc += 2; continue;
        case OP_ACC: cnt += s < a; pc += 1; continue;
        default: goto done;
      }
    }
  done:;
  }
  free(bc); *st = s; *ops = nops; return cnt;
}

// ---- run-once code ring ----
// chunk: 89 f8 mov eax,edi | 31 c9 xor ecx,ecx | per byte: 0f b6 80 d32 | 3c A | 83 d1 00 |
//        48 c1 e1 20 shl rcx,32 | 48 09 c8 or rax,rcx | c3 ret
#define PER_BYTE 12
#define CHUNK_OVH (2 + 2 + 4 + 3 + 1)
static uint8_t pat[256][16];
static void build_pat(void) {
  for (int b = 0; b < 256; b++) {
    uint8_t *q = pat[b]; uint32_t d = (uint32_t)(uintptr_t)T + 256u * b;
    q[0] = 0x0f; q[1] = 0xb6; q[2] = 0x80; memcpy(q + 3, &d, 4);
    q[7] = 0x3c; q[8] = (uint8_t)A; q[9] = 0x83; q[10] = 0xd1; q[11] = 0x00;
  }
}
static inline uint8_t *emit_chunk(uint8_t *q, const uint8_t *in, size_t m) {
  *q++ = 0x89; *q++ = 0xf8; *q++ = 0x31; *q++ = 0xc9;
  for (size_t i = 0; i < m; i++) { memcpy(q, pat[in[i]], 16); q += PER_BYTE; }
  static const uint8_t tail[8] = {0x48, 0xc1, 0xe1, 0x20, 0x48, 0x09, 0xc8, 0xc3};
  memcpy(q, tail, 8); return q + 8;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: k4 MODE INPUT [ring] [chunk] [MiB]\n"); return 2; }
  const char *mode = argv[1], *inp = argv[2];
  size_t ring = argc > 3 ? strtoul(argv[3], 0, 0) : 65536;
  size_t chunk = argc > 4 ? strtoul(argv[4], 0, 0) : 0;
  size_t mib = argc > 5 ? strtoul(argv[5], 0, 0) : 1024;
  if (getenv("K4_CTL")) { ctl = open(getenv("K4_CTL"), O_WRONLY); ack = open(getenv("K4_ACK"), O_RDONLY); }
  int nq = build_dfa();

  if (!strcmp(mode, "control")) {
    uint64_t x = 1; perf_cmd("enable\n"); double t0 = now();
    for (long i = 0; i < (1L << 28); i++) x = sm(x);
    double t = now() - t0; perf_cmd("disable\n");
    printf("mode=control ns=%.0f x=%lx\n", t * 1e9, x); return 0;
  }
  build_input(inp, mib);

  if (!strcmp(mode, "selftest")) {
    size_t L = N < (1 << 20) ? N : (1 << 20); uint64_t ref = 0;
    for (size_t i = 0; i < L; i++) {
      int hit = 0;
      for (int w = 0; w < nwords && !hit; w++) { size_t l = strlen(words[w]); hit = l <= i + 1 && !memcmp(IN + i + 1 - l, words[w], l); }
      ref += hit;
    }
    size_t saveN = N; N = L; uint32_t s = S0; uint64_t c = run_table(&s); N = saveN;
    printf("selftest prefix=%zu naive=%lu table=%lu %s |Q|=%d A=%d words=%d\n", L, ref, c, ref == c ? "OK" : "FAIL", nq, A, nwords);
    for (int w = 0; w < nwords; w++) printf("%s%s", w ? "|" : "patterns: ", words[w]);
    printf("\n"); return ref != c;
  }

  // largest chunk that fits the ring (one slot) unless given
  size_t maxchunk = (ring - CHUNK_OVH - 4) / PER_BYTE;
  if (!chunk) chunk = maxchunk;
  if (chunk > maxchunk) { fprintf(stderr, "chunk %zu > max %zu for ring %zu\n", chunk, maxchunk, ring); return 2; }
  size_t slot = (CHUNK_OVH + 4 + PER_BYTE * chunk + 63) & ~(size_t)63;
  size_t nslots = ring / slot;
  uint8_t *R = mmap(0, ring, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (R == MAP_FAILED) { perror("mmap ring"); return 1; }
  build_pat();
  memset(R, 0xc3, ring);

  uint32_t s = S0; uint64_t cnt = 0, ginstr = 0, gbytes = 0, nchunks = 0;
  perf_cmd("enable\n"); double t0 = now();
  if (!strcmp(mode, "table")) cnt = run_table(&s);
  else if (!strcmp(mode, "interp")) cnt = run_interp(&s, chunk, &ginstr);
  else if (!strcmp(mode, "jit") || !strcmp(mode, "gen")) {
    int exec = !strcmp(mode, "jit"); size_t k = 0;
    for (size_t off = 0; off < N; off += chunk, nchunks++) {
      size_t m = N - off < chunk ? N - off : chunk;
      uint8_t *p = R + k * slot; if (++k == nslots) k = 0;
      uint8_t *e = emit_chunk(p, IN + off, m);
      gbytes += e - p; ginstr += 3 * m + 5;
      if (exec) { uint64_t r = ((uint64_t (*)(uint32_t))p)(s); s = (uint32_t)r; cnt += r >> 32; }
    }
    if (!exec) { s = R[(N * 7) % ring]; }  // keep the stores observable
  } else { fprintf(stderr, "bad mode\n"); return 2; }
  double t = now() - t0; perf_cmd("disable\n");
  printf("mode=%s input=%s ring=%zu chunk=%zu slots=%zu N=%zu state=%u matches=%lu ns=%.0f ns_per_byte=%.4f "
         "gen_instr=%lu gen_bytes=%lu chunks=%lu |Q|=%d A=%d\n",
         mode, inp, ring, chunk, nslots, N, s, cnt, t * 1e9, t * 1e9 / N, ginstr, gbytes, nchunks, nq, A);
  return 0;
}
