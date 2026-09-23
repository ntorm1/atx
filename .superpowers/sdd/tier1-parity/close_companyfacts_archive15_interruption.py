"""Close only archive15 stale ledgers after original workers and tool handle disappeared."""
import datetime as dt
import json
from pathlib import Path

import duckdb

base = Path(__file__).parent
out = base / 'companyfacts-archive15-interruption-recovery.json'
if out.exists():
    raise SystemExit('Refusing repeat archive15 recovery')
evidence = json.loads((base / 'activation-companyfacts-archive15-process-check.json').read_text(encoding='utf-8-sig'))
guard = json.loads((base / 'activation-companyfacts-archive15-memory.json').read_text())
assert evidence['run_id'] == 'activation-companyfacts-archive15'
assert evidence['matching_process_count'] == evidence['original_pid_count'] == 0
assert evidence['tool_handle_missing'] is True
assert guard['status'] == 'running'  # Stale receipt; process absence is the stop evidence.
assert 'activation-companyfacts-archive15' in guard['command']
now = dt.datetime.now(dt.UTC)
checked = dt.datetime.fromisoformat(evidence['observed_at_utc'].replace('Z', '+00:00'))
assert dt.timedelta(0) <= now - checked < dt.timedelta(minutes=5)
result = {'observed_at_utc': now.isoformat(), 'process_check': evidence,
          'guard_status': guard['status'], 'guard_last_observed_headroom': guard['headroom'],
          'termination_cause': 'original workers and tool handle absent; interruption cause unproven'}
with duckdb.connect('C:/atx/atx-db/data/warehouse.duckdb', config={
    'memory_limit': '1GB', 'threads': '1', 'preserve_insertion_order': 'false'
}) as con:
    con.execute("SET TimeZone='UTC'")
    con.execute("SET max_temp_directory_size='2GB'")
    activation = con.execute("""
        SELECT stage,run_id,status,started_at,finished_at,rows
        FROM activation_stage_runs
        WHERE run_id='activation-companyfacts-archive15' AND stage='companyfacts_load'
    """).fetchall()
    dataset = con.execute("""
        SELECT run_id,dataset_id,status,started_at,finished_at,rows_loaded
        FROM dataset_runs
        WHERE dataset_id='sec_company_facts' AND json_valid(params_json)
          AND json_extract_string(params_json,'$.run_id')='activation-companyfacts-archive15-companyfacts'
    """).fetchall()
    assert len(activation) == len(dataset) == 1
    assert activation[0][2] == dataset[0][2] == 'running'
    assert activation[0][4] is None and dataset[0][4] is None
    run_uuid = dataset[0][0]
    attempt = con.execute("""
        SELECT count(*),count(DISTINCT cik),min(cik),max(cik)
        FROM sec_company_facts WHERE run_id=?
    """, [run_uuid]).fetchone()
    assert attempt is not None
    totals = con.execute("""
        SELECT (SELECT count(*) FROM sec_company_facts),
               (SELECT count(*) FROM fundamental_points),
               (SELECT count(*) FROM equity_daily_bars),
               (SELECT count(*) FROM custom_features_daily),
               (SELECT max(version) FROM schema_migrations)
    """).fetchone()
    outcomes = con.execute("""
        SELECT status,count(*),sum(try_cast(json_extract_string(metadata_json,'$.rows') AS BIGINT))
        FROM raw_source_files WHERE json_valid(metadata_json)
          AND json_extract_string(metadata_json,'$.run_id')=?
        GROUP BY status ORDER BY status
    """, [run_uuid]).fetchall()
    result.update(actual_dataset_run_id=run_uuid, activation_before=activation,
                  dataset_before=dataset, retained_totals_columns=['facts','points','prices','custom_features','schema_version'],
                  retained_totals=totals, attempt_columns=['rows','ciks','first_cik','last_cik'],
                  attempt=attempt, source_outcomes_columns=['status','members','reported_rows'],
                  source_outcomes=outcomes)
    reason = ('Archive15 original workers and tool handle are absent. Last guard receipt is stale-running: '
              f"physical_free_gb={guard['headroom']['physical_free_gb']}, "
              f"commit_free_gb={guard['headroom']['commit_free_gb']}. "
              'No host-stop or source-failure cause is asserted; original/other warehouse guard workers are absent. '
              f'{attempt[0]} committed attempt rows across {attempt[1]} CIKs retained; counts include replacements. '
              'Retained rows were measured during recovery. finished_at is operator ledger recovery time; '
              'see companyfacts-archive15-interruption-recovery.json.')
    con.execute('BEGIN')
    try:
        a = con.execute("""
            UPDATE activation_stage_runs SET status='failed',finished_at=?,rows=?,error=?
            WHERE run_id='activation-companyfacts-archive15' AND stage='companyfacts_load'
              AND status='running' AND finished_at IS NULL
            RETURNING stage,run_id,status,finished_at,rows
        """, [now, attempt[0], reason]).fetchall()
        b = con.execute("""
            UPDATE dataset_runs SET status='failed',finished_at=?,rows_loaded=?,error_message=?
            WHERE run_id=? AND dataset_id='sec_company_facts' AND status='running' AND finished_at IS NULL
            RETURNING run_id,dataset_id,status,finished_at,rows_loaded
        """, [now, attempt[0], reason, run_uuid]).fetchall()
        assert len(a) == len(b) == 1
        con.execute('COMMIT')
    except BaseException:
        con.execute('ROLLBACK')
        raise
    result.update(activation_after=a, dataset_after=b, committed=True, checkpoint='pending')
    with out.open('x', encoding='utf-8') as handle:
        json.dump(result, handle, indent=2, default=str)
    con.execute('CHECKPOINT')
    result['checkpoint'] = 'passed'
    out.write_text(json.dumps(result, indent=2, default=str) + '\n', encoding='utf-8')
print(json.dumps(result, default=str))
