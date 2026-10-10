#!/usr/bin/env python3
"""Verify carrier resources and malformed-map refusal."""
import pathlib,sys,struct
root=pathlib.Path(__file__).resolve().parents[3];sys.path.insert(0,str(root/'tools'))
import rsrcedit
from rsrc import entries,merge
carrier=pathlib.Path(sys.argv[1]).read_bytes();source=pathlib.Path(sys.argv[2]);original=source.read_bytes()
owned={(b'CODE',0),(b'CODE',1)}
def inventory(data):return {(t,i):(a,n,p) for t,i,a,n,p,r in entries(data)}
a=inventory(original);b=inventory(carrier)
assert all(b[k]==v for k,v in a.items() if k not in owned)
assert source.read_bytes()==original
print('carrier: original resources preserved',len(a))
base=bytearray(carrier)
for typ in {k[0] for k in b} - {b'CODE',b'SIZE'}:rsrcedit.delete(base,typ)
synth=bytearray(base);rsrcedit.add(synth,b'TEST',128,b'payload',b'named');rsrcedit.add(synth,b'TEST',129,b'compressed',b'zero')
r=next(r for t,i,a,n,p,r in entries(synth) if (t,i)==(b'TEST',129));synth[r+4]=1
_,f,_=rsrcedit.fork(synth);mo=struct.unpack_from('>I',synth,f+4)[0];nl=struct.unpack_from('>H',synth,f+mo+26)[0];no=struct.unpack_from('>H',synth,r+2)[0];synth[f+mo+nl+no]=0
merged=merge(base,synth)
assert inventory(merged)==inventory(synth)
_,f,_=rsrcedit.fork(synth);mo=struct.unpack_from('>I',synth,f+4)[0];m=f+mo;tl=struct.unpack_from('>H',synth,m+24)[0]
for offset,fmt,value in [(f+12,'>I',27),(m+tl,'>H',256),(r+4,'>I',0x00ffffff)]:
    bad=bytearray(synth);struct.pack_into(fmt,bad,offset,value)
    try:merge(base,bad)
    except ValueError:pass
    else:raise AssertionError('malformed resource accepted')
print('carrier: synthetic attributes/names and3 malformed maps PASS')
