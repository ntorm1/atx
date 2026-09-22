"""Recover only the two measured archive7 ledger rows after the host guard stop."""
import datetime as dt
import json
from pathlib import Path

import duckdb

base = Path(__file__).parent
out = base / "companyfacts-archive7-ledger-recovery.json"
if out.exists():
    raise SystemExit("Refusing repeat ledger mutation")
observed = json.loads((base / "companyfacts-archive7-stop-inspection.json").read_text())
guard = json.loads((base / "activation-companyfacts-archive7-memory.json").read_text())
assert guard["status"] == "stopped_low_headroom"
run_id = "404f66a2-655b-4611-a7a0-b71d4dc6d4d3"
assert observed["attempt_rows"]["rows"] == [[run_id, 5351787, 1785, "0001347242", "0001442741"]]
assert observed["dataset"]["rows"][0][:3] == [run_id, "sec_company_facts", "running"]
assert observed["activation"]["rows"][0][:3] == ["companyfacts_load", "activation-companyfacts-archive7", "running"]
now = dt.datetime.now(dt.UTC)
reason = (
    "Memory guard terminated the owned archive7 job when host free physical memory "
    "fell to1.373397827GiB, below1.5GiB; free commit remained4.114677429GiB. "
    "Guard receipt timestamp2026-09-21T23:53:00Z; session terminal and descendants absent. "
    "5,351,787 committed attempt rows across1,785 newly processed CIKs retained; "
    "1,250 empty and41 unavailable receipts, no source errors. "
    "6,805 ancestor loaded receipts had been verified and reused. "
    "Counts include replacements, not net additions. finished_at records operator "
    "ledger recovery time. See companyfacts-archive7-stop-inspection.json."
)
with duckdb.connect("C:/atx/atx-db/data/warehouse.duckdb", config={
        "memory_limit": "1GB", "threads": "1", "preserve_insertion_order": "false"}) as con:
    con.execute("SET TimeZone='UTC'")
    con.execute("SET max_temp_directory_size='2GB'")
    con.execute("BEGIN")
    try:
        a = con.execute(
            "UPDATE activation_stage_runs SET status='failed',finished_at=?,rows=5351787,error=? "
            "WHERE run_id='activation-companyfacts-archive7' AND stage='companyfacts_load' "
            "AND status='running' AND finished_at IS NULL RETURNING stage,run_id,status,finished_at,rows",
            [now, reason],
        ).fetchall()
        b = con.execute(
            "UPDATE dataset_runs SET status='failed',finished_at=?,rows_loaded=5351787,error_message=? "
            "WHERE run_id=? AND dataset_id='sec_company_facts' AND status='running' AND finished_at IS NULL "
            "RETURNING run_id,dataset_id,status,finished_at,rows_loaded",
            [now, reason, run_id],
        ).fetchall()
        assert len(a) == len(b) == 1
        con.execute("COMMIT")
    except BaseException:
        con.execute("ROLLBACK")
        raise
    result = {"recovered_at_utc": now.isoformat(), "reason": reason,
              "activation": a, "dataset": b, "committed": True, "checkpoint": "pending"}
    with out.open("x", encoding="utf-8") as handle:
        json.dump(result, handle, indent=2, default=str)
    con.execute("CHECKPOINT")
    result["checkpoint"] = "passed"
    out.write_text(json.dumps(result, indent=2, default=str) + "\n", encoding="utf-8")
print(json.dumps(result, default=str))
