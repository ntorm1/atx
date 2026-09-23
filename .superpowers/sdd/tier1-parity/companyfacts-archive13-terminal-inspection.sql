-- Read only after the archive13 guard/process tree is terminal.
-- Select individual provenance fields; never expose dataset params_json.
WITH attempt AS (
    SELECT run_id,dataset_id,status,started_at,finished_at,rows_loaded
    FROM dataset_runs
    WHERE dataset_id='sec_company_facts' AND json_valid(params_json)
      AND json_extract_string(params_json,'$.run_id')='activation-companyfacts-archive13-companyfacts'
), stage AS (
    SELECT stage,run_id,status,started_at,finished_at,rows
    FROM activation_stage_runs
    WHERE stage='companyfacts_load' AND run_id='activation-companyfacts-archive13'
), attempt_facts AS (
    SELECT f.run_id,count(*) AS committed_attempt_rows,
           count(DISTINCT f.cik) AS committed_attempt_ciks,
           min(f.cik) AS first_cik,max(f.cik) AS last_cik
    FROM sec_company_facts f JOIN attempt a ON a.run_id=f.run_id
    GROUP BY f.run_id
), outcomes AS (
    SELECT a.run_id,r.status,count(*) AS member_receipts,
           sum(try_cast(json_extract_string(r.metadata_json,'$.rows') AS BIGINT)) AS reported_rows
    FROM raw_source_files r JOIN attempt a
      ON json_extract_string(CASE WHEN json_valid(r.metadata_json) THEN r.metadata_json END,'$.run_id')=a.run_id
    GROUP BY a.run_id,r.status
)
SELECT 'activation' AS kind,to_json(s) AS evidence FROM stage s
UNION ALL
SELECT 'dataset',to_json(a) FROM attempt a
UNION ALL
SELECT 'committed_attempt_facts',to_json(f) FROM attempt_facts f
UNION ALL
SELECT 'attempt_source_outcomes',to_json(o) FROM outcomes o
UNION ALL
SELECT 'retained_totals',to_json(struct_pack(
    facts:=(SELECT count(*) FROM sec_company_facts),
    points:=(SELECT count(*) FROM fundamental_points),
    prices:=(SELECT count(*) FROM equity_daily_bars),
    custom_features:=(SELECT count(*) FROM custom_features_daily),
    schema_version:=(SELECT max(version) FROM schema_migrations)
))
ORDER BY kind,evidence;
