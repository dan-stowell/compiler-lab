// E12 driver for the C decoder (dec.c).
//   e12dec PAYLOAD dump        -> one line per instruction: "start skeleton v0 v1 ..."
//   e12dec PAYLOAD time REPS   -> sequential whole-payload decode, REPS times; median ns/instr
#include "dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CAP 200000
static Insn recs[CAP];

static double now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}

static int cmpd(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  FILE *f = fopen(argv[1], "rb");
  if (!f)
    return 2;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = calloc(1, sz + 16);
  if (fread(buf, 1, sz, f) != (size_t)sz)
    return 2;
  fclose(f);
  Decoder d;
  int e = dec_init(&d, buf);
  if (e) {
    fprintf(stderr, "dec_init failed %d\n", e);
    return 1;
  }
  if (!strcmp(argv[2], "dump")) {
    size_t n = dec_run(&d, 0, 0, recs, CAP);
    for (size_t i = 0; i < n; i++) {
      printf("%u %u", recs[i].start, recs[i].si);
      for (int j = 0; j < d.sk[recs[i].si].nk; j++)
        printf(" %lld", (long long)recs[i].v[j]);
      printf("\n");
    }
    return 0;
  }
  int reps = argc > 3 ? atoi(argv[3]) : 201;
  double *t = malloc(reps * sizeof *t);
  size_t n = 0;
  uint64_t chk = 0;
  for (int r = 0; r < reps; r++) {
    double t0 = now_ns();
    n = dec_run(&d, 0, 0, recs, CAP);
    t[r] = now_ns() - t0;
    chk += recs[n - 1].start + (uint64_t)recs[n / 2].v[0];
  }
  qsort(t, reps, sizeof *t, cmpd);
  printf("instructions=%zu reps=%d median_ns=%.0f min_ns=%.0f median_ns_per_instr=%.3f "
         "min_ns_per_instr=%.3f table_bytes=%zu payload_bytes=%ld chk=%llu\n",
         n, reps, t[reps / 2], t[0], t[reps / 2] / n, t[0] / n, d.table_bytes, sz,
         (unsigned long long)chk);
  return 0;
}
