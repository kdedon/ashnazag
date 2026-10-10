#ifndef AUX_OT_CONFIGURATOR_H
#define AUX_OT_CONFIGURATOR_H
typedef unsigned char (*otr_can_configure)(void *, unsigned long);
typedef long (*otr_create_configurator)(void **);
extern unsigned short OTCfigNumberOfChildren(void *);
extern unsigned long OTCfigGetProviderNameD0(void *);
extern unsigned long OTNewConfiguratorD0(void *, long (*)(void *, void *),
    long (*)(void *, void *, unsigned long, void *, void *), void *);
extern unsigned long OTCreateStateMachineD0(void *, unsigned long, unsigned long, void *, void *);
extern unsigned char OTSMOpenStream(void *, const char *, unsigned long);
extern long OTSMReturnToCaller(void *);
extern long OTSetupConfigurator(otr_can_configure *, otr_create_configurator *, unsigned char *);
#endif
