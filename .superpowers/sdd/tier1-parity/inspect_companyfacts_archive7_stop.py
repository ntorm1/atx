"""Bounded read-only evidence after archive7's confirmed host-memory guard stop."""
import datetime as dt
import json
from pathlib import Path

import duckdb

RUN = "activation-companyfacts-archive7"
base = Path(__file__).parent
guard = json.loads((base / "activation-companyfacts-archive7-memory.json").read_text())
assert guard["status"] == "stopped_low_headroom"
SCOPE = "json_valid(r.params_json) AND json_extract_string(r.params_json,'$.run_id')='activation-companyfacts-archive7-companyfacts'"
queries = {
    "activation": "SELECT stage,run_id,status,started_at,finished_at,rows,error FROM activation_stage_runs WHERE run_id='activation-companyfacts-archive7'",
    "dataset": "SELECT run_id,dataset_id,status,started_at,finished_at,rows_loaded,error_message FROM dataset_runs r WHERE " + SCOPE,
    "retained_totals": "SELECT (SELECT count(*) FROM sec_company_facts) AS facts,(SELECT count(*) FROM fundamental_points) AS points,(SELECT count(*) FROM equity_daily_bars) AS prices,(SELECT count(*) FROM custom_features_daily) AS custom_features",
    "attempt_rows": "SELECT f.run_id,count(*) AS rows,count(DISTINCT cik) AS ciks,min(cik) AS first_cik,max(cik) AS last_cik FROM sec_company_facts f JOIN dataset_runs r USING(run_id) WHERE " + SCOPE + " GROUP BY f.run_id",
    "source_outcomes": "SELECT s.status,count(*) AS members,sum(try_cast(json_extract_string(s.metadata_json,'$.rows') AS BIGINT)) AS reported_rows,min(json_extract_string(s.metadata_json,'$.cik')) AS first_cik,max(json_extract_string(s.metadata_json,'$.cik')) AS last_cik FROM raw_source_files s JOIN dataset_runs r ON json_extract_string(s.metadata_json,'$.run_id')=r.run_id WHERE " + SCOPE + " GROUP BY s.status ORDER BY s.status",
    "migrations": "SELECT max(version) AS version FROM schema_migrations",
    "cvx_raw_reported_quarter_eps": """
        WITH eligible AS (
            SELECT source,security_id,cik,concept,unit,period_start,period_end,
                   value,filed_date,accession_number,
                   greatest(available_at,CAST(filed_date AS TIMESTAMP)+INTERVAL '46 hours') AS effective_available_at
            FROM sec_company_facts
            WHERE cik='0000093410' AND concept='EarningsPerShareDiluted'
              AND unit='USD/shares' AND period_end IN
                  (DATE '2026-06-30',DATE '2026-03-31',DATE '2025-12-31',
                   DATE '2025-06-30',DATE '2025-03-31',DATE '2024-12-31')
              AND date_diff('day',period_start,period_end) BETWEEN 70 AND 115
        )
        SELECT * FROM eligible WHERE effective_available_at<=TIMESTAMP '2026-09-20 22:00:00'
        QUALIFY row_number() OVER(PARTITION BY source,security_id,concept,unit,period_start,period_end
            ORDER BY effective_available_at DESC,filed_date DESC,accession_number DESC)=1
        ORDER BY period_end DESC,security_id,source
    """,
    "cvx_raw_2025_annual_and_ytd_eps": """
        WITH eligible AS (
            SELECT source,security_id,cik,concept,unit,period_start,period_end,
                   value,filed_date,accession_number,
                   greatest(available_at,CAST(filed_date AS TIMESTAMP)+INTERVAL '46 hours') AS effective_available_at
            FROM sec_company_facts
            WHERE cik='0000093410' AND concept='EarningsPerShareDiluted'
              AND unit='USD/shares' AND period_start=DATE '2025-01-01'
              AND period_end IN (DATE '2025-09-30',DATE '2025-12-31')
        )
        SELECT * FROM eligible WHERE effective_available_at<=TIMESTAMP '2026-09-20 22:00:00'
        QUALIFY row_number() OVER(PARTITION BY source,security_id,concept,unit,period_start,period_end
            ORDER BY effective_available_at DESC,filed_date DESC,accession_number DESC)=1
        ORDER BY period_end DESC,security_id,source
    """,
    "cvx_downstream_row_counts": """
        SELECT 'fundamental_statement_points' AS relation,count(*) AS rows
          FROM fundamental_statement_points WHERE cik='0000093410' AND canonical_metric='eps_diluted'
        UNION ALL
        SELECT 'fundamental_standardized',count(*) FROM fundamental_standardized
          WHERE cik='0000093410' AND canonical_code='eps_diluted' AND basis='quarterly'
        UNION ALL
        SELECT 'derived_metric_values',count(*) FROM derived_metric_values
          WHERE metric_code='eps_diluted_q_growth_yoy' AND security_id IN
              (SELECT DISTINCT security_id FROM sec_company_facts WHERE cik='0000093410')
    """,
    "production_table_counts": """
        SELECT 'fundamental_statement_points' AS relation,count(*) AS rows FROM fundamental_statement_points
        UNION ALL SELECT 'fundamental_standardized',count(*) FROM fundamental_standardized
        UNION ALL SELECT 'derived_metric_values',count(*) FROM derived_metric_values
    """,
}
result = {"observed_at_utc": dt.datetime.now(dt.UTC).isoformat(), "read_only": True,
          "run": RUN, "duckdb_version": duckdb.__version__,
          "stop_reason": "Guard terminated owned job after host physical headroom fell below1.5GiB; session terminal and descendants absent.",
          "stop_headroom": guard["headroom"]}
out = base / "companyfacts-archive7-stop-inspection.json"
if out.exists():
    raise SystemExit("Refusing report overwrite")
with duckdb.connect("C:/atx/atx-db/data/warehouse.duckdb", read_only=True,
                    config={"memory_limit": "1GB", "threads": "1", "preserve_insertion_order": "false"}) as con:
    con.execute("SET TimeZone='UTC'")
    con.execute("SET max_temp_directory_size='2GB'")
    for name, sql in queries.items():
        cur = con.execute(sql)
        rows = cur.fetchmany(101)
        if len(rows) > 100:
            raise RuntimeError(f"{name} exceeded bounded report rows")
        result[name] = {"columns": [d[0] for d in cur.description], "rows": rows}
with out.open("x", encoding="utf-8") as handle:
    json.dump(result, handle, indent=2, default=str)
print(json.dumps(result, indent=2, default=str))
