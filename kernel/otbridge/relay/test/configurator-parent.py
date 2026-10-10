#!/usr/bin/env python3
"""Check generated parent interfaces and malformed parent specifications."""
from pathlib import Path
import subprocess,sys,tempfile
builder,obj=sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='cfig-parent-') as directory:
 output=str(Path(directory)/'library.bin')
 args=[builder,'-n','OTLib$hostcfig','-v','0x100','-i','1','-c','aX02','-G','0x224',
       '-I','cfig_utils','-I','cfig_client','-e','OTClnt$auxCfig,0x100,0x100,OTSetupConfigurator']
 subprocess.run(args+['-p','OTClnt$auxCfig,OTClnt$cfigMkr','-o',output,obj],check=True,stdout=subprocess.DEVNULL)
 description=subprocess.check_output([builder,'-d',output],text=True)
 assert 'fset  OTClnt$auxCfig 0100/0100 : OTClnt$cfigMkr' in description
 assert 'flags 0840 0224' in description
 for mode in ['-t','-l']:subprocess.run([builder,mode,output],check=True,stdout=subprocess.DEVNULL)
 for specs in [['x,OTClnt$cfigMkr'],['OTClnt$auxCfig,OTClnt$auxCfig'],
               ['OTClnt$auxCfig,'],[',OTClnt$cfigMkr'],['OTClnt$auxCfig,x,y'],
               ['OTClnt$auxCfig,'+'x'*256],['OTClnt$auxCfig,x','OTClnt$auxCfig,y']]:
  parent=[item for spec in specs for item in ['-p',spec]]
  result=subprocess.run(args+parent+['-o',output,obj],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
  assert result.returncode!=0,specs
 print('PASS 10 generated configurator parent, package and malformed-input cases')
