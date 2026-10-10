#include "configurator.h"
extern long otr_admission_register(void);
#ifdef OTR_TRACE_LOAD
extern long otr_sys_write(int, const unsigned char *, unsigned long);
#define trace(text) ((void)otr_sys_write(1, (const unsigned char *)(text), sizeof(text) - 1))
#else
#define trace(text) ((void)0)
#endif

#ifdef OTR_DIAGNOSTICS
unsigned long otr_config_diagnostics[16] = { 0x4f544447UL, 1, 3 };
#define CSET(n,v) (otr_config_diagnostics[n] = (unsigned long)(v))
#define CINC(n) (++otr_config_diagnostics[n])
#define OTSetupConfigurator otr_setup_configurator
#else
#define CSET(n,v) ((void)0)
#define CINC(n) ((void)0)
#endif
static int own_name(void *configuration)
{
    const char *name;
    int i;
    static const char *names[] = { "auxtcp", "auxudp", "tcp", "udp" };
    if (!configuration) return 0;
    { unsigned long children = OTCfigNumberOfChildren(configuration);
      CSET(6,children);if (children) return 0; }
    name = (const char *)OTCfigGetProviderNameD0(configuration);
    if (!name) return 0;
    for (i = 0; i < 4; i++) {
        int n;
        for (n = 0; n < 7 && name[n] == names[i][n]; n++)
            if (!name[n]) return (i & 1) + 1;
    }
    return 0;
}
static unsigned char can_configure(void *configuration, unsigned long pass)
{
    CINC(4);CSET(5,pass);CSET(8,0);
    if (pass != 0 || !own_name(configuration)) return 0;
    CSET(8,1);
    trace("otrelay I configurator accepted leaf\n");
    return 1;
}
static long configure(void *self, void *configuration)
{
    (void)self;CINC(11);
    {long result=own_name(configuration) ? 0 : -3221L;CSET(12,result);return result;}
}
static long create_stream(void *self, void *configuration, unsigned long flags,
    void *notify, void *context)
{
    union { unsigned long alignment; unsigned char bytes[112]; } work;
    void *state;
    int protocol;
    (void)self;CINC(13);CSET(14,1);CSET(7,context);
    protocol = own_name(configuration);
    if (!protocol) {CSET(15,-3221L);return -3221L;}
    trace("otrelay I configurator create stream\n");
    state = (void *)OTCreateStateMachineD0(work.bytes, sizeof work.bytes, 0, notify, context);
    if (!state) {CSET(15,-3211L);return -3211L;}
    CSET(14,2);
    OTSMOpenStream(state, protocol == 1 ? "auxtcp" : "auxudp", flags);
    trace("otrelay I configurator open dispatched\n");
    {long result=OTSMReturnToCaller(state);CSET(14,3);CSET(15,result);return result;}
}
static long create_configurator(void **result)
{
    CINC(9);
    if (!result) {CSET(10,-3221L);return -3221L;}
    trace("otrelay I configurator factory\n");
    *result = (void *)OTNewConfiguratorD0(0, configure, create_stream, 0);
    {long status=*result ? 0 : -3211L;CSET(10,status);return status;}
}
long OTSetupConfigurator(otr_can_configure *can, otr_create_configurator *create,
    unsigned char *kind)
{
    CINC(3);
    if (!can || !create || !kind) return -3221L;
    trace("otrelay I configurator setup\n");
    {long status=otr_admission_register();
        if(status) {CSET(15,status);trace("otrelay I admission registration failed\n");return -3221L;}
    }
    *can = can_configure;
    *create = create_configurator;
    *kind = 1;
    return 0;
}
