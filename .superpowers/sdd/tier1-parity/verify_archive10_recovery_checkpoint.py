"""Verify durable archive10 recovery and retry checkpoint without ledger edits."""
import datetime as dt
import json
from pathlib import Path

import duckdb

base = Path(__file__).parent
out = base / 'companyfacts-archive10-recovery-checkpoint-verification.json'
if out.exists():
    raise SystemExit('Refusing repeated checkpoint verification output')
recovery = json.loads((base / 'companyfacts-archive10-headroom-recovery.json').read_text())
assert recovery['committed'] and recovery['checkpoint'] == 'pending'
run_uuid = recovery['actual_dataset_run_id']
assert run_uuid == 'ade90629-8e06-4186-9ea7-565cbd05e285'
result = {'observed_at_utc': dt.datetime.now(dt.UTC).isoformat(),
          'actual_dataset_run_id': run_uuid, 'ledger_mutations': False}
with duckdb.connect('C:/atx/atx-db/data/warehouse.duckdb', config={
    'memory_limit': '1GB', 'threads': '1', 'preserve_insertion_order': 'false'
}) as con:
    con.execute("SET max_temp_directory_size='2GB'")
    a = con.execute("""SELECT status,rows,finished_at FROM activation_stage_runs
        WHERE run_id='activation-companyfacts-archive10' AND stage='companyfacts_load'""").fetchall()
    b = con.execute("""SELECT status,rows_loaded,finished_at FROM dataset_runs
        WHERE run_id=? AND dataset_id='sec_company_facts'""", [run_uuid]).fetchall()
    assert len(a) == len(b) == 1
    assert a[0][0] == b[0][0] == 'failed'
    assert a[0][1] == b[0][1] == recovery['attempt'][0] == 1753504
    assert a[0][2] is not None and b[0][2] is not None
    totals = con.execute("""SELECT (SELECT count(*) FROM sec_company_facts),
        (SELECT count(*) FROM fundamental_points)""").fetchone()
    assert totals == (47906807,47906807)
    result.update(activation=a,dataset=b,retained_facts_points=totals,checkpoint='started')
    with out.open('x',encoding='utf-8') as handle:
        json.dump(result,handle,indent=2,default=str)
    print(json.dumps(result,default=str),flush=True)
    con.execute('CHECKPOINT')
    result['checkpoint'] = 'passed'
    result['finished_at_utc'] = dt.datetime.now(dt.UTC).isoformat()
    out.write_text(json.dumps(result,indent=2,default=str)+'\n',encoding='utf-8')
print(json.dumps(result,default=str),flush=True)
