"""M6B-FINISH (RESUME 12) item 4, SQL4-S S-1: the cause of the fixture catalog digest change, table by table.

Compares `pre-dump/<table>.txt` with `post-dump/<table>.txt` (the `atx-research-store dump --table` output of the
pre-merge p9-2r and the post-merge p9-3e store exe over the committed fixture tree; fixture rows, no real data).
Each dump line is one JSON row. A table is SAME when its bytes are equal; otherwise every row pair is compared as
JSON after masking only the VOLATILE values named below, and every remaining differing key is printed by name.

Volatile (run identity, not content):
  * `catalog_run_id` / `started_utc` (the catalog run's timestamp id) in every table;
  * the store exe's own SHA-256: `store_info.created_by`, `catalog_run.store_exe_sha256`;
  * `catalog_run.seconds` (wall time) and `catalog_run.catalog_digest` (the quantity under test: shown separately).
Then `ledger_state`'s two head keys are checked against B2's chain rule: head_rule must equal kChainRule
(`atx-ledger-chain-fold/v1`) and head_sha256 must equal `backtest_integrity.chain_head` of the fixture ledger's
lines (B2's Python copy of the rule; the C++ `chain.hpp` is the other copy).

usage: dump_cmp.py LOGS_DIR FIXTURE_LEDGER REPO
"""
import importlib.util
import json
import sys
from pathlib import Path

logs, ledger, repo = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
VOLATILE = {
    "*": {"catalog_run_id", "started_utc"},
    "store_info": {"value"},          # only the created_by row is masked (checked by key below)
    "catalog_run": {"store_exe_sha256", "seconds", "catalog_digest"},
}
outside = 0
ledger_rows = None
for pre in sorted((logs / "pre-dump").glob("*.txt")):
    t = pre.stem
    post = logs / "post-dump" / pre.name
    a, b = pre.read_bytes(), post.read_bytes()
    if a == b:
        print(f"SAME   {t}")
        continue
    la, lb = a.decode("utf-8").splitlines(), b.decode("utf-8").splitlines()
    if len(la) != len(lb):
        print(f"DIFF   {t}: row count {len(la)} vs {len(lb)} OUTSIDE")
        outside += 1
        continue
    keys = set()
    for x, y in zip(la, lb):
        ja, jb = json.loads(x), json.loads(y)
        for k in sorted(set(ja) | set(jb)):
            if ja.get(k) == jb.get(k):
                continue
            if k in VOLATILE["*"] or k in VOLATILE.get(t, set()) and (t != "store_info" or ja.get("key") == "created_by"):
                keys.add(f"{k} (volatile)")
            else:
                keys.add(k)
        if t == "ledger_state":
            ledger_rows = (ja, jb)
    hard = sorted(k for k in keys if not k.endswith("(volatile)"))
    print(f"DIFF   {t}: {len(la)} rows; differing keys {sorted(keys)}")
    if t != "ledger_state":
        outside += len(hard)
spec = importlib.util.spec_from_file_location("bi", repo / "atx-impl" / "tools" / "backtest_integrity.py")
bi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bi)
lines = [line for line in ledger.read_text(encoding="utf-8").splitlines() if line.strip()]
want = bi.chain_head(lines)
if ledger_rows:
    ja, jb = ledger_rows
    other = sorted(k for k in set(ja) | set(jb) if ja.get(k) != jb.get(k) and k not in ("head_sha256", "head_rule"))
    print(f"ledger_state pre  head_sha256 {ja.get('head_sha256')} head_rule {ja.get('head_rule')}")
    print(f"ledger_state post head_sha256 {jb.get('head_sha256')} head_rule {jb.get('head_rule')}")
    print(f"B2 chain_head of the fixture ledger ({len(lines)} lines): {want}; CHAIN_RULE {bi.CHAIN_RULE}")
    print(f"post head = B2 chain head: {jb.get('head_sha256') == want}; post rule = kChainRule: "
          f"{jb.get('head_rule') == bi.CHAIN_RULE}; pre both NULL: "
          f"{ja.get('head_sha256') is None and ja.get('head_rule') is None}; other ledger_state keys differing: {other}")
    outside += len(other) + (jb.get("head_sha256") != want) + (jb.get("head_rule") != bi.CHAIN_RULE)
print(f"TOTAL non-volatile differences outside ledger_state.head_sha256 / head_rule: {outside}")
