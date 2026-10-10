"""Copy original application resources into its native carrier."""
import struct
import rsrcedit

def entries(data):
    _,base,size=rsrcedit.fork(data)
    if base+size>len(data) or size<16:raise ValueError('short resource fork')
    do,mo,dl,ml=struct.unpack_from('>IIII',data,base)
    if do+dl>size or mo+ml>size or ml<28:raise ValueError('invalid resource bounds')
    m=base+mo;tl,nl=struct.unpack_from('>HH',data,m+24)
    if tl+2>ml or nl>ml:raise ValueError('invalid resource map')
    count=struct.unpack_from('>H',data,m+tl)[0]
    if count==65535:return
    if count>255 or tl+2+8*(count+1)>ml:raise ValueError('invalid type count')
    seen=set()
    for i in range(count+1):
        typ,n,ro=struct.unpack_from('>4sHH',data,m+tl+2+8*i)
        if n>511 or tl+ro+12*(n+1)>ml:raise ValueError('invalid reference list')
        for j in range(n+1):
            r=m+tl+ro+12*j;rid,no,packed=struct.unpack_from('>hHI',data,r)
            key=(typ,rid)
            if key in seen:raise ValueError('duplicate resource')
            seen.add(key);offset=packed&0xffffff;attrs=packed>>24
            if offset+4>dl:raise ValueError('invalid resource offset')
            size=struct.unpack_from('>I',data,base+do+offset)[0]
            if offset+4+size>dl:raise ValueError('short resource')
            name=None
            if no!=65535:
                if nl+no>=ml:raise ValueError('invalid resource name')
                a=m+nl+no;length=data[a]
                if nl+no+1+length>ml:raise ValueError('short resource name')
                name=bytes(data[a+1:a+1+length])
            yield typ,rid,attrs,name,bytes(data[base+do+offset+4:base+do+offset+4+size]),r

def merge(carrier,source):
    data=bytearray(carrier)
    owned={(b'CODE',0),(b'CODE',1)}
    if any(typ==b'SIZE' for typ,*_ in entries(source)):
        rsrcedit.delete(data,b'SIZE')
    for typ,rid,attrs,name,payload,_ in entries(source):
        if (typ,rid) in owned:continue
        rsrcedit.add(data,typ,rid,payload,b"\0" if name==b"" else name)
        ref=next(r for t,i,a,n,p,r in entries(data) if (t,i)==(typ,rid))
        data[ref+4]=attrs
        if name==b"":
            _,base,_=rsrcedit.fork(data);mo=struct.unpack_from(">I",data,base+4)[0]
            nl=struct.unpack_from(">H",data,base+mo+26)[0]
            no=struct.unpack_from(">H",data,ref+2)[0]
            data[base+mo+nl+no]=0
    return data
