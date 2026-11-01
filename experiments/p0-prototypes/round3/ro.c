// run-once vs interpreter on a once-executed random op stream (opcode = D known at gen, regs = S runtime)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <x86intrin.h>
#define NOPS (1<<24)
typedef struct { uint8_t op,d,a,b; } Op;
static uint64_t sm(uint64_t *s){uint64_t z=(*s+=0x9e3779b97f4a7c15ULL);z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;z=(z^(z>>27))*0x94d049bb133111ebULL;return z^(z>>31);}
static uint32_t interp(Op*p,long n,uint32_t*r){
  static void*L[6]={&&ADD,&&SUB,&&XOR,&&AND,&&OR,&&MUL};
  Op*e=p+n;
#define NX if(p==e) goto END; goto *L[p->op];
  NX
ADD: r[p->d]=r[p->a]+r[p->b];p++;NX
SUB: r[p->d]=r[p->a]-r[p->b];p++;NX
XOR: r[p->d]=r[p->a]^r[p->b];p++;NX
AND: r[p->d]=r[p->a]&(r[p->b]|1);p++;NX
OR:  r[p->d]=r[p->a]|r[p->b];p++;NX
MUL: r[p->d]=r[p->a]*(r[p->b]|1);p++;NX
END: return r[0];
}
// branch-free "execute every op kind and select" baseline
static uint32_t pred(Op*p,long n,uint32_t*r){
  for(long i=0;i<n;i++){uint32_t x=r[p[i].a],y=r[p[i].b];uint32_t v[6]={x+y,x-y,x^y,x&(y|1),x|y,x*(y|1)};r[p[i].d]=v[p[i].op];}
  return r[0];
}
// x86: mov eax,[rdi+a]; OP eax,[rdi+b]; mov [rdi+d],eax
static const uint8_t opc[6][2]={{0x03,0},{0x2B,0},{0x33,0},{0x23,0},{0x0B,0},{0x0F,0xAF}};
static uint8_t *ring;
typedef struct{uint8_t b[16];uint8_t pb,pd,len;}T;
static const T tm[6]={
 {{0x8B,0x47,0,0x03,0x47,0,0x89,0x47,0},5,8,9},
 {{0x8B,0x47,0,0x2B,0x47,0,0x89,0x47,0},5,8,9},
 {{0x8B,0x47,0,0x33,0x47,0,0x89,0x47,0},5,8,9},
 {{0x8B,0x47,0,0x8B,0x4F,0,0x83,0xC9,1,0x21,0xC8,0x89,0x47,0},5,13,14},
 {{0x8B,0x47,0,0x0B,0x47,0,0x89,0x47,0},5,8,9},
 {{0x8B,0x47,0,0x8B,0x4F,0,0x83,0xC9,1,0x0F,0xAF,0xC1,0x89,0x47,0},5,14,15}};
static uint32_t jit(Op*p,long n,uint32_t*r,int chunk,int exec){
  for(long base=0;base<n;base+=chunk){
    uint8_t*c=ring; long m=n-base<chunk?n-base:chunk;
    for(long i=0;i<m;i++){Op o=p[base+i];
      const T*t=&tm[o.op]; memcpy(c,t->b,16); c[2]=o.a*4; c[t->pb]=o.b*4; c[t->pd]=o.d*4; c+=t->len;}
    *c++=0xC3;
    if(exec)((void(*)(uint32_t*))ring)(r);
  }
  return r[0];
}
int main(int argc,char**argv){
  int nk=atoi(argv[1]); // opcode kinds used randomly (1 = predictable)
  int chunk=4096;
  ring=mmap(0,1<<20,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  Op*p=malloc(sizeof(Op)*NOPS);uint64_t s=1;
  for(long i=0;i<NOPS;i++){uint64_t z=sm(&s);p[i]=(Op){z%nk,(z>>8)&15,(z>>16)&15,(z>>24)&15};}
  uint32_t r[16];uint64_t t;
  for(int k=0;k<2;k++){
  for(int i=0;i<16;i++)r[i]=i*7+1; t=__rdtsc(); uint32_t a=interp(p,NOPS,r); double ti=(__rdtsc()-t)/(double)NOPS;
  for(int i=0;i<16;i++)r[i]=i*7+1; t=__rdtsc(); uint32_t b=pred(p,NOPS,r); double tp=(__rdtsc()-t)/(double)NOPS;
  for(int i=0;i<16;i++)r[i]=i*7+1; t=__rdtsc(); uint32_t c=jit(p,NOPS,r,chunk,1); double tj=(__rdtsc()-t)/(double)NOPS;
  t=__rdtsc(); jit(p,NOPS,r,chunk,0); double tg=(__rdtsc()-t)/(double)NOPS;
  if(k)printf("kinds=%d interp %.2f pred %.2f jit %.2f (gen %.2f) tsc-cyc/op  agree=%d\n",nk,ti,tp,tj,tg,a==b&&b==c);}
}
