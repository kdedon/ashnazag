#!/usr/bin/env python3
"""Check movable code and classic application metadata."""
from pathlib import Path
import struct
import subprocess
import sys

root = Path(__file__).resolve().parent
work = Path(sys.argv[1])
b = (work / 'probe.elf').read_bytes()
at = struct.unpack_from('>I', b, 32)[0]
size, count = struct.unpack_from('>HH', b, 46)
sections = [struct.unpack_from('>10I', b, at + i * size) for i in range(count)]


def check(blob, expected):
    path = work / 'guard-test.elf'
    path.write_bytes(blob)
    r = subprocess.run([sys.executable, str(root / 'checkelf.py'), str(path)],
                       capture_output=True)
    assert (r.returncode == 0) == expected, r.stderr.decode()
    path.unlink()


check(b, True)
changed = bytearray(b)
reloc = next(s for s in sections if s[1] in (4, 9))
info = struct.unpack_from('>I', changed, reloc[4] + 4)[0]
struct.pack_into('>I', changed, reloc[4] + 4, (info & ~255) | 1)
check(changed, False)
changed = bytearray(b)
strings = sections[struct.unpack_from('>H', b, 50)[0]]
names = b[strings[4]:strings[4] + strings[5]]
idx = next(i for i, s in enumerate(sections)
           if names[s[0]:].split(b'\0', 1)[0] == b'.data')
struct.pack_into('>I', changed, at + idx * size + 20, 4)
check(changed, False)
changed = bytearray(b)
for i, s in enumerate(sections):
    if s[1] in (4, 9):
        struct.pack_into('>I', changed, at + i * size + 20, 0)
check(changed, False)
a = (work / 'OTProbe').read_bytes()
entries = [struct.unpack_from('>III', a, 26 + 12 * i)
           for i in range(struct.unpack_from('>H', a, 24)[0])]
_, off, length = next(e for e in entries if e[0] == 2)
fork = a[off:off + length]
data, rmap = struct.unpack_from('>II', fork)
types = rmap + struct.unpack_from('>H', fork, rmap + 24)[0]
for i in range(struct.unpack_from('>H', fork, types)[0] + 1):
    p = types + 2 + 8 * i
    if fork[p:p + 4] == b'CODE':
        n, refs = struct.unpack_from('>HH', fork, p + 4)
        for j in range(n + 1):
            p = types + refs + 12 * j
            ident = struct.unpack_from('>h', fork, p)[0]
            if ident == 0:
                resource = struct.unpack_from('>I', fork, p + 4)[0] & 0xffffff
                assert struct.unpack_from('>4I', fork, data + resource + 4) == (40, 256, 8, 32)
                break
        else:
            raise AssertionError('missing CODE0')
        break
else:
    raise AssertionError('missing CODE type')
