#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define N (1<<17)
#define W (N/64)
#define K 32
#define NI (1<<16)
#define WI (NI/64)
static uint64_t s=12345; static uint64_t rnd(){s+=0x9e3779b97f4a7c15ULL;uint64_t z=s;z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;z=(z^(z>>27))*0x94d049bb133111ebULL;return z^(z>>31);}
static uint64_t A[K][W],B[K][W],C[K][W],I[K][WI];
static void fill(uint64_t*x,int words,int nbits,int cnt){memset(x,0,words*8);int c=0;while(c<cnt){uint64_t r=rnd()%nbits;if(!(x[r/64]>>(r%64)&1)){x[r/64]|=1ULL<<(r%64);c++;}}}
static long pc(uint64_t*a,uint64_t*b,int w){long t=0;for(int k=0;k<w;k++)t+=__builtin_popcountll(a[k]&b[k]);return t;}
int main(int argc,char**argv){int reps=argc>1?atoi(argv[1]):1;
 for(int i=0;i<K;i++){fill(A[i],W,N,4096);fill(B[i],W,N,4096);fill(C[i],W,N,4096);fill(I[i],WI,NI,2048);}
 double tot=0;static double cnt[NI];
 for(int r=0;r<reps;r++){
 static uint64_t mask[W];for(int k=0;k<W;k++)mask[k]=(k<W/2)?~0ULL:0; // op1 select a<N/2
 double a1[K];for(int i=0;i<K;i++)a1[i]=pc(A[i],mask,W);
 static double RR[K][K],RP[K][K];
 for(int i=0;i<K;i++)for(int j=0;j<K;j++){RR[i][j]=pc(B[i],A[j],W);RP[i][j]=pc(B[i],C[j],W);} // op2 compose R.R, op4 compose with P
 double wk[K];for(int k=0;k<K;k++){double t=0;for(int i=0;i<K;i++){double v=RP[i][k]; // op3 bag-union: paths len1 + len2
   for(int j=0;j<K;j++)v+=RR[i][j]*RP[j][k]; t+=a1[i]*v;} wk[k]=t;}
 memset(cnt,0,sizeof cnt);for(int k=0;k<K;k++)for(int w=0;w<WI;w++){uint64_t x=I[k][w];while(x){int b=__builtin_ctzll(x);cnt[w*64+b]+=wk[k];x&=x-1;}} // op5 per-item aggregate
 int best=0;for(int t=0;t<NI;t++)if(cnt[t]>cnt[best])best=t; tot+=cnt[best]; } // op6 top-1
 printf("%g\n",tot);}
