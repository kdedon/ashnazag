#!/usr/bin/env python3
"""Validate admission API identities against local shipping resources."""
import importlib.util, struct, sys
from pathlib import Path
spec=importlib.util.spec_from_file_location("config",Path(__file__).with_name("configurator-evidence.py"))
config=importlib.util.module_from_spec(spec);spec.loader.exec_module(config)
root=Path(sys.argv[1])
config.check(*[(root/f"cd15-{i}.bin").read_bytes() for i in (1,0,2)],0x48a,"OTClnt$NtvUtils",{
    "OTFindConfigurator":(5,0x66c),"OTCfigNumberOfChildren":(34,0x11c2),
    "OTCfigGetProviderName":(39,0x1290),"OTCfigChangeProviderName":(44,0x1356)})
code=(root/"cd15-2.bin").read_bytes()
assert code[0x66c:0x678]==bytes.fromhex("4e56000048e70038202e000c")
assert code[0x716:0x71a]==bytes.fromhex("226e0008")
assert code[0x1356:0x1368]==bytes.fromhex("4e5600002f2e000c206e00087034d1c02f08")
print("PASS chooser output/config C argument order and public two-argument name mutation")

shared=(root/"cd32-2.bin").read_bytes()
assert shared[0x24fbe:0x24fc6]==bytes.fromhex("70012f002f2e0008")
assert shared[0x256c6:0x256d2]==bytes.fromhex("2f0a2052206800404e904fef")
assert shared[0x256ee:0x256f6]==bytes.fromhex("2052206800444e90")
assert shared[0x24fce:0x24fd2]==bytes.fromhex("360048c3")
assert shared[0x25794:0x2579a]==bytes.fromhex("2039fffffeac")
print("PASS public ASLM load/unload manager64/68, long flags1, signed OSErr result and authenticated manager getter")
