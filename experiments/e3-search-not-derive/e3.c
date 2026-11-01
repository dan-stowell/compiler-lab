// E3: shortest stack-VM program by behaviour-deduplicated BFS vs postfix compiler output.
// Usage: e3 list            -> print all expressions: idx \t infix \t derived_len \t derived_prog
//        e3 solve IDX [CAP] -> search for expression IDX (CAP = CPU seconds, default 600), print one TSV line
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

#define N 4096
#define NEXPR 200
#define SEED 0xE3E3E3E3ULL

/* ---------- instruction set ---------- */
enum { I_X = 0, I_Y = 1, I_Z = 2, I_C0 = 3 /* ..18 */, I_ADD = 19, I_SUB, I_MUL, I_AND, I_OR, I_XOR,
       I_DUP, I_SWAP, I_OVER, NINSTR };
static const char *opname[] = {"ADD", "SUB", "MUL", "AND", "OR", "XOR"};
static const char opch[] = {'+', '-', '*', '&', '|', '^'};

static void instr_str(int in, char *buf) {
    if (in == I_X) strcpy(buf, "PUSHX");
    else if (in == I_Y) strcpy(buf, "PUSHY");
    else if (in == I_Z) strcpy(buf, "PUSHZ");
    else if (in < I_ADD) sprintf(buf, "PUSHC %d", in - I_C0);
    else if (in <= I_XOR) strcpy(buf, opname[in - I_ADD]);
    else if (in == I_DUP) strcpy(buf, "DUP");
    else if (in == I_SWAP) strcpy(buf, "SWAP");
    else strcpy(buf, "OVER");
}
static void prog_str(const uint8_t *p, int len, char *out) {
    out[0] = 0;
    char b[16];
    for (int i = 0; i < len; i++) { instr_str(p[i], b); if (i) strcat(out, "; "); strcat(out, b); }
}
static inline int binop(int op, int a, int b) { // a = second, b = top
    switch (op) {
    case 0: return (a + b) & 15; case 1: return (a - b) & 15; case 2: return (a * b) & 15;
    case 3: return a & b; case 4: return a | b; default: return a ^ b;
    }
}
// Reference scalar VM, independent of the vectorised search code. Returns -1 if invalid.
static int run_scalar(const uint8_t *p, int len, int x, int y, int z) {
    int st[64], sp = 0;
    for (int i = 0; i < len; i++) {
        int in = p[i];
        if (in <= I_Z || in < I_ADD) { if (sp >= 64) return -1; st[sp++] = in == I_X ? x : in == I_Y ? y : in == I_Z ? z : in - I_C0; }
        else if (in <= I_XOR) { if (sp < 2) return -1; st[sp - 2] = binop(in - I_ADD, st[sp - 2], st[sp - 1]); sp--; }
        else if (in == I_DUP) { if (sp < 1) return -1; st[sp] = st[sp - 1]; sp++; }
        else if (in == I_SWAP) { if (sp < 2) return -1; int t = st[sp - 1]; st[sp - 1] = st[sp - 2]; st[sp - 2] = t; }
        else { if (sp < 2) return -1; st[sp] = st[sp - 2]; sp++; }
    }
    return sp == 1 ? st[0] : -1;
}

