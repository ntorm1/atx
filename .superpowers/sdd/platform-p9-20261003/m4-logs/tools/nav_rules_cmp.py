"""Root M4 (RESUME 10): compare two NAV `nav --list-rules --json` envelopes (W3-9 / COV-MV review item 3).

usage: nav_rules_cmp.py BEFORE.json AFTER.json [ID_EXPECTED_LAST]
Prints the rows added / removed / changed by id (a changed row names its differing keys), the capability diff, and,
for ID_EXPECTED_LAST: whether it is the last row, the only row of its kind, and its kind / x-flag / incompatible[].
Registry text only (no run output, no statistic).
"""
import json
import sys

before = json.load(open(sys.argv[1], encoding="utf-8"))
after = json.load(open(sys.argv[2], encoding="utf-8"))
last = sys.argv[3] if len(sys.argv) > 3 else None
rb = {r["id"]: r for r in before["rules"]}
ra = {r["id"]: r for r in after["rules"]}
print("schema", before.get("schema"), "->", after.get("schema"))
print("rows", len(rb), "->", len(ra))
print("added:", [i for i in ra if i not in rb])
print("removed:", [i for i in rb if i not in ra])
changed = {i: sorted(k for k in set(rb[i]) | set(ra[i]) if rb[i].get(k) != ra[i].get(k)) for i in rb if i in ra}
print("changed:", {i: ks for i, ks in changed.items() if ks})
order_b = [r["id"] for r in before["rules"]]
order_a = [r["id"] for r in after["rules"] if r["id"] in rb]
print("order of the kept rows unchanged:", order_b == order_a)
print("capabilities", sorted(set(after.get("capabilities") or []) - set(before.get("capabilities") or [])), "added,",
      sorted(set(before.get("capabilities") or []) - set(after.get("capabilities") or [])), "removed")
if last:
    row = ra.get(last)
    print(f"{last}: present {row is not None}")
    if row is not None:
        same_kind = [r["id"] for r in after["rules"] if r["kind"] == row["kind"]]
        print(f"  last row: {after['rules'][-1]['id'] == last}; kind {row['kind']}; rows of that kind {same_kind}")
        print(f"  x-flag {row.get('x-flag')}; incompatible {row.get('incompatible')}")
