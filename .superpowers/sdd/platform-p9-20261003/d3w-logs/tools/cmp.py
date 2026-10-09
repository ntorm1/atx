import json, hashlib, sys
from pathlib import Path
base, head = Path(sys.argv[1]), Path(sys.argv[2])
for d in sorted(base.iterdir()):
    for f in sorted(d.iterdir()):
        a, b = f.read_bytes(), (head / d.name / f.name).read_bytes()
        if a == b:
            print(f"{d.name}/{f.name}: identical sha256 {hashlib.sha256(a).hexdigest()}")
            continue
        diffs = []
        def walk(x, y, p):
            if isinstance(x, dict) and isinstance(y, dict):
                for k in sorted(set(x) | set(y)):
                    walk(x.get(k), y.get(k), f"{p}.{k}")
            elif x != y:
                diffs.append(p)
        walk(json.loads(a), json.loads(b), "$")
        print(f"{d.name}/{f.name}: DIFFERS at {diffs}")
    extra = sorted({p.name for p in (head / d.name).iterdir()} - {p.name for p in d.iterdir()})
    if extra:
        print(f"{d.name}: extra files in head {extra}")