/* ---------- expressions ---------- */
typedef struct Node { int kind; /* 0 x,1 y,2 z,3 const,4 op */ int val; struct Node *l, *r; } Node;
static uint64_t rng;
static uint64_t next64(void) { // splitmix64
    uint64_t z = (rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static Node *gen(int nops) {
    Node *n = calloc(1, sizeof *n);
    if (nops == 0) { n->kind = next64() % 4; if (n->kind == 3) n->val = next64() % 16; return n; }
    n->kind = 4; n->val = next64() % 6;
    int k = next64() % nops;
    n->l = gen(k); n->r = gen(nops - 1 - k);
    return n;
}
static int eval(Node *n, int x, int y, int z) {
    switch (n->kind) { case 0: return x; case 1: return y; case 2: return z; case 3: return n->val; }
    return binop(n->val, eval(n->l, x, y, z), eval(n->r, x, y, z));
}
static void infix(Node *n, char *out) {
    char a[256], b[256];
    switch (n->kind) {
    case 0: strcpy(out, "x"); return; case 1: strcpy(out, "y"); return; case 2: strcpy(out, "z"); return;
    case 3: sprintf(out, "%d", n->val); return;
    }
    infix(n->l, a); infix(n->r, b); sprintf(out, "(%s %c %s)", a, opch[n->val], b);
}
// The straightforward tree-walking (postfix) compiler.
static void derive(Node *n, uint8_t *p, int *len) {
    if (n->kind < 3) { p[(*len)++] = I_X + n->kind; return; }
    if (n->kind == 3) { p[(*len)++] = I_C0 + n->val; return; }
    derive(n->l, p, len); derive(n->r, p, len); p[(*len)++] = I_ADD + n->val;
}
static Node *exprs[NEXPR];
static void make_exprs(void) {
    rng = SEED;
    for (int i = 0; i < NEXPR; i++) exprs[i] = gen(1 + next64() % 4);
}
static int verify(const uint8_t *p, int len, Node *e) {
    for (int i = 0; i < N; i++) {
        int x = i & 15, y = (i >> 4) & 15, z = i >> 8;
        if (run_scalar(p, len, x, y, z) != eval(e, x, y, z)) return 0;
    }
    return 1;
}

/* ---------- hashing ---------- */
typedef struct { uint64_t a, b; } H128;
static inline uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 33)) * 0xff51afd7ed558ccdULL; z = (z ^ (z >> 33)) * 0xc4ceb9fe1a85ec53ULL;
    return z ^ (z >> 33);
}
static H128 hash_vec(const uint8_t *v) {
    const uint64_t *w = (const uint64_t *)v;
    uint64_t a0 = 0x243F6A8885A308D3ULL, a1 = 0x13198A2E03707344ULL, b0 = 0xA4093822299F31D0ULL, b1 = 0x082EFA98EC4E6C89ULL;
    for (int i = 0; i < N / 8; i += 2) {
        a0 = (a0 ^ w[i]) * 0x9E3779B97F4A7C15ULL; a1 = (a1 ^ w[i + 1]) * 0xC2B2AE3D27D4EB4FULL;
        b0 = (b0 + w[i]) * 0x165667B19E3779F9ULL; b1 = (b1 + w[i + 1]) * 0x27D4EB2F165667C5ULL;
        a0 ^= a0 >> 29; a1 ^= a1 >> 31; b0 ^= b0 >> 27; b1 ^= b1 >> 30;
    }
    H128 h = {mix64(a0 ^ mix64(a1)), mix64(b0 + mix64(b1 ^ 0x5555))};
    return h;
}
static inline H128 hash_stack(const H128 *vh, int d) {
    uint64_t a = 0x9E3779B97F4A7C15ULL * (d + 1), b = 0x6A09E667F3BCC909ULL ^ d;
    for (int i = 0; i < d; i++) { a = mix64(a ^ vh[i].a); b = mix64(b + vh[i].b + 0x3C6EF372FE94F82BULL); }
    H128 h = {a, b};
    return h;
}

/* ---------- seen set (open addressing, 128-bit keys) ---------- */
static H128 *tab; static size_t tcap, tcount;
static int seen_insert(H128 h) { // returns 1 if newly inserted
    if (h.a == 0 && h.b == 0) h.a = 1;
    if (2 * (tcount + 1) > tcap) {
        size_t ocap = tcap; H128 *ot = tab;
        tcap = ocap ? ocap * 2 : (1 << 20);
        tab = calloc(tcap, sizeof(H128));
        if (!tab) { fprintf(stderr, "oom\n"); exit(2); }
        for (size_t i = 0; i < ocap; i++) if (ot[i].a | ot[i].b) {
            size_t j = ot[i].a & (tcap - 1);
            while (tab[j].a | tab[j].b) j = (j + 1) & (tcap - 1);
            tab[j] = ot[i];
        }
        free(ot);
    }
    size_t j = h.a & (tcap - 1);
    while (tab[j].a | tab[j].b) {
        if (tab[j].a == h.a && tab[j].b == h.b) return 0;
        j = (j + 1) & (tcap - 1);
    }
    tab[j] = h; tcount++;
    return 1;
}

