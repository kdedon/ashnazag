#!/usr/bin/env python3
"""Check that compiled native pointer imports consume D0."""
from pathlib import Path
import re,subprocess,sys
text=subprocess.check_output([sys.argv[1],'-dr',sys.argv[2]],text=True)
names=['OTAllocMem_D0','allocb_D0','getq_D0','RD_D0','WR_D0','mi_timer_alloc_D0',
       'OTCfigGetProviderNameD0','OTNewConfiguratorD0','OTCreateStateMachineD0']
for name in names:
 found=re.search(r'R_68K_32\s+'+name+r'\n\s*[0-9a-f]+:\s+2040\s+moveal %d0,%a0',text)
 assert found,name
print('PASS 9 compiled native pointer imports consume D0 before returning A0')