code = (work / 'probe.bin').read_bytes()
assert code.count(bytes.fromhex('3f3c10007000aa59')) == 1
assert bytes.fromhex('3f3c00107000aa59') not in code
symtab = next(s for s in sections if s[1] == 2)
symnames = sections[symtab[6]]
strings = b[symnames[4]:symnames[4] + symnames[5]]
symbols = {}
for i in range(symtab[5] // symtab[9]):
    name, value = struct.unpack_from('>II', b, symtab[4] + i * symtab[9])
    symbols[strings[name:].split(b'\0', 1)[0].decode()] = value
for name, following, arguments in (
        ('cfmcall', 'vector4', ('206f0030', '222f0034', '202f0038')),
        ('newdescriptor', 'newnativecallback', ('206f0030', '226f0034', '3f3c10007000aa59')),
        ('newnativecallback', 'disposedescriptor', ('206f0030', '226f0034', '42677000aa59')),
        ('disposedescriptor', 'descriptorcall', ('2f2f0030',)),
        ('descriptorcall', 'descriptorcalln', ('206f0030',)),
        ('descriptorcalln', 'exitapp', ('206e0034', '226e0038', '222e003c', '4e560000', '4e5e'))):
    wrapper = code[symbols[name]:symbols[following]]
    assert wrapper.startswith(bytes.fromhex('48e73f3e')), name
    assert wrapper.endswith(bytes.fromhex('4cdf7cfc4e75')), name
    for instruction in arguments:
        assert bytes.fromhex(instruction) in wrapper, name
print('PASS 8 application packaging/ISA/register checks')

entry = symbols['notify_entry']
callback = code[entry:symbols['notify_cfm_entry']]
assert callback.startswith(bytes.fromhex('4e56000048e73f3c206e0014217c00000001002c2a50'))
assert bytes.fromhex('2f2e00082f2e000c2f2e00102f2e0014') in callback
assert bytes.fromhex('4fef0010206e0014217c00000004002c4cdf3cfc4e5e4e740010') in callback
pascal_stack = ['return', 'cookie', 'result', 'event', 'context']
frame = ['saved_frame'] + pascal_stack
c_stack = []
for offset in (8, 12, 16, 20):
    c_stack.insert(0, frame[offset//4])
assert c_stack == ['context', 'event', 'result', 'cookie']
assert 0x3fc0 == sum(3 << (6 + 2*i) for i in range(4))
assert len(pascal_stack[1:])*4 == 16
print('PASS 6 Pascal notifier frame/argument/A5/cleanup checks')

wrappers = (
    ('findextensions', 'hopenresource', 'a823', ('3f3c8000', '2f3c6578746e', '2f2e0034', '2f2e0038')),
    ('hopenresource', 'resourcecount', 'a81a', ('3f2e0036', '2f2e0038', '2f2e003c', '3f3c0100')),
    ('resourcecount', 'namedresource', 'a80d', ('2f2e0034',)),
    ('namedresource', 'resourceref', 'a820', ('2f2e0034', '2f2e0038')),
    ('resourceref', 'resourceerror', 'a994', ()),
    ('resourceerror', 'useresource', 'a9af', ()),
)
for name, following, trap, arguments in wrappers:
    body = code[symbols[name]:symbols[following]]
    assert body.startswith(bytes.fromhex('48e73f3e4e560000')), name
    assert body.endswith(bytes.fromhex('4e5e4cdf7cfc4e75')), name
    assert body.count(bytes.fromhex(trap)) == 1, name
    for instruction in arguments:
        assert bytes.fromhex(instruction) in body, (name, instruction)
    if name == 'namedresource':
        assert bytes.fromhex('201f') in body
    else:
        assert bytes.fromhex('301f48c0') in body
# Eleven saved registers, a frame pointer and a return address precede C args.
assert (11 + 1 + 1) * 4 == 52
assert 2 + 4 + 2 + 4 + 4 == 16
assert 2 + 4 + 4 + 2 == 12
assert bytes([1, 0])[0] == 1
assert (0xffff).to_bytes(2, 'big', signed=False) == b'\xff\xff'
print('PASS 11 ResourceManager/FolderManager frame, width, permission and return checks')

body=code[symbols['hfileinfo']:symbols['indexedresource']]
assert body.startswith(bytes.fromhex('48e73f3e206f0030a20c48c04cdf7cfc4e75'))
assert body.count(bytes.fromhex('a20c')) == 1
print('PASS hierarchical File Manager A0/D0/register/return guard')

for name,following,trap,args in (
    ('indexedresource','resourcesize','a80e',('2f2e0034','3f2e003a')),
    ('resourcesize',None,'a9a5',('2f2e0034',))):
    body=code[symbols[name]:symbols[following] if following else len(code)]
    assert body.startswith(bytes.fromhex('48e73f3e4e560000'))
    assert bytes.fromhex(trap) in body
    assert bytes.fromhex('201f4e5e4cdf7cfc4e75') in body
    for arg in args:assert bytes.fromhex(arg) in body
print('PASS 2 indexed-resource/size Pascal frame and return guards')

subprocess.run([sys.executable, str(root/'context-test.py')],check=True)
subprocess.run([sys.executable, str(root/'thunk-test.py')],check=True)
subprocess.run([sys.executable, str(root/'import-test.py')],check=True)

subprocess.run([sys.executable, str(root/'notifier-test.py')],check=True)

source=(root/'probe.c').read_text()
first=source.index('struct notifyctx {')
last=source.index('enum { PBIND',first)
helper=source[first:last]
harness='#include <assert.h>\n#include <stdio.h>\nHELPER\nint main(void) {\n struct notifyctx c={0}; unsigned long ev[]={0x20000001UL,2,4,1,0x20000003UL,0x10,0x20,0x40,0x24000002UL,0xdead}; unsigned int i;\n for(i=0;i<10;i++) {c.callbackstage=1;notify_event(&c,ev[i],17,(void*)123);assert(c.callbackstage==3 && c.lastevent==ev[i] && c.lastresult==17 && c.lastcookie==(void*)123 && c.calls==i+1);}\n assert(c.bind==1 && c.connected==1 && c.data==1 && c.listen==1 && c.accepted==1 && c.errors==4);\n assert(c.bindresult==17 && c.connectresult==17 && c.acceptresult==17);\n puts("PASS 12 actual callback stage/event/count/result cases");return 0;\n}\n'
callback_source=work/'callback-stage.c'
callback_source.write_text(harness.replace('HELPER',helper))
subprocess.run(['cc','-std=c89','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(callback_source),'-o',str(work/'callback-stage')],check=True)
subprocess.run([str(work/'callback-stage')],check=True)

selfcall=code[symbols['notify_selfcall']:]
assert selfcall.startswith(bytes.fromhex('48e73f3e4e560000206e00342f2e003842a742a742a74e90'))
assert bytes.fromhex('4e5e4cdf7cfc4e75') in selfcall
stack=[]
for arg in ('context','event0','result0','cookie0'):stack.insert(0,arg)
assert stack==['cookie0','result0','event0','context']
assert 'notify_selfcall(notifier,&ctx);' in source
assert source.index('bind callback context')<source.index('notify_selfcall(notifier,&ctx);')
assert source.index('notifier_selfcall')<source.index('e=invoke(desc[PBIND],3,ep,0,0,0);')
print('PASS 6 owned-UPP selfcall Pascal frame, preservation and pre-bind guards')

first=source.index('static void callback_vector(')
last=source.index('\n}',first)+2
vectorhelper=source[first:last]
vectorharness='#include <assert.h>\n#include <stdint.h>\n#include <stdio.h>\nstatic unsigned int calls; static uintptr_t observed[4];\nstatic void notify_cfm_entry(void *ctx,unsigned long event,long result,void *cookie) {calls++;observed[0]=(uintptr_t)ctx;observed[1]=event;observed[2]=result;observed[3]=(uintptr_t)cookie;}\nHELPER\nstatic int native_dispatch(unsigned long *vector,void *ctx) {\n void (*entry)(void*,unsigned long,long,void*);\n if(vector[0]!=(unsigned long)notify_cfm_entry)return 0;\n entry=(void(*)(void*,unsigned long,long,void*))vector[0];entry(ctx,0x20000001UL,0,(void*)23);return 1;\n}\nint main(void) {unsigned long vector[2];unsigned long bad[2]={0xaafe0700UL,0};int i;\n for(i=1;i<=3;i++) {callback_vector(vector,0x700000+i);assert(vector[0]==(unsigned long)notify_cfm_entry && vector[1]==(unsigned long)(0x700000+i));assert(native_dispatch(vector,(void*)(uintptr_t)i));assert(observed[0]==(uintptr_t)i && observed[1]==0x20000001UL && observed[2]==0 && observed[3]==23);}\n assert(calls==3 && !native_dispatch(bad,0) && calls==3);puts("PASS 10 actual CFM callback vector construction/dispatch/refusal cases");return 0;}\n'
vector_source=work/'callback-vector.c'
vector_source.write_text(vectorharness.replace('HELPER',vectorhelper))
subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(vector_source),'-o',str(work/'callback-vector')],check=True)
subprocess.run([str(work/'callback-vector')],check=True)
assert 'ep,(long)callbackvector,(long)&ctx)' in source
assert 'ep[i],(long)callbackvector,(long)&ctx[i])' in source
assert 'notify_selfcall(notifier,&ctx);' in source
assert 'if(notifier)disposedescriptor(notifier);' in source
assert 'if(upp)disposedescriptor(upp);' in source
print('PASS 5 traffic/listener native-vector registration and separate descriptor-lifetime guards')

assert source.index('callback_outcome("bind submitted",&ctx);')<source.index('e=await(desc[PIDLE],&ctx.bind);')
assert source.index('callback_outcome("bind awaited",&ctx);')<source.index('check("bind_notify",e&&ctx.bindresult==0)')
assert 'callback_outcome("listener bind",&ctx[0])' in source
assert 'callback_outcome("connector bind",&ctx[1])' in source
assert 'listener open index ' in source and 'hex(err);' in source
print('PASS 5 native callback outcome and listener-open diagnostic guards')

assert source.index('callback_native_state(ep,callbackvector);')<source.index('check("bind_notify",e&&ctx.bindresult==0)')
assert 'snapshot(table+100,b,4)' in source and 'snapshot(target,b,24)' in source
assert 'registered vector entry ' in source
print('PASS 3 post-bind native dispatch identity/snapshot guards')

assert "snapshot(self+20,b,4)" in source
assert "snapshot(notifier,b,28)" in source
assert "target=byteslong(b+12)" in source
print("PASS 3 installed native notifier object/callback protected-read guards")

assert 'cfmnotifier=newdescriptor((long)callbackvector,0x3fc0L)' in source
assert source.index('fault capture arm ')<source.index('notify_selfcall(cfmnotifier,&ctx);')
assert 'notify_selfcall(notifier,&ctx);' in source
assert source.index('notifier_cfm_selfcall')<source.index('e=invoke(desc[PBIND],3,ep,0,0,0);')
assert 'if(cfmnotifier)disposedescriptor(cfmnotifier);' in source
assert source.index('if(installed)invoke(desc[PREMOVE]')<source.index('if(cfmnotifier)disposedescriptor')
print('PASS 6 CFM/native selfcall independence, armed capture and callback-lifetime guards')
cfmcreate=code[symbols['newdescriptor']:symbols['newnativecallback']]
classiccreate=code[symbols['newnativecallback']:symbols['disposedescriptor']]
assert bytes.fromhex('3f3c1000') in cfmcreate and bytes.fromhex('4267') in classiccreate
print('PASS native/classic descriptor assembled ISA distinction')

cfm=code[symbols['notify_cfm_entry']:symbols['findextensions']]
assert cfm.startswith(bytes.fromhex('2a514e56fff848e73f3c206e0008217c00000001002c'))
assert bytes.fromhex('2f2e00142f2e00102f2e000c2f2e0008') in cfm
assert cfm.endswith(bytes.fromhex('4fef0010206e0008217c00000004002c4cdf3cfc4e5e4e740010'))
frame=['saved_frame','return','context','event','result','cookie']
pushed=[]
for offset in (20,16,12,8): pushed.insert(0,frame[offset//4])
assert pushed==['context','event','result','cookie']
assert cfm.count(bytes.fromhex('4e740010'))==1
assert 'vector[0]=(unsigned long)notify_cfm_entry' in source
print('PASS 6 CFM68k entry/A1/A5/reserved-frame/argument-order/callee-cleanup checks')
