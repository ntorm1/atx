"""M2d COV flag-absent identity: compare two risk-model store dirs by file SHA-256 and manifest JSON path (key names
and value equality only; no value printed except SHA / producer strings).

usage: cov_cmp.py REF_DIR NEW_DIR
"""
import hashlib
import json
import sys
from pathlib import Path


def sha(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def flatten(doc, prefix="$"):
    out = {}
    if isinstance(doc, dict):
        for k, v in doc.items():
            out.update(flatten(v, f"{prefix}.{k}"))
        if not doc:
            out[prefix] = "{}"
    elif isinstance(doc, list):
        for i, v in enumerate(doc):
            out.update(flatten(v, f"{prefix}[{i}]"))
        if not doc:
            out[prefix] = "[]"
    else:
        out[prefix] = json.dumps(doc, sort_keys=True)
    return out


ref, new = Path(sys.argv[1]), Path(sys.argv[2])
fr = {p.name for p in ref.iterdir() if p.is_file()}
fn = {p.name for p in new.iterdir() if p.is_file()}
print("files only in ref:", sorted(fr - fn))
print("files only in new:", sorted(fn - fr))
for f in sorted(fr & fn):
    if f == "manifest.json":
        continue
    a, b = sha(ref / f), sha(new / f)
    print(f"{'EQUAL' if a == b else 'DIFF '} {f} {a[:16]} {b[:16]}")
mr = json.loads((ref / "manifest.json").read_bytes())
mn = json.loads((new / "manifest.json").read_bytes())
xr, xn = flatten(mr), flatten(mn)
print("manifest paths only in ref:", sorted(set(xr) - set(xn)))
only_new = sorted(set(xn) - set(xr))
roots = sorted({p.split(".")[1].split("[")[0] if p.count(".") >= 1 else p for p in only_new})
print("manifest paths only in new:", len(only_new), "under top-level keys", roots,
      "files.* added:", [p for p in only_new if p.startswith("$.files.")][:4])
diff = sorted(k for k in set(xr) & set(xn) if xr[k] != xn[k])
print("manifest paths whose value differs:", diff)
for k in diff:
    if k.startswith("$.producer."):
        print("  ", k, xr[k], "->", xn[k])
for name, entry in mn.get("files", {}).items():
    if (new / name).is_file():
        ok = entry.get("sha256") == sha(new / name)
        if not ok:
            print("MANIFEST ENTRY MISMATCH", name)
print("manifest files entries self-consistent: checked", len(mn.get("files", {})))
