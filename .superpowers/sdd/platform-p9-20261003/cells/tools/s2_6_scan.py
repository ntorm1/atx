"""Cells root, P9-L pre-cell check S2-6 (ledger :365; prereg B.1 "Inputs"; C.10): no `imputed-v1` IC-result entry
written by a pre-`110fd735` build exists under any cache root the cell reads.

The IC-result cache layout (atx-impl/src/strategy_ic_result_cache.cpp, ic_cache_scope): <signal entry dir>/
ic<v>_<key16>/<id>.json; each record carries its full scope key, which holds "label_terminal" (and, for imputed-v1,
"label_role_manifest_sha256") only when the IC pass ran with --label-terminal (P9 S2). This tool walks every cache
root given, groups the IC-result records by scope dir, and reads ONLY each record's scope key (no IC value is read or
printed): per scope dir the record count and the label_terminal values found. An imputed-v1 record found is listed by
path (to be deleted by name and logged, never reused, S2-6). usage: s2_6_scan.py CACHE_ROOT [CACHE_ROOT ...]
"""
from __future__ import annotations

import json
import os
import re
import sys
from collections import defaultdict

SCOPE = re.compile(r"^ic\d+_[0-9a-f]{16}$")


def key_of(doc):
    """The record's scope key (the dict that carries semantics_version / ic_identity), wherever the record keeps it."""
    if not isinstance(doc, dict):
        return None
    rec = doc.get("record")          # the entry envelope {schema, record_sha256, record{..., key}, recorded}
    if isinstance(rec, dict) and isinstance(rec.get("key"), dict):
        return rec["key"]
    for k in ("key", "scope", "scope_key"):
        v = doc.get(k)
        if isinstance(v, dict) and "ic_identity" in v:
            return v
    if "ic_identity" in doc and "semantics_version" in doc:
        return doc
    for v in doc.values():
        if isinstance(v, dict) and "ic_identity" in v and "semantics_version" in v:
            return v
    return None


def main(roots: list[str]) -> int:
    scopes = defaultdict(lambda: {"records": 0, "label_terminal": defaultdict(int), "no_key": 0, "identity": set()})
    imputed = []
    for root in roots:
        for dp, dn, fn in os.walk(root):
            dn.sort()
            name = os.path.basename(dp)
            if not SCOPE.match(name):
                continue
            for f in sorted(fn):
                if not f.endswith(".json"):
                    continue
                p = os.path.join(dp, f)
                s = scopes[(root, name)]
                s["records"] += 1
                try:
                    doc = json.loads(open(p, encoding="utf-8").read())
                except (OSError, ValueError):
                    s["no_key"] += 1
                    continue
                key = key_of(doc)
                if key is None:
                    s["no_key"] += 1
                    continue
                s["identity"].add(str(key.get("ic_identity")))
                s.setdefault("engine_git_sha", set()).add(
                    str(((doc.get("recorded") or {}) if isinstance(doc, dict) else {}).get("engine_git_sha")))
                lt = key.get("label_terminal")
                s["label_terminal"][lt if lt is not None else "<absent: flag-absent scope>"] += 1
                if lt == "imputed-v1":
                    imputed.append(p.replace(os.sep, "/"))
    total = 0
    for (root, name), s in sorted(scopes.items()):
        total += s["records"]
        print(f"{root}: scope {name}: {s['records']} IC-result records; label_terminal "
              f"{dict(s['label_terminal'])}; records without a readable key {s['no_key']}; ic_identity "
              f"{sorted(s['identity'])}; recorded engine_git_sha {sorted(s.get('engine_git_sha', set()))}")
    print(f"scope dirs {len(scopes)}, IC-result records {total}, imputed-v1 records {len(imputed)}")
    for p in imputed:
        print(f"  IMPUTED-V1 RECORD: {p}")
    print("S2-6: " + ("HOLDS (no imputed-v1 IC-result entry under the cache roots the cell reads)" if not imputed
                      else "FOUND imputed-v1 entries: delete each by name and log it, never reuse (S2-6)"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
