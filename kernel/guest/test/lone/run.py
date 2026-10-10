#!/usr/bin/env python3
import pathlib, subprocess, tempfile, os
root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'mod/guestcore/glone.c').read_text()
source = '\n'.join(s for s in source.splitlines() if not s.startswith('#include'))
with tempfile.TemporaryDirectory(prefix='guest-lone-') as d:
    p = pathlib.Path(d)
    (p / 'production.c').write_text(source)
    driver = (root / 'mod/cpmguest/cpmguest.c').read_text().split('struct mod_drv_data cpmguest_drvdata')[0]
    driver = '\n'.join(s for s in driver.splitlines() if not s.startswith('#include'))
    (p / 'driver.c').write_text(driver)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu89', '-Wall', '-Wextra',
                    '-Wno-unused-parameter', '-Wno-int-to-pointer-cast', '-I'+str(root/'include'),
                    '-I'+d, str(root/'test/lone/test.c'), '-o', str(p/'check')], check=True)
    subprocess.run([str(p/'check')], check=True)
