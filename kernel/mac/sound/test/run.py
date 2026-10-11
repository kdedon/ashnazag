#!/usr/bin/env python3
import os, pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='mac-sound-test-') as tmp:
    t=pathlib.Path(tmp);(t/'sys').mkdir();(t/'sys/stropts.h').write_text('')
    for name in ('relay',):
        exe=t/name
        subprocess.run(['nice','-n','19',os.environ.get('CC','cc'),'-std=gnu89','-w','-I'+tmp,str(root/(name+'.c')),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=10)
