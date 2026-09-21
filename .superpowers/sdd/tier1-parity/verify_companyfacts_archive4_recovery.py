"""Verify committed recovery after a 512MB checkpoint failed; checkpoint at normal 1GB."""
import datetime as dt
import json
from pathlib import Path
import duckdb

out = Path(__file__).with_name('companyfacts-archive4-ledger-recovery-verified.json')
if out.exists():
    raise SystemExit('Refusing report overwrite')
with duckdb.connect('C:/atx/atx-db/data/warehouse.duckdb', config={
        'memory_limit': '1GB', 'threads': '1', 'preserve_insertion_order': 'false'}) as con:
    con.execute("SET TimeZone='UTC'")
    a = con.execute("SELECT stage,run_id,status,finished_at,rows,error FROM activation_stage_runs WHERE run_id='activation-companyfacts-archive4' AND stage='companyfacts_load'").fetchall()
    b = con.execute("SELECT run_id,dataset_id,status,finished_at,rows_loaded,error_message FROM dataset_runs WHERE run_id='c7c21cd1-c7c4-4de8-af64-4f2877ef6f59' AND dataset_id='sec_company_facts'").fetchall()
    assert len(a) == len(b) == 1
    assert a[0][2] == b[0][2] == 'failed'
    assert a[0][3] == b[0][3] and a[0][3] is not None
    assert a[0][4] == b[0][4] == 7999850
    con.execute('CHECKPOINT')
result = {'verified_at_utc': dt.datetime.now(dt.UTC).isoformat(), 'activation': a, 'dataset': b,
          'note': 'Ledger COMMIT survived prior checkpoint failure at 512MB. Verified without a second UPDATE; checkpoint now passed at normal production 1GB/one thread.'}
with out.open('x', encoding='utf-8') as handle:
    json.dump(result, handle, indent=2, default=str)
print(json.dumps(result, default=str))
