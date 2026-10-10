"""Install a native resource façade while preserving the original COFF data."""
import pathlib,struct,sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[3]/'tools'))
from rsrc import entries
import rsrcedit

def facade(original,carrier):
    if original[:8]!=bytes.fromhex('0005160700010000'):
        raise ValueError('expected AppleDouble resources')
    resources=list(entries(carrier));owned={(b'CODE',0),(b'CODE',1)}
    source={(t,i):(a,n,p) for t,i,a,n,p,r in entries(original)}
    target={(t,i):(a,n,p) for t,i,a,n,p,r in resources}
    if not all(target.get(k)==v for k,v in source.items()):
        raise ValueError('original resources changed')
    if any(k not in source and k not in owned for k in target):
        raise ValueError('unexpected façade resources')
    if not owned.issubset(target):raise ValueError('missing native code')
    _,start,size=rsrcedit.fork(original)
    _,newstart,newsize=rsrcedit.fork(carrier)
    result=bytearray(original[:start]+carrier[newstart:newstart+newsize]+original[start+size:])
    delta=newsize-size;info=None
    tableend=26+12*struct.unpack_from('>H',original,24)[0]
    for j in range(struct.unpack_from('>H',original,24)[0]):
        at=26+12*j;kind,offset,length=struct.unpack_from('>III',original,at)
        if offset<tableend or offset+length>len(original):raise ValueError('invalid entry bounds')
        if kind==2:
            if (offset,length)!=(start,size):raise ValueError('resource descriptor mismatch')
            struct.pack_into('>I',result,at+8,newsize)
        else:
            if offset<start+size and offset+length>start:raise ValueError('overlapping entries')
            if offset>=start+size:
                offset+=delta;struct.pack_into('>I',result,at+4,offset)
            if kind==9:info=(offset,length)
    if info is None or info[1]!=32:raise ValueError('missing Finder information')
    at=info[0]
    if result[at:at+4]!=b'APPL':raise ValueError('expected application')
    result[at+4:at+8]=b'COff'
    assert {(t,i):(a,n,p) for t,i,a,n,p,r in entries(result)}==target
    return result

if __name__=='__main__':
    data,resource,carrier,root=map(pathlib.Path,sys.argv[1:])
    dest=root/'mac/bin/CommandShell';fork=root/'mac/bin/%CommandShell'
    if dest.is_symlink() or fork.is_symlink() or fork.resolve()==resource.resolve():
        raise ValueError('installation requires an independent staged copy')
    original=data.read_bytes();before=dest.read_bytes()
    if before!=original or original[:2]!=b'\x01\x50':raise ValueError('expected original COFF data')
    fork.write_bytes(facade(resource.read_bytes(),carrier.read_bytes()))
    assert dest.read_bytes()==before and data.read_bytes()==original
    print('CommandShell: original COFF data and resources preserved; native façade installed')
