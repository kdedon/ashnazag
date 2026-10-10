#!/usr/bin/env python3
import pathlib,struct,sys
out=pathlib.Path(sys.argv[1])
for folder,name in [('provider','AUX Host Internet Library'),('configurator','AUX Host Configurator Library')]:
    data=(out/folder/name).read_bytes()
    assert data[2:2+data[1]].decode('mac_roman')==name
    data_size,resource_size=struct.unpack_from('>II',data,83)
    offset=128+(data_size+127)//128*128
    assert (out/'Extensions'/name).read_bytes()==data[128:128+data_size]
    apple=(out/'Extensions'/('%'+name)).read_bytes()
    assert struct.unpack_from('>I',apple)[0]==0x51607
    assert apple[0xe0:0xe4]==b'libr'
    assert apple[0x200:]==data[offset:offset+resource_size]
assert (out/'Extensions'/'AUXNet').exists()
assert (out/'Extensions'/'%AUXNet').read_bytes()[0xe0:0xe4]==b'INIT'
print('PASS 12 paired library names, forks, types and network initializer checks')
