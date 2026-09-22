"""Bounded, read-only CVX identity evidence for the user's EPS acceptance case."""
import datetime as dt
import json
from pathlib import Path

import duckdb

out = Path(__file__).with_name("cvx-identity-live-evidence.json")
if out.exists():
    raise SystemExit("Refusing evidence overwrite")
ids = """WITH ids AS (
    SELECT security_id FROM securities WHERE primary_symbol='CVX' OR upper(name) LIKE 'CHEVRON%'
    UNION SELECT security_id FROM security_identifier_history
        WHERE (id_type='CIK' AND try_cast(id_value AS BIGINT)=93410)
           OR (id_type IN ('TICKER','SYMBOL') AND id_value='CVX')
) """
queries = {
    "securities": ids + "SELECT security_id,entity_id,issuer_id,primary_symbol,name,source,first_seen_date,last_seen_date FROM securities WHERE security_id IN (SELECT security_id FROM ids) ORDER BY security_id",
    "identifier_history": ids + "SELECT security_id,id_type,id_value,valid_from,valid_to,as_of_date,available_at,source FROM security_identifier_history WHERE security_id IN (SELECT security_id FROM ids) AND id_type IN ('CIK','TICKER','SYMBOL','ENTITY_ID') ORDER BY security_id,id_type,valid_from,available_at",
    "candidates": "SELECT source_dataset_id,source_table,source_key_type,source_key_value,source_security_id,target_security_id,target_id_type,target_id_value,match_method,confidence,candidate_status,as_of_date,available_at FROM identifier_resolution_candidates WHERE try_cast(source_key_value AS BIGINT)=93410 ORDER BY source_table,target_security_id",
    "prices": "SELECT security_id,symbol,count(*) AS rows,min(trade_date) AS first_date,max(trade_date) AS last_date FROM equity_daily_bars WHERE symbol='CVX' GROUP BY security_id,symbol ORDER BY security_id",
}
result = {"observed_at_utc": dt.datetime.now(dt.UTC).isoformat(), "read_only": True,
          "snapshot_cutoff": "2026-09-20T22:00:00Z", "queries": {}}
with duckdb.connect("C:/atx/atx-db/data/warehouse.duckdb", read_only=True,
                    config={"memory_limit": "1GB", "threads": "1", "preserve_insertion_order": "false"}) as con:
    con.execute("SET TimeZone='UTC'")
    con.execute("SET max_temp_directory_size='2GB'")
    for name, sql in queries.items():
        cur = con.execute(sql)
        rows = cur.fetchmany(101)
        if len(rows) > 100:
            raise RuntimeError(f"{name} exceeded bounded output")
        result["queries"][name] = {"columns": [d[0] for d in cur.description], "rows": rows}
with out.open("x", encoding="utf-8") as handle:
    json.dump(result, handle, indent=2, default=str)
print(json.dumps(result, indent=2, default=str))
