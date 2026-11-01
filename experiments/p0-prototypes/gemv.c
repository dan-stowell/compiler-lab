#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define N 8192
static inline float rv(uint64_t i){ uint64_t z=i*0x9E3779B97F4A7C15ull; z^=z>>31; z*=0xBF58476D1CE4E5B9ull; z^=z>>29; return (float)(int32_t)(z>>32)*(1.0f/2147483648.0f);}
int main(int c,char**v){ int gen=atoi(v[1]); float*x=malloc(N*4),*y=calloc(N,4); for(int i=0;i<N;i++)x[i]=rv(1u<<40|i);
 float*R=0; if(!gen){R=malloc((size_t)N*N*4); for(size_t i=0;i<(size_t)N*N;i++)R[i]=rv(i);}
 /* measured region approx: whole program; R fill counted separately via gen=2 */
 if(gen==2){printf("%f\n",R?R[5]:0);return 0;}
 for(size_t i=0;i<N;i++){float s=0; for(size_t j=0;j<N;j++) s+= (gen? rv(i*N+j): R[i*N+j])*x[j]; y[i]=s;}
 double t=0; for(int i=0;i<N;i++)t+=y[i]; printf("%f\n",t); }
