"""PM5-16 / PM5-18 (3) cause test (ii), step 1 (orientation-consumers.md Q5 (ii); integrator W0-2c).

argv: LIBRARY LIBRARY_SHA256 OLD_orientations.json NEW_TRAIN_MANIFEST_SHA256 OUT_composition_weights.json
Writes an atx.dsl-composition-weights/v1 file carrying the runner's default weights 1/(F*n_family) (the
strategy_ic_composition.cpp expression) and the 3-year run's own TRAIN signs, bound to the 4-year role; a sign-0
candidate gets weight 0 (skipped exactly as the default path skips it). Prints only the file SHA-256 and counts;
never a sign.
"""
import hashlib, json, sys
from collections import Counter
from pathlib import Path
lib_path, lib_sha, orient3, role4, out = sys.argv[1:6]
lb = Path(lib_path).read_bytes(); assert hashlib.sha256(lb).hexdigest() == lib_sha
lib = json.loads(lb)["candidates"]; o = json.loads(Path(orient3).read_bytes())
assert o["schema"] == "atx.dsl-ic-orientations/v1" and o["library_sha256"] == lib_sha
rows = o["candidates"]; assert [r["id"] for r in rows] == [c["id"] for c in lib]
fam = Counter(c["family"] for c in lib); F = len(fam); w, s = {}, {}
for c, r in zip(lib, rows):
    w[c["id"]] = 1.0 / (float(F) * float(fam[c["family"]])) if r["sign"] != 0 else 0.0
    if r["sign"] != 0: s[c["id"]] = r["sign"]
doc = {"schema": "atx.dsl-composition-weights/v1", "library_sha256": lib_sha, "train_manifest_sha256": role4,
       "weights": w, "signs": s}
p = Path(out); p.parent.mkdir(parents=True, exist_ok=False); data = (json.dumps(doc, indent=1) + "\n").encode()
p.write_bytes(data)
print({"sha256": hashlib.sha256(data).hexdigest(), "candidates": len(lib), "families": F, "unoriented": len(lib) - len(s)})
