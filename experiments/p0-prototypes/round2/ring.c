#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
static inline uint64_t sm(uint64_t x){x+=0x9e3779b97f4a7c15ULL;x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;x=(x^(x>>27))*0x94d049bb133111ebULL;return x^(x>>31);}
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
#define PER 680
int main(int argc,char**argv){
  int K=atoi(argv[1]); long chunks=atol(argv[2]);
  uint8_t*mem=mmap(0,(size_t)K*4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  uint64_t acc=0; double t0=now(); uint64_t idx=0;
  if(K>0){
   for(long c=0;c<chunks;c++){
    uint8_t*p=mem+(c%K)*4096; uint8_t*q=p;
    *q++=0x48;*q++=0x31;*q++=0xc0; // xor rax,rax
    for(int i=0;i<PER;i++){uint32_t im=(uint32_t)sm(idx++)&0x7fffffff;*q++=0x48;*q++=0x05;memcpy(q,&im,4);q+=4;}
    *q++=0xc3;
    acc+=((uint64_t(*)(void))p)();
   }
  } else {
   for(long c=0;c<chunks;c++){uint64_t s=0;for(int i=0;i<PER;i++){uint32_t im=(uint32_t)sm(idx++)&0x7fffffff;s+=(int64_t)(int32_t)im;}acc+=s;}
  }
  double t=now()-t0;
  printf("K=%d ns/generated-instr=%.3f acc=%lx\n",K,t*1e9/(chunks*PER),acc);
}
