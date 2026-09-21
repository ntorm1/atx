"""Bounded read-only inventory of materializations relevant to FC1 activation."""
import datetime as dt
import json
from pathlib import Path
import duckdb

out = Path(__file__).with_name('fundamental-clock-legacy-input-inventory.json')
if out.exists():
    raise SystemExit('Refusing report overwrite')
tables = ['fundamental_xbrl_metric', 'shares_outstanding_history', 'est_actual',
          'fundamental_fact_revisions', 'fundamental_statement_points',
          'fundamental_standardized', 'derived_metrics', 'market_daily_metrics']
result = {'observed_at_utc': dt.datetime.now(dt.UTC).isoformat(), 'read_only': True, 'tables': {}}
with duckdb.connect('C:/atx/atx-db/data/warehouse.duckdb', read_only=True, config={
        'memory_limit': '1GB', 'threads': '1', 'preserve_insertion_order': 'false'}) as con:
    existing = {r[0] for r in con.execute('SELECT table_name FROM information_schema.tables WHERE table_schema=\'main\'').fetchall()}
    for table in tables:
        result['tables'][table] = con.execute(f'SELECT count(*) FROM "{table}"').fetchone()[0] if table in existing else 'not present'
with out.open('x', encoding='utf-8') as handle:
    json.dump(result, handle, indent=2)
print(json.dumps(result))
