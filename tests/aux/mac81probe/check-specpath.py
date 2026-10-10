#!/usr/bin/env python3
"""Exercise the production path continuation with a guest catalog fixture."""
import os
import re
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[3]
source = (root / "kernel/guest/mod/uinter/uipatch.c").read_text()
code = source[source.index("struct uispecpath {"):source.index("static int\ncatspec(r)")]
hfst = re.search(r"if \((!\(gp->gp_flags & GPF_STUB\) && !\(tw & 0x400\) && hfsspec\(r, tw\))\)", source).group(1)
pspec = re.search(r"if \((p->p_kind == PSPEC.*?)\) \{", source, re.S).group(1)
clear = re.search(r"if \(i != 2\)\s*gp->gp_flags &= ~GPF_STUB;", source).group(0)
guards = """
#define GPF_STUB 1
#define PSPEC 5
static int intercepted;
static int hfsspec(char *r,int tw) {intercepted++;return 1;}
static void guardtests(void) {
 struct {int gp_flags;} g, *gp=&g;
 struct {int p_kind,p_sysver,p_trap;} patch, *p=&patch;
 char *r=0;int tw=0xa260,sv=0x810,i;
 patch.p_kind=PSPEC;patch.p_sysver=sv;patch.p_trap=tw&~0x200;
 gp->gp_flags=GPF_STUB;intercepted=0;i=2;
 CLEAR
 if (HFS) { }
 if (SPEC) { }
 need(gp->gp_flags==GPF_STUB && intercepted==0,"native continuation cannot reintercept");
 i=1;CLEAR
 if (HFS) { }
 if (SPEC) { }
 need(gp->gp_flags==0 && intercepted==2,"completed continuation restores interception");
}
""".replace("CLEAR", clear).replace("HFS", hfst).replace("SPEC)", pspec+")")
harness = Path(__file__).with_suffix(".c").read_text().replace("/* PRODUCTION */", code).replace("/* PRODUCTION_GUARDS */", guards)
with tempfile.TemporaryDirectory(prefix="specpath-") as tmp:
    c, exe = Path(tmp)/"test.c", Path(tmp)/"test"
    c.write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu89", "-w", "-fsanitize=address,undefined", "-g", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
