#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "config.h"
static int checks;
#define CHECK(x) do {assert(x);checks++;}while(0)
int main(void){unsigned char a[4],saved[4];char name[16];struct otb_identity i;
 CHECK(otb_ipv4("10.0.2.15",a)&&a[0]==10&&a[3]==15);memcpy(saved,a,4);
 CHECK(!otb_ipv4("256.0.2.1",a));CHECK(!otb_ipv4("1.2.3",a));CHECK(!otb_ipv4("1.2.3.4extra",a));CHECK(!memcmp(saved,a,4));
 CHECK(otb_route("Routing tables\nDestination Gateway Flags Refcnt Use Interface\ndefault 10.0.2.2 UG 0 0 aen0\n",a,name)&&a[3]==2&&!strcmp(name,"aen0"));
 CHECK(!otb_route("default 10.0.2.2 U 0 0 aen0\n",a,name));
 CHECK(!otb_route("default 10.0.2.2 UG 0 0 bad/name\n",a,name));
 CHECK(!otb_route("default 10.0.2.2 UG 0 0 aen0\ndefault 10.0.2.3 UG 0 0 aen0\n",a,name));
 CHECK(!otb_route("10.0.2.0 10.0.2.15 U 0 0 aen0\n",a,name));
 CHECK(otb_route("0.0.0.0 10.0.2.2 UG 0 0 aen0",a,name));
 memset(&i,0,sizeof i);CHECK(otb_resolver("# local\nnameserver 10.0.2.3\nnameserver 1.1.1.1\nsearch example.test other.test\n",&i));CHECK(i.dns[3]==3&&i.dns[7]==1&&!strcmp((char*)i.domain,"example.test"));
 CHECK(otb_resolver("nameserver 1.1.1.1\nnameserver 2.2.2.2\nnameserver 3.3.3.3\n",&i)&&i.dns[11]==3);
 CHECK(!otb_resolver("nameserver 1.1.1.1\nnameserver 2.2.2.2\nnameserver 3.3.3.3\nnameserver 4.4.4.4\n",&i));
 CHECK(!otb_resolver("nameserver invalid\n",&i));CHECK(!otb_resolver("domain\n",&i));CHECK(otb_resolver("",&i)&&!i.dns[0]&&!i.domain[0]);
 printf("PASS %d target configuration parser checks\n",checks);return 0;}
