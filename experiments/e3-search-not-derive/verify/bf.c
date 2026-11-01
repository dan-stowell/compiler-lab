// independent brute force: no dedup; DFS over all programs of length <= maxlen
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define M 64
static unsigned char tt[4096], samp_t[M]; static int sidx[M], maxlen; static long long hits, leaves;
static unsigned char st[20][20][M]; // st[level][depth][sample]
static int prog[20];
static int op(int o,int a,int b){switch(o){case 0:return (a+b)&15;case 1:return (a-b)&15;case 2:return (a*b)&15;case 3:return a&b;case 4:return a|b;default:return a^b;}}
static int full(int len){ for(int i=0;i<4096;i++){int x=i&15,y=(i>>4)&15,z=i>>8,s[20],sp=0;
  for(int k=0;k<len;k++){int in=prog[k];
   if(in<3)s[sp++]=in==0?x:in==1?y:z; else if(in<19)s[sp++]=in-3; else if(in<25){s[sp-2]=op(in-19,s[sp-2],s[sp-1]);sp--;}
   else if(in==25){s[sp]=s[sp-1];sp++;} else if(in==26){int t=s[sp-1];s[sp-1]=s[sp-2];s[sp-2]=t;} else {s[sp]=s[sp-2];sp++;}}
  if(sp!=1||s[0]!=tt[i])return 0;} return 1;}
static void dfs(int k,int d){
  if(d==1&&k>0&&!memcmp(st[k][0],samp_t,M)){ if(full(k)){hits++; printf("  FOUND len %d:",k); for(int i=0;i<k;i++)printf(" %d",prog[i]); printf("\n");} }
  if(k==maxlen) return;
  int r=maxlen-k-1;
  for(int in=0;in<28;in++){
    int nd = in<19||in==25||in==27 ? d+1 : in==26 ? d : d-1;
    if((in==25&&d<1)||((in>=19&&in!=25)&&d<2)) continue;
    if(nd<1||nd>r+1) continue;
    memcpy(st[k+1],st[k],sizeof(st[0][0])*d); prog[k]=in;
    for(int j=0;j<M;j++){int i=sidx[j],x=i&15,y=(i>>4)&15,z=i>>8; unsigned char *t=st[k+1][0];(void)t;
      if(in<3) st[k+1][d][j]=in==0?x:in==1?y:z; else if(in<19) st[k+1][d][j]=in-3;
      else if(in<25) st[k+1][d-2][j]=op(in-19,st[k][d-2][j],st[k][d-1][j]);
      else if(in==25) st[k+1][d][j]=st[k][d-1][j];
      else if(in==26){st[k+1][d-1][j]=st[k][d-2][j];st[k+1][d-2][j]=st[k][d-1][j];}
      else st[k+1][d][j]=st[k][d-2][j];}
    leaves++; dfs(k+1,nd);
  }}
int main(int c,char**v){ maxlen=atoi(v[1]); for(int i=0;i<4096;i++)tt[i]=v[2][i]-'a';
  srand(7); for(int j=0;j<M;j++){sidx[j]=rand()%4096; samp_t[j]=tt[sidx[j]];}
  dfs(0,0); printf("maxlen %d nodes %lld exact hits %lld\n",maxlen,leaves,hits); return 0;}
