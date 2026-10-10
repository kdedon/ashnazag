#include "config.h"
#include <string.h>
static int space(c) int c;
{ return c==' ' || c=='\t' || c=='\r'; }
static char *word(p,out,cap) char *p,*out;int cap;
{
 int n=0;while(space(*p))p++;while(*p && *p!='\n' && !space(*p)){
  if(n+1>=cap)return 0;out[n++]=*p++;
 }
 out[n]=0;return p;
}
int otb_ipv4(s,p) char *s;unsigned char *p;
{
 unsigned char b[4];int k,n,d;
 for(k=0;k<4;k++){
  n=d=0;while(*s>='0'&&*s<='9'){n=n*10+*s++-'0';if(++d>3||n>255)return 0;}
  if(!d)return 0;b[k]=n;if(k<3){if(*s++!='.')return 0;}else if(*s)return 0;
 }
 memcpy(p,b,4);return 1;
}
int otb_route(text,gateway,name) char *text;unsigned char *gateway;char *name;
{
 char a[64],b[64],c[64],last[64];char *p,*end;int found=0,n;
 while(*text){
  end=strchr(text,'\n');if(!end)end=text+strlen(text);
  p=word(text,a,sizeof a);if(!p)return 0;
  if(!strcmp(a,"default")||!strcmp(a,"0.0.0.0")){
   if(found || !(p=word(p,b,sizeof b)) || !otb_ipv4(b,gateway))return 0;
   if(!(p=word(p,c,sizeof c)) || !strchr(c,'U') || !strchr(c,'G'))return 0;
   last[0]=0;while(p<end){p=word(p,c,sizeof c);if(!p)return 0;if(!c[0])break;strcpy(last,c);}
   n=strlen(last);if(!n||n>=16)return 0;
   for(n=0;last[n];n++)if(!((last[n]>='a'&&last[n]<='z')||(last[n]>='A'&&last[n]<='Z')||(last[n]>='0'&&last[n]<='9')||last[n]=='_'))return 0;
   strcpy(name,last);found=1;
  }
  text=*end?end+1:end;
 }
 return found;
}
int otb_resolver(text,i) char *text;struct otb_identity *i;
{
 char key[64],value[128];char *p,*end;int n=0,j;
 memset(i->dns,0,sizeof i->dns);memset(i->domain,0,sizeof i->domain);
 while(*text){
  end=strchr(text,'\n');if(!end)end=text+strlen(text);
  p=word(text,key,sizeof key);if(!p)return 0;
  if(key[0]!='#' && key[0]!=';' && key[0]){
   if(!strcmp(key,"nameserver")){
    if(!(p=word(p,value,sizeof value)) || n==3 || !otb_ipv4(value,i->dns+4*n))return 0;n++;
   }else if(!strcmp(key,"domain")||!strcmp(key,"search")){
    if(!(p=word(p,value,sizeof value)) || !value[0] || strlen(value)>=sizeof i->domain)return 0;
    for(j=0;value[j];j++)if((unsigned char)value[j]<33||(unsigned char)value[j]>126)return 0;
    strcpy((char *)i->domain,value);
   }
  }
  text=*end?end+1:end;
 }
 return 1;
}
