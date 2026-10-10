import pathlib,runpy,struct,sys
root=pathlib.Path(__file__).resolve().parents[3]
runpy.run_path(str(root/"tests/aux/mac81probe/mkapp.py"),run_name="__main__")
p=pathlib.Path(sys.argv[2]);d=bytearray(p.read_bytes());d[0xe4:0xe8]=b"COff";p.write_bytes(d)

if len(sys.argv)>3:
    sys.path.insert(0,str(root/"tools"))
    from rsrc import merge
    p.write_bytes(merge(p.read_bytes(),pathlib.Path(sys.argv[3]).read_bytes()))
