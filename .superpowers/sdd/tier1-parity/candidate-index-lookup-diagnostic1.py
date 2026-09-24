"""Read-only, 64 MB diagnosis of the failed scratch fixture; never production."""
import hashlib
import json
from pathlib import Path

import duckdb

path = Path("C:/Users/natha/AppData/Local/Temp/pytest-of-natha/pytest-82/test_two_phase_rebuild_preserv0/candidates.duckdb")
output = Path("C:/atx/.superpowers/sdd/tier1-parity/candidate-index-lookup-diagnostic1.json")
assert path.is_file() and not output.exists()
before = hashlib.sha256(path.read_bytes()).hexdigest()
base = "SELECT candidate_id FROM main.identifier_resolution_candidates WHERE target_security_id=?"
report = {"database": str(path), "duckdb_version": duckdb.__version__, "queries": {}}
with duckdb.connect(str(path), read_only=True, config={"memory_limit": "64MB", "threads": "1"}) as con:
    settings = con.execute("SELECT current_setting('index_scan_max_count'), current_setting('index_scan_percentage')").fetchone()
    report["settings"] = settings
    for label, sql in {
        "ordered_limit": base + " ORDER BY candidate_id LIMIT 1001",
        "plain": base,
        "limit": base + " LIMIT 1001",
        "ordered": base + " ORDER BY candidate_id",
    }.items():
        indexed = con.execute(sql, ["issuer-a"]).fetchall()
        plan = "\n".join(str(row[-1]) for row in con.execute("EXPLAIN ANALYZE " + sql, ["issuer-a"]).fetchall())
        con.execute("SET index_scan_max_count=0")
        con.execute("SET index_scan_percentage=0")
        sequential = con.execute(sql, ["issuer-a"]).fetchall()
        con.execute("SET index_scan_max_count=?", [settings[0]])
        con.execute("SET index_scan_percentage=?", [settings[1]])
        report["queries"][label] = {"default_rows": indexed, "sequential_rows": sequential,
                                    "default_uses_index": "Index Scan" in plan, "plan": plan}
report["file_unchanged"] = hashlib.sha256(path.read_bytes()).hexdigest() == before
output.write_text(json.dumps(report, indent=2), encoding="utf-8")
print(json.dumps({"file_unchanged": report["file_unchanged"], "queries": {
    key: {k: v for k, v in value.items() if k != "plan"} for key, value in report["queries"].items()
}}))
