#!/usr/bin/env python3
"""Verify public configurator imports against local shipping resources."""
import importlib.util,struct,sys
from pathlib import Path
sp=importlib.util.spec_from_file_location('install',Path(__file__).with_name('install-evidence.py'))
install=importlib.util.module_from_spec(sp);sp.loader.exec_module(install)

def check(init,jumps,code,at,class_id,expected):
 data=install.decode(init,at);pos=data.index(class_id.encode()+b'\0')
 table=(pos+len(class_id)+2)&~1
 for name,(ordinal,wanted) in expected.items():
  pointer=struct.unpack_from('>I',data,table+4*ordinal)[0]
  segment,trap,entry=struct.unpack_from('>HHI',jumps,pointer-18)
  assert segment==2 and trap==0xa9f0 and entry==wanted
  for o in range(entry,min(entry+1200,len(code)-4)):
   if code[o:o+2]!=b'\x4e\x75' or not code[o+2]&128:continue
   size=code[o+2]&127;text=code[o+3:o+3+size]
   if not all(32<=x<127 for x in text):continue
   assert text.decode()==name,(name,text);break
  else:raise AssertionError(name)
  print('PASS',class_id,name,'ordinal',ordinal,'entry',hex(entry))

def main():
 root=Path(sys.argv[1])
 check(*[(root/f'cd15-{i}.bin').read_bytes() for i in [1,0,2]],0x48a,'OTClnt$NtvUtils',{
  'OTCfigNumberOfChildren':(34,0x11c2), 'OTCfigGetProviderName':(39,0x1290),
  'OTNewConfigurator':(45,0xc68)})
 check(*[(root/f'cd16-{i}.bin').read_bytes() for i in [1,0,2]],0x4d0,'OTClnt$NtvClient',{
  'OTCreateStateMachine':(74,0x3c82), 'OTSMOpenStream':(52,0x43e4),
  'OTSMReturnToCaller':(55,0x4160)})
 code=(root/'cd16-2.bin').read_bytes()
 for site,target in [(0x1ac8,0x3c82),(0x1b06,0x43e4),(0x1b2a,0x4160)]:
  assert struct.unpack_from('>H',code,site)[0]==0x4eb9
  assert struct.unpack_from('>I',code,site+2)[0]+40==target
 print('PASS shipped standard leaf uses create-state/open-stream/return-to-caller sequence')
 utils=(root/'cd15-2.bin').read_bytes()
 assert struct.unpack_from('>3H',utils,0x7da)==(0x4aaa,44,0x660c)
 assert struct.unpack_from('>3H',utils,0x7e2)==(0x4aaa,44,0x6704)
 print('PASS default configurator leaf/wrapper port predicates')
if __name__=='__main__':main()
