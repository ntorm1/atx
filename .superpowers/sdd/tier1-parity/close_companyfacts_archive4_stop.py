"""Close only the confirmed guard-interrupted archive4 ledgers."""
import datetime as dt
import json
from pathlib import Path

import duckdb

base = Path(__file__).parent
out = base / 'companyfacts-archive4-ledger-recovery.json'
if out.exists():
    raise SystemExit('Refusing repeat ledger mutation')
observed = json.loads((base / 'companyfacts-archive4-stop-inspection.json').read_text())
run_id = 'c7c21cd1-c7c4-4de8-af64-4f2877ef6f59'
assert observed['attempt_rows']['rows'] == [[run_id, 7999850, 2042, '0001034054', '0001254699']]
guard = json.loads((base / 'activation-companyfacts-archive4-memory.json').read_text())
assert guard['status'] == 'stopped_low_headroom'
now = dt.datetime.now(dt.UTC)
reason = ('Owned job terminated by memory guard: host commit headroom below 3 GiB; '
          'receipt observed terminal by 2026-09-21T00:29:18Z. '
          '7,999,850 committed attempt rows across 2,042 newly processed CIKs retained; '
          '1,010 empty and 34 unavailable receipts, no source errors. '
          '3,750 ancestor loaded receipts had been verified and reused. '
          'Counts include replacements, not net additions. '
          'finished_at records operator ledger recovery time. '
          'See companyfacts-archive4-stop-inspection.json and guard receipt.')
with duckdb.connect('C:/atx/atx-db/data/warehouse.duckdb',
                    config={'memory_limit': '512MB', 'threads': '1'}) as con:
    con.execute("SET TimeZone='UTC'")
    con.execute('BEGIN')
    try:
        a = con.execute("UPDATE activation_stage_runs SET status='failed',finished_at=?,rows=7999850,error=? WHERE run_id='activation-companyfacts-archive4' AND stage='companyfacts_load' AND status='running' AND finished_at IS NULL RETURNING stage,run_id,status,finished_at,rows", [now, reason]).fetchall()
        b = con.execute("UPDATE dataset_runs SET status='failed',finished_at=?,rows_loaded=7999850,error_message=? WHERE run_id=? AND dataset_id='sec_company_facts' AND status='running' AND finished_at IS NULL RETURNING run_id,dataset_id,status,finished_at,rows_loaded", [now, reason, run_id]).fetchall()
        assert len(a) == len(b) == 1
        con.execute('COMMIT')
    except BaseException:
        con.execute('ROLLBACK')
        raise
    con.execute('CHECKPOINT')
result = {'recovered_at_utc': now.isoformat(), 'reason': reason, 'activation': a, 'dataset': b}
with out.open('x', encoding='utf-8') as handle:
    json.dump(result, handle, indent=2, default=str)
print(json.dumps(result, default=str))
