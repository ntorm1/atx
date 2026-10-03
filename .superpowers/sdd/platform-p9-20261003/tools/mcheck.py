"""P12 marginal-row check (no value printed): the screen's marginal_ic.json rows (contract K6) of the given ids vs
(a) the screen receipt's carried rows and (b) the wave result's marginal rows, by canonical-JSON SHA-256 per row.
usage: mcheck.py STAGE   (screen: source vs the 03-screen receipt; result: source vs wave-result.json)
"""
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path("C:/atx-wt/pool-2")
MK = ("id", "ic21", "ic21_hac_t", "marginal_ic21", "marginal_hac_t", "max_abs_rho", "max_rho_member")
PER_ROW = ("id", "ic21", "ic21_hac_t", "marginal_ic21", "marginal_hac_t")


def canon(x) -> str:
    return hashlib.sha256(json.dumps(x, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


rec = json.loads((ROOT / "build-equity/waves/y-s/receipts/03-screen.json").read_text(encoding="utf-8"))
sc = rec["outputs"]
spec = json.loads((ROOT / sc["spec"]).read_text(encoding="utf-8"))
mout = spec["marginal"]["output"]
src_doc = json.loads((ROOT / mout / "marginal_ic.json").read_text(encoding="utf-8"))
src_rows = src_doc if isinstance(src_doc, list) else src_doc.get("candidates", [])
src = {r["id"]: r for r in src_rows if isinstance(r, dict)}
print(f"source {mout}/marginal_ic.json sha256 "
      f"{hashlib.sha256((ROOT / mout / 'marginal_ic.json').read_bytes()).hexdigest()}; rows {len(src)}")
ids = [c["id"] for c in json.loads((ROOT / "scripts/specs/v8/waves/y-s.json").read_text(encoding="utf-8"))["candidates"]]
mode = sys.argv[1]
if mode == "screen":
    got = {r["id"]: r for r in sc["marginal"]}
    keys = MK
else:
    res = json.loads((ROOT / "build-equity/waves/y-s/wave-result.json").read_text(encoding="utf-8"))
    mg = res.get("marginal") or {}
    print(f"result marginal source: {mg.get('source')}; mode {mg.get('mode')}; rows {len(mg.get('rows') or [])}")
    got = {r["id"]: r for r in mg.get("rows") or []}
    keys = PER_ROW if "carried" in str(mg.get("source")) else MK
    ids = [i for i in ids if i in got] or ids
eq = 0
for i in ids:
    a = {k: src.get(i, {}).get(k) for k in keys}
    b = {k: got.get(i, {}).get(k) for k in keys}
    same = i in src and i in got and canon(a) == canon(b)
    eq += same
    print(f"  {i}: {'equal' if same else 'DIFFERENT'} {canon(a)[:12]} {canon(b)[:12]}")
print(f"{eq} of {len(ids)} rows byte-equal on keys {keys}")
