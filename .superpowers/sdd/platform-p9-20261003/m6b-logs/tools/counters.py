"""M6b (RESUME 11) SQL4 identity: print only cache COUNTERS of an IC summary.json or a marginal_ic.json (never a
statistic, never a path). IC: per role, the candidate count and the scalar leaves of `candidate_cache` (and its
`ic_results` block) whose key names a count or the index mode. Marginal: the scalar leaves of `/inputs/pair_cache`
(the same filter) and the count of `candidates`. usage: counters.py FILE...
"""
import json
import re
import sys

COUNT = re.compile(r"(hits|misses|evaluations|written|read|imported|computed|stored|skipped|entries_count|pairs|"
                   r"rows|shards|index|mode)", re.I)


def scalars(block, prefix=""):
    out = []
    for k, v in (block or {}).items():
        if isinstance(v, dict):
            out += scalars(v, f"{prefix}{k}.")
        elif isinstance(v, (int, float, bool)) or (isinstance(v, str) and k in ("index", "mode")):
            if COUNT.search(k):
                out.append(f"{prefix}{k}={v}")
    return out


for path in sys.argv[1:]:
    doc = json.load(open(path, encoding="utf-8"))
    if "roles" in doc:
        roles = doc["roles"]
        items = roles.items() if isinstance(roles, dict) else enumerate(roles)
        for key, role in items:
            cands = role.get("candidates")
            print(f"{path}: role {key} candidates {len(cands) if cands is not None else '?'} | "
                  + " ".join(scalars(role.get("candidate_cache"))))
    else:
        pc = (doc.get("inputs") or {}).get("pair_cache")
        cands = doc.get("candidates")
        print(f"{path}: candidates {len(cands) if cands is not None else '?'} | pair_cache " + " ".join(scalars(pc)))