/* ---------- frontier: programs packed 5 bits/instr ---------- */
typedef struct { uint64_t *v; size_t n, cap; } Vec;
static void push(Vec *f, uint64_t x) {
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 1024; f->v = realloc(f->v, f->cap * 8); if (!f->v) { fprintf(stderr, "oom\n"); exit(2); } }
    f->v[f->n++] = x;
}

static uint8_t base[3 + 16][N];
static void vec_binop(int op, const uint8_t *a, const uint8_t *b, uint8_t *o) {
    switch (op) {
    case 0: for (int i = 0; i < N; i++) o[i] = (a[i] + b[i]) & 15; break;
    case 1: for (int i = 0; i < N; i++) o[i] = (a[i] - b[i]) & 15; break;
    case 2: for (int i = 0; i < N; i++) o[i] = (a[i] * b[i]) & 15; break;
    case 3: for (int i = 0; i < N; i++) o[i] = a[i] & b[i]; break;
    case 4: for (int i = 0; i < N; i++) o[i] = a[i] | b[i]; break;
    default: for (int i = 0; i < N; i++) o[i] = a[i] ^ b[i]; break;
    }
}

static double cpu_now(void) { return (double)clock() / CLOCKS_PER_SEC; }

int main(int argc, char **argv) {
    make_exprs();
    if (argc >= 2 && !strcmp(argv[1], "list")) {
        for (int i = 0; i < NEXPR; i++) {
            char s[512], ps[1024]; uint8_t p[32]; int len = 0;
            infix(exprs[i], s); derive(exprs[i], p, &len); prog_str(p, len, ps);
            printf("%d\t%s\t%d\t%s\n", i, s, len, ps);
        }
        return 0;
    }
    if (argc < 3 || strcmp(argv[1], "solve")) { fprintf(stderr, "usage: e3 list | e3 solve IDX [CAP_SECONDS]\n"); return 1; }
    int idx = atoi(argv[2]);
    double cap = argc > 3 ? atof(argv[3]) : 600.0;
    Node *e = exprs[idx];
    char es[512]; infix(e, es);
    uint8_t dp[32]; int dlen = 0; derive(e, dp, &dlen);
    if (!verify(dp, dlen, e)) { fprintf(stderr, "derived program failed verification\n"); return 3; }

    for (int i = 0; i < N; i++) { base[0][i] = i & 15; base[1][i] = (i >> 4) & 15; base[2][i] = i >> 8; }
    for (int k = 0; k < 16; k++) memset(base[3 + k], k, N);
    H128 bh[19]; for (int k = 0; k < 19; k++) bh[k] = hash_vec(base[k]);

    static uint8_t tv[N];
    for (int i = 0; i < N; i++) tv[i] = eval(e, i & 15, (i >> 4) & 15, i >> 8);
    H128 th = hash_vec(tv);

    // Search lengths 1..dlen-1. If nothing shorter exists, the shortest is dlen (the derived program is a verified witness).
    int maxlen = dlen - 1;
    double t0 = cpu_now();
    uint64_t explored = 0; // distinct (deduped) states stored for expansion + children examined at last layer counted separately
    uint64_t children = 0;
    int found_len = -1; uint8_t found[32]; int timeout = 0;
    Vec cur = {0}, nxt = {0};
    push(&cur, 0); // empty program
    static uint8_t stk[16][N]; static uint8_t tmp[N];
    H128 vh[16], cvh[16];
    seen_insert(hash_stack(vh, 0));
    for (int k = 0; k < maxlen && found_len < 0 && !timeout; k++) { // expand programs of length k into length k+1
        int r = maxlen - (k + 1); // remaining budget after child
        nxt.n = 0;
        for (size_t fi = 0; fi < cur.n && found_len < 0; fi++) {
            if ((fi & 1023) == 0 && cpu_now() - t0 > cap) { timeout = 1; break; }
            uint64_t code = cur.v[fi];
            uint8_t p[16]; for (int i = 0; i < k; i++) p[i] = (code >> (5 * i)) & 31;
            // replay
            int d = 0;
            for (int i = 0; i < k; i++) {
                int in = p[i];
                if (in < I_ADD) { memcpy(stk[d], base[in], N); vh[d] = bh[in]; d++; }
                else if (in <= I_XOR) { vec_binop(in - I_ADD, stk[d - 2], stk[d - 1], tmp); memcpy(stk[d - 2], tmp, N); d--; vh[d - 1] = hash_vec(stk[d - 1]); }
                else if (in == I_DUP) { memcpy(stk[d], stk[d - 1], N); vh[d] = vh[d - 1]; d++; }
                else if (in == I_SWAP) { memcpy(tmp, stk[d - 1], N); memcpy(stk[d - 1], stk[d - 2], N); memcpy(stk[d - 2], tmp, N); H128 t = vh[d - 1]; vh[d - 1] = vh[d - 2]; vh[d - 2] = t; }
                else { memcpy(stk[d], stk[d - 2], N); vh[d] = vh[d - 2]; d++; }
            }
            for (int in = 0; in < NINSTR; in++) {
                int nd;
                if (in < I_ADD || in == I_DUP) nd = d + 1;
                else if (in == I_OVER) nd = d + 1;
                else if (in == I_SWAP) nd = d;
                else nd = d - 1;
                if (in == I_DUP && d < 1) continue;
                if ((in >= I_ADD && in <= I_XOR) || in == I_SWAP || in == I_OVER) if (d < 2) continue;
                if (nd < 1 || nd > r + 1) continue; // can't reduce to one value in remaining budget
                children++;
                memcpy(cvh, vh, sizeof(H128) * d);
                if (in < I_ADD) cvh[d] = bh[in];
                else if (in <= I_XOR) { vec_binop(in - I_ADD, stk[d - 2], stk[d - 1], tmp); cvh[d - 2] = hash_vec(tmp); }
                else if (in == I_DUP) cvh[d] = vh[d - 1];
                else if (in == I_SWAP) { cvh[d - 1] = vh[d - 2]; cvh[d - 2] = vh[d - 1]; }
                else cvh[d] = vh[d - 2];
                if (nd == 1 && cvh[0].a == th.a && cvh[0].b == th.b) {
                    memcpy(found, p, k); found[k] = in;
                    if (verify(found, k + 1, e)) { found_len = k + 1; break; }
                }
                if (r == 0) continue;
                if (seen_insert(hash_stack(cvh, nd))) { explored++; push(&nxt, code | ((uint64_t)in << (5 * k))); }
            }
        }
        Vec t = cur; cur = nxt; nxt = t;
    }
    double cpu = cpu_now() - t0;
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    int slen = found_len > 0 ? found_len : (timeout ? -1 : dlen);
    char dps[1024], fps[1024];
    prog_str(dp, dlen, dps);
    if (found_len > 0) prog_str(found, found_len, fps); else if (!timeout) strcpy(fps, dps); else strcpy(fps, "TIMEOUT");
    // idx expr derived_len search_len status cpu_s rss_kb states children derived_prog found_prog
    printf("%d\t%s\t%d\t%d\t%s\t%.3f\t%ld\t%llu\t%llu\t%s\t%s\n", idx, es, dlen, slen,
           timeout ? "timeout" : found_len > 0 ? "found" : "derived_optimal", cpu, ru.ru_maxrss,
           (unsigned long long)explored, (unsigned long long)children, dps, fps);
    return 0;
}
