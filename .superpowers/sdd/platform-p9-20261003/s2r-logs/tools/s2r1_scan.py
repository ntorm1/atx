"""S2R-1 source scan (scratch): which book / P9 strings call a flat-window-split op. Specs only, no data."""
import json
import re
import sys
from pathlib import Path

ROOT = Path("C:/atx-wt/pool-18")
OPS = ["ts_zscore", "stddev", "ts_std", "ts_var", "slope", "rsquare", "resid"]
PAT = re.compile(r"(?<![A-Za-z0-9_.])(" + "|".join(OPS) + r")\s*\(")


def walk_strings(node, path=""):
    if isinstance(node, str):
        yield path, node
    elif isinstance(node, dict):
        for k, v in node.items():
            yield from walk_strings(v, f"{path}.{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from walk_strings(v, f"{path}[{i}]")


spec = json.loads((ROOT / "scripts/specs/v8/lib-v8ysb-gm.json").read_text(encoding="utf-8"))
lib_path = spec["inputs"]["library"]["path"]
print("lib-v8ysb-gm library:", lib_path, "| other inputs keys:", sorted(spec["inputs"].keys()))
book = json.loads((ROOT / lib_path).read_text(encoding="utf-8"))
cands = book["candidates"]
print("book candidates:", len(cands), "| fields:", len(book["fields"]))
print("== book candidates (inline dsl) using a split op")
nb = 0
for c in cands:
    if PAT.search(c["dsl"]):
        nb += 1
        print(f"-- {c['id']} ({c['family']}): ops={sorted(set(PAT.findall(c['dsl'])))}")
        print(f"      {c['dsl']}")
print("book candidates hit:", nb)
lib = json.loads((ROOT / "atx-impl/strategies/libraries/v8ysb.json").read_text(encoding="utf-8"))
members = lib["members"]
print("\n(cross-check) libraries/v8ysb.json members:", len(members),
      "same ids as the book:", sorted(members) == sorted(c["id"] for c in cands))
reg = json.loads((ROOT / "atx-impl/strategies/alphas/registry.json").read_text(encoding="utf-8"))

# find the alpha list in the registry (shape-agnostic)
by_id = {}
for p, s in walk_strings(reg):
    pass
def collect(node):
    if isinstance(node, dict):
        if "id" in node and isinstance(node.get("id"), str) and any(k in node for k in ("dsl", "expr", "expression")):
            by_id[node["id"]] = node
        for v in node.values():
            collect(v)
    elif isinstance(node, list):
        for v in node:
            collect(v)
collect(reg)
print("registry entries with an expression:", len(by_id))
missing = [m for m in members if m not in by_id]
print("members without a registry entry:", missing)
# any member-level expression overrides inside the library file itself
lib_hits = [(p, s) for p, s in walk_strings(lib) if PAT.search(s)]
print("library-file strings with a split op:", lib_hits)
spec_hits = [(p, s) for p, s in walk_strings(spec) if PAT.search(s)]
print("lib-v8ysb-gm spec strings with a split op:", spec_hits)

print("\n== book members using a split op")
n_hit = 0
for m in members:
    node = by_id.get(m)
    if node is None:
        continue
    strings = [(p, s) for p, s in walk_strings(node) if PAT.search(s)]
    if strings:
        n_hit += 1
        print(f"-- {m}")
        for p, s in strings:
            print(f"   {p}: ops={sorted(set(PAT.findall(s)))}")
            print(f"      {s}")
print("registry-resolved members hit:", n_hit)
diff = [c["id"] for c in cands if c["id"] in by_id and by_id[c["id"]].get("dsl") not in (None, c["dsl"])]
print("book dsl != registry dsl for:", diff)

print("\n== scripts/specs/p9 strings using a split op")
for f in sorted((ROOT / "scripts/specs/p9").rglob("*.json")):
    data = json.loads(f.read_text(encoding="utf-8"))
    allstr = list(walk_strings(data))
    hits = [(p, s) for p, s in allstr if PAT.search(s)]
    print(f"{f.relative_to(ROOT)}: {len(allstr)} strings, {len(hits)} with a split op")
    for p, s in hits:
        print(f"   {p}: ops={sorted(set(PAT.findall(s)))}")
        print(f"      {s}")
