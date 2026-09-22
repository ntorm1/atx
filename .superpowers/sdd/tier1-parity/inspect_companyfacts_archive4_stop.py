"""Bounded read-only evidence after archive4's confirmed memory guard stop."""
import datetime as dt
import json
from pathlib import Path

import duckdb

RUN = 'activation-companyfacts-archive4'
SCOPE = "json_valid(r.params_json) AND json_extract_string(r.params_json,'$.run_id')='activation-companyfacts-archive4-companyfacts'"
queries = {
    'activation': "SELECT stage,run_id,status,started_at,finished_at,rows,error FROM activation_stage_runs WHERE run_id='activation-companyfacts-archive4'",
    'dataset': "SELECT run_id,dataset_id,status,started_at,finished_at,rows_loaded,error_message FROM dataset_runs r WHERE " + SCOPE,
    'retained_totals': 'SELECT (SELECT count(*) FROM sec_company_facts) AS facts,(SELECT count(*) FROM fundamental_points) AS points,(SELECT count(*) FROM equity_daily_bars) AS prices,(SELECT count(*) FROM custom_features_daily) AS custom_features',
    'attempt_rows': "SELECT f.run_id,count(*) AS rows,count(DISTINCT cik) AS ciks,min(cik) AS first_cik,max(cik) AS last_cik FROM sec_company_facts f JOIN dataset_runs r USING(run_id) WHERE " + SCOPE + " GROUP BY f.run_id",
    'source_outcomes': "SELECT s.status,count(*) AS members,sum(try_cast(json_extract_string(s.metadata_json,'$.rows') AS BIGINT)) AS reported_rows,min(json_extract_string(s.metadata_json,'$.cik')) AS first_cik,max(json_extract_string(s.metadata_json,'$.cik')) AS last_cik FROM raw_source_files s JOIN dataset_runs r ON json_extract_string(s.metadata_json,'$.run_id')=r.run_id WHERE " + SCOPE + " GROUP BY s.status ORDER BY s.status",
    'migrations': 'SELECT max(version) AS version FROM schema_migrations',
}
result = {'observed_at_utc': dt.datetime.now(dt.UTC).isoformat(), 'read_only': True, 'run': RUN, 'duckdb_version': duckdb.__version__}
out = Path(__file__).with_name('companyfacts-archive4-stop-inspection.json')
if out.exists():
    raise SystemExit('Refusing report overwrite')
with duckdb.connect('C:/atx/atx-db/data/warehouse.duckdb', read_only=True,
                    config={'memory_limit': '1GB', 'threads': '1', 'preserve_insertion_order': 'false'}) as con:
    con.execute("SET TimeZone='UTC'")
    con.execute("SET max_temp_directory_size='2GB'")
    for name, sql in queries.items():
        cur = con.execute(sql)
        rows = cur.fetchmany(101)
        if len(rows) > 100:
            raise RuntimeError(f'{name} exceeded bounded report rows')
        result[name] = {'columns': [d[0] for d in cur.description], 'rows': rows}
with out.open('x', encoding='utf-8') as handle:
    json.dump(result, handle, indent=2, default=str)
print(json.dumps(result, indent=2, default=str))
