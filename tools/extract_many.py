import importlib.util, sys, os
from pathlib import Path

spec = importlib.util.spec_from_file_location('xp3', r'C:\Users\10530\Desktop\project\KRKR-ns\tools\xp3_index.py')
xp3 = importlib.util.module_from_spec(spec)
try:
    spec.loader.exec_module(xp3)
except SystemExit:
    pass

# usage: extract_many.py <archive> <outdir> <name1> [<name2> ...]
archive = Path(sys.argv[1])
outdir = Path(sys.argv[2])
outdir.mkdir(parents=True, exist_ok=True)
wanted = {n.lower(): n for n in sys.argv[3:]}
es = list(xp3.entries(archive))
for name, orig, arch, segs in es:
    key = str(name).lower()
    if key in wanted:
        data = xp3.extract(archive, segs)
        out = outdir / Path(str(name).replace('\\', '_'))
        out.write_bytes(data)
        print('ok', name, len(data))
