"""Close only archive5's verified user-interrupted ledgers after explicit resume."""
import datetime as dt
import json
from pathlib import Path

import duckdb

base = Path(__file__).parent
out = base / "companyfacts-archive5-ledger-recovery.json"
if out.exists():
    raise SystemExit("Refusing repeat ledger mutation")
observed = json.loads((base / "companyfacts-archive5-stop-inspection.json").read_text())
run_id = "6400b3c2-f0d1-4f47-bcf0-95aadd9241de"
assert observed["attempt_rows"]["rows"] == [[run_id, 2803031, 778, "0001255474", "0001329919"]]
assert observed["dataset"]["rows"][0][:3] == [run_id, "sec_company_facts", "running"]
assert observed["activation"]["rows"][0][:3] == ["companyfacts_load", "activation-companyfacts-archive5", "running"]
now = dt.datetime.now(dt.UTC)
reason = (
    "User requested stop; controller terminated the verified owned archive5 job and "
    "confirmed its descendants exited. Explicit user continuation authorized recovery. "
    "2,803,031 committed attempt rows across 778 newly processed CIKs retained; "
    "1,110 empty and 39 unavailable receipts, no source errors. "
    "5,792 ancestor loaded receipts had been verified and reused. "
    "Counts include replacements, not net additions. finished_at records operator "
    "ledger recovery time. See companyfacts-archive5-stop-inspection.json."
)
with duckdb.connect("C:/atx/atx-db/data/warehouse.duckdb", config={
        "memory_limit": "1GB", "threads": "1", "preserve_insertion_order": "false"}) as con:
    con.execute("SET TimeZone='UTC'")
    con.execute("SET max_temp_directory_size='2GB'")
    con.execute("BEGIN")
    try:
        a = con.execute(
            "UPDATE activation_stage_runs SET status='failed',finished_at=?,rows=2803031,error=? "
            "WHERE run_id='activation-companyfacts-archive5' AND stage='companyfacts_load' "
            "AND status='running' AND finished_at IS NULL RETURNING stage,run_id,status,finished_at,rows",
            [now, reason],
        ).fetchall()
        b = con.execute(
            "UPDATE dataset_runs SET status='failed',finished_at=?,rows_loaded=2803031,error_message=? "
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
