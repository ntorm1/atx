"""Recover only the two verified archive6 ledger rows after a host-memory stop."""
import datetime as dt
import json
from pathlib import Path

import duckdb

base = Path(__file__).parent
out = base / "companyfacts-archive6-ledger-recovery.json"
if out.exists():
    raise SystemExit("Refusing repeat ledger mutation")
observed = json.loads((base / "companyfacts-archive6-stop-inspection.json").read_text())
guard = json.loads((base / "activation-companyfacts-archive6-memory.json").read_text())
assert guard["status"] == "stopped_low_headroom"
run_id = "7da8bd67-de3a-4fe6-a9bc-08a7a7d4cce7"
assert observed["attempt_rows"]["rows"] == [[run_id, 699244, 235, "0001329957", "0001347218"]]
assert observed["dataset"]["rows"][0][:3] == [run_id, "sec_company_facts", "running"]
assert observed["activation"]["rows"][0][:3] == ["companyfacts_load", "activation-companyfacts-archive6", "running"]
now = dt.datetime.now(dt.UTC)
reason = (
    "Memory guard terminated the owned archive6 job when host free physical memory "
    "fell to1.4619865GiB, below1.5GiB; free commit remained5.913353GiB. "
    "Guard receipt timestamp2026-09-21T22:23:30Z; session terminal and descendants absent. "
    "699,244 committed attempt rows across235 newly processed CIKs retained; "
    "1,128 empty and39 unavailable receipts, no source errors. "
    "6,570 ancestor loaded receipts had been verified and reused. "
    "Counts include replacements, not net additions. finished_at records operator "
    "ledger recovery time. See companyfacts-archive6-stop-inspection.json."
)
with duckdb.connect("C:/atx/atx-db/data/warehouse.duckdb", config={
        "memory_limit": "1GB", "threads": "1", "preserve_insertion_order": "false"}) as con:
    con.execute("SET TimeZone='UTC'")
    con.execute("SET max_temp_directory_size='2GB'")
    con.execute("BEGIN")
    try:
        a = con.execute(
            "UPDATE activation_stage_runs SET status='failed',finished_at=?,rows=699244,error=? "
            "WHERE run_id='activation-companyfacts-archive6' AND stage='companyfacts_load' "
            "AND status='running' AND finished_at IS NULL RETURNING stage,run_id,status,finished_at,rows",
            [now, reason],
        ).fetchall()
        b = con.execute(
            "UPDATE dataset_runs SET status='failed',finished_at=?,rows_loaded=699244,error_message=? "
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
