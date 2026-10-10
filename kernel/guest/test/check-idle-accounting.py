#!/usr/bin/env python3
import pathlib,struct,subprocess,sys,tempfile
objdump,nm,ld=sys.argv[1:4]; work=pathlib.Path(sys.argv[4])
def run(*a):return subprocess.check_output(a,text=True,stderr=subprocess.STDOUT)
def symbols(path):
 return {a[2]:int(a[0],16) for l in run(nm,str(path)).splitlines() if len(a:=l.split())==3 and a[1] in "Tt"}
def memory(path):
 b=pathlib.Path(path).read_bytes(); assert b[:6]==b"\x7fELF\x01\x02"
 o=struct.unpack_from(">I",b,32)[0]; n,size=struct.unpack_from(">HH",b,46)
 secs=[struct.unpack_from(">10I",b,o+j*n) for j in range(size)]
 def read(addr,count):
  for s in secs:
   if s[1]==1 and s[3]<=addr and addr+count<=s[3]+s[5]:return b[s[4]+addr-s[3]:s[4]+addr-s[3]+count]
  raise AssertionError("unmapped code")
 return read
cases=0
for path in sys.argv[5:]:
 s=symbols(path); rd=memory(path); clock=s["clock"]; wait=s["waitloc"]
 assert rd(wait-4,6)==bytes.fromhex("4e7220004e75")
 if struct.unpack_from(">H",pathlib.Path(path).read_bytes(),16)[0]==1:
  assert rd(clock+0xb2,10)==bytes.fromhex("0cab0000000000426600")
  assert "%x: R_68K_32\twaitloc"%(clock+0xb4) in run(objdump,"-dr",str(path))
 else:assert rd(clock+0xb2,10)==bytes.fromhex("0cab")+struct.pack(">I",wait)+bytes.fromhex("00426600")
 assert rd(clock+0xbe,2)==bytes.fromhex("43f9")
 assert rd(clock+0x114,2)==bytes.fromhex("52b9") and rd(clock+0x11a,4)==bytes.fromhex("52aa004c")
 # The native clock branches on supervisor SR and the saved STOP return PC.
 def charge(sr,pc,waiting):
  if not sr&0x2000:return "user"
  if pc!=wait:return "system"
  return "wait" if waiting else "idle"
 assert charge(0x2000,wait,0)=="idle";assert charge(0x2000,wait,1)=="wait"
 assert charge(0x2000,s["idle"]+4,0)=="system" if s["idle"]+4!=wait else True
 assert charge(0,wait,0)=="user";assert charge(0x2000,clock,0)=="system"
 cases+=5;print(path, "waitloc=%x idle=%x"%(wait,s["idle"]))
for name in ("runtime040","mainmarks","diagvec"):
 o=work/(name+".o"); dis=run(objdump,"-dr",str(o))
 assert "R_68K_32\twaitloc-0x4" in dis
 with tempfile.TemporaryDirectory() as d:
  out=pathlib.Path(d)/"linked"
  run(ld,"--unresolved-symbols=ignore-all","--defsym=waitloc=0x101a0","-e","idle","-Ttext=0x300000","-o",str(out),str(o))
  rd=memory(out); ds=run(objdump,"-d",str(out));assert "1019c" in ds
  # The relocated transfer lands on the native STOP, not a new STOP address.
  assert ("4ef9 0001 019c" in ds) if name!="diagvec" else ("4eb9 0001 019c" in ds)
 cases+=2
print("idle-accounting: %d binary accounting/link cases PASS"%cases)
