#define main k5_main
#include "k5.c"
#undef main
/* own materialization: explicit-stack DFS over the generator, then qsort + count via binary search */
static u64 *buf; static u64 cnt;
static void walk(const col *c, node x) {
  node st[256]; int sp = 0; st[sp++] = x;
  while (sp) { node y = st[--sp];
    if (y.n == 0) continue;
    if (y.b - y.a == 1) { for (u64 i = 0; i < y.n; i++) buf[cnt++] = (u64)y.a; continue; }
    if (y.n <= LEAF) { cnt += leaf_keys(c, y, buf + cnt); continue; }
    node L, R; split(c, y, &L, &R); st[sp++] = L; st[sp++] = R; }
}
static int cmp(const void *a, const void *b) { u64 x = *(u64*)a, y = *(u64*)b; return x < y ? -1 : x > y; }
static u64 lb(u64 *v, u64 n, u64 k) { u64 lo = 0, hi = n; while (lo < hi) { u64 m = (lo+hi)/2; if (v[m] < k) lo = m+1; else hi = m; } return lo; }
int main(int argc, char **argv) {
  u64 N = strtoull(argv[1],0,0); u128 W = W_of(argv[2]);
  col A = {0xA11CE ^ N}, B = {0xB0B ^ N}; node r = {0, W, N};
  u64 *a = malloc(N*8), *b = malloc(N*8);
  buf = a; cnt = 0; walk(&A, r); u64 na = cnt; buf = b; cnt = 0; walk(&B, r); u64 nb = cnt;
  qsort(a, na, 8, cmp); qsort(b, nb, 8, cmp);
  u128 s = 0; for (u64 i = 0; i < na; ) { u64 j = i; while (j < na && a[j] == a[i]) j++;
    u64 k = lb(b, nb, a[i]); u64 m = k; while (m < nb && b[m] == a[i]) m++; s += (u128)(j-i)*(m-k); i = j; }
  u64 dupA = 0; for (u64 i = 1; i < na; i++) dupA += a[i]==a[i-1];
  printf("mine N=%llu na=%llu nb=%llu count=%llu dupA=%llu expected~N^2/W=%.0f\n", (unsigned long long)N,
    (unsigned long long)na,(unsigned long long)nb,(unsigned long long)s,(unsigned long long)dupA,(double)N*N/(double)W);
}
