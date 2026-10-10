#include "admission.h"

void otr_admit_configuration(void *configuration)
{
    const char *name;
    if (!configuration || configuration == (void *)-1L ||
        OTCfigNumberOfChildren(configuration)) return;
    name = (const char *)OTCfigGetProviderNameD0(configuration);
    if (!name) return;
    if (name[0] == 't' && name[1] == 'c' && name[2] == 'p' && !name[3])
        OTCfigChangeProviderName(configuration, "auxtcp");
    else if (name[0] == 'u' && name[1] == 'd' && name[2] == 'p' && !name[3])
        OTCfigChangeProviderName(configuration, "auxudp");
}
