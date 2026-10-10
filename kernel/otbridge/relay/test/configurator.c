#include "../configurator.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct cfig { const char *name; unsigned short children; };
static int checks, factories, states, opens, returns, failfactory, failstate;
static long status, register_error;
static int registrations;
long otr_admission_register(void) {++registrations;return register_error;}
static long (*configure_fn)(void *, void *);
static long (*stream_fn)(void *, void *, unsigned long, void *, void *);
static const char *opened;
static void *notifier, *context;
static unsigned long flags;
#define CHECK(x) do { assert(x); ++checks; } while (0)
unsigned short OTCfigNumberOfChildren(void *p) { return ((struct cfig *)p)->children; }
unsigned long OTCfigGetProviderNameD0(void *p) { return (unsigned long)((struct cfig *)p)->name; }
unsigned long OTNewConfiguratorD0(void *data,long (*c)(void *,void *),
    long (*s)(void *,void *,unsigned long,void *,void *),void *event)
{
    CHECK(!data && !event);factories++;configure_fn=c;stream_fn=s;
    return failfactory ? 0 : 1;
}
unsigned long OTCreateStateMachineD0(void *buffer,unsigned long size,unsigned long extra,void *notify,void *ctx)
{
    CHECK(buffer && size==112 && extra==0);
    states++;notifier=notify;context=ctx;return failstate ? 0 : 2;
}
unsigned char OTSMOpenStream(void *state,const char *name,unsigned long f)
{
    CHECK(state==(void *)2);opens++;opened=name;flags=f;return 0;
}
long OTSMReturnToCaller(void *state)
{
    CHECK(state==(void *)2);returns++;return status;
}
int main(void)
{
    otr_can_configure can=0;otr_create_configurator create=0;
    unsigned char kind=255;void *obj=0;int i;
    struct cfig cfg={"auxtcp",0};
    const char *bad[]={"ddp","rawip","auxtcpX","auxudpX","AUXTCP","","aux","auxtc","auxud",0};
    CHECK(OTSetupConfigurator(&can,&create,&kind)==0&&can&&create&&kind==1);
    CHECK(!factories&&!states&&!opens&&!returns && registrations==1);
    register_error=-14;can=0;create=0;kind=255;
    CHECK(OTSetupConfigurator(&can,&create,&kind)==-3221 && !can && !create && kind==255);
    register_error=0;CHECK(!OTSetupConfigurator(&can,&create,&kind));
    CHECK(OTSetupConfigurator(0,&create,&kind)==-3221);
    CHECK(OTSetupConfigurator(&can,0,&kind)==-3221);
    CHECK(OTSetupConfigurator(&can,&create,0)==-3221);
    CHECK(!can(0,0));CHECK(!can(&cfg,1));CHECK(!can(&cfg,2));
    for(i=0;i<10;i++){cfg.name=bad[i];CHECK(!can(&cfg,0));}
    cfg.name="auxtcp";CHECK(can(&cfg,0));cfg.children=1;CHECK(!can(&cfg,0));
    cfg.children=2;CHECK(!can(&cfg,0));cfg.children=0;cfg.name="auxudp";CHECK(can(&cfg,0));
    CHECK(create(0)==-3221);failfactory=1;CHECK(create(&obj)==-3211&&!obj);
    failfactory=0;CHECK(create(&obj)==0&&obj==(void *)1&&configure_fn&&stream_fn);
    CHECK(configure_fn(obj,&cfg)==0);cfg.name="ddp";
    CHECK(configure_fn(obj,&cfg)==-3221);
    CHECK(stream_fn(obj,&cfg,7,(void *)3,(void *)4)==-3221&&!states);
    cfg.name="auxtcp";failstate=1;
    CHECK(stream_fn(obj,&cfg,7,(void *)3,(void *)4)==-3211&&states==1&&!opens&&!returns);
    failstate=0;status=-3201;
    CHECK(stream_fn(obj,&cfg,7,(void *)3,(void *)4)==-3201);
    CHECK(opens==1&&returns==1&&flags==7&&notifier==(void *)3&&context==(void *)4&&!strcmp(opened,"auxtcp"));
#ifdef OTR_DIAGNOSTICS
    {extern unsigned long otr_config_diagnostics[16];CHECK(otr_config_diagnostics[7]==4);}
#endif
    cfg.name="auxudp";status=0;
    CHECK(stream_fn(obj,&cfg,0,0,0)==0);
    CHECK(opens==2&&returns==2&&flags==0&&!notifier&&!context&&!strcmp(opened,"auxudp"));
    cfg.name="tcp";CHECK(can(&cfg,0));CHECK(configure_fn(obj,&cfg)==0);
    CHECK(stream_fn(obj,&cfg,7,(void *)3,(void *)4)==0);
    CHECK(!strcmp(opened,"auxtcp")&&flags==7&&notifier==(void *)3&&context==(void *)4);
    cfg.name="udp";CHECK(can(&cfg,0));CHECK(configure_fn(obj,&cfg)==0);
    CHECK(stream_fn(obj,&cfg,0,0,0)==0);CHECK(!strcmp(opened,"auxudp"));
    cfg.children=1;CHECK(!can(&cfg,0));CHECK(configure_fn(obj,&cfg)==-3221);
    CHECK(stream_fn(obj,&cfg,0,0,0)==-3221);cfg.children=0;
    cfg.name="tcpX";CHECK(!can(&cfg,0));cfg.name="udpX";CHECK(!can(&cfg,0));
    cfg.name="udp";CHECK(stream_fn(obj,&cfg,0,0,0)==0);
#ifdef OTR_DIAGNOSTICS
    {extern unsigned long otr_config_diagnostics[16];
     CHECK(otr_config_diagnostics[0]==0x4f544447UL && otr_config_diagnostics[1]==1 && otr_config_diagnostics[2]==3);
     CHECK(otr_config_diagnostics[3]>0 && otr_config_diagnostics[4]>0 && otr_config_diagnostics[9]>0);
     CHECK(otr_config_diagnostics[7]==0);
     CHECK(otr_config_diagnostics[13]>0 && otr_config_diagnostics[14]==3 && otr_config_diagnostics[15]==0);}
#endif
    printf("PASS %d production configurator selection/factory/state/notification checks\n",checks);
    return 0;
}
