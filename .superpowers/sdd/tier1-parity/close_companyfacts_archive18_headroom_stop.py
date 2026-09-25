"""Close only archive18's ledgers after its host-headroom guard stopped."""
import datetime as dt
import json
import uuid
from pathlib import Path

import duckdb


def _sql_text(value: str) -> str:
    if not isinstance(value, str) or '\x00' in value:
        raise ValueError('SQL text requires a string without NUL')
    return "'" + value.replace("'", "''") + "'"


def _sql_uuid(value: str) -> str:
    if not isinstance(value, str) or str(uuid.UUID(value)) != value:
        raise ValueError('Dataset run ID must be a canonical UUID string')
    return _sql_text(value)


def _sql_count(value: int) -> str:
    if type(value) is not int or value < 0:
        raise ValueError('Attempt rows must be a nonnegative builtin integer')
    return str(value)


def _sql_utc_timestamp(value: dt.datetime) -> str:
    if not isinstance(value, dt.datetime) or value.utcoffset() is None:
        raise ValueError('Recovery timestamp must be timezone aware')
    utc = value.astimezone(dt.UTC).isoformat(sep=' ', timespec='microseconds')
    return 'TIMESTAMPTZ ' + _sql_text(utc)


def _close_ledgers(con, *, now: dt.datetime, run_uuid: str, rows: int, reason: str):
    # Typed literals avoid DuckDB's first scalar-bind pandas/NumPy import.
    # Every embedded value is validated or escaped before beginning mutation.
    timestamp_sql = _sql_utc_timestamp(now)
    run_sql = _sql_uuid(run_uuid)
    rows_sql = _sql_count(rows)
    reason_sql = _sql_text(reason)
    con.execute('BEGIN')
    try:
        a = con.execute(f"""
            UPDATE activation_stage_runs SET status='failed',finished_at={timestamp_sql},rows={rows_sql},error={reason_sql}
            WHERE run_id='activation-companyfacts-archive18' AND stage='companyfacts_load'
              AND status='running' AND finished_at IS NULL
            RETURNING stage,run_id,status,finished_at,rows
        """).fetchall()
        b = con.execute(f"""
            UPDATE dataset_runs SET status='failed',finished_at={timestamp_sql},rows_loaded={rows_sql},error_message={reason_sql}
            WHERE run_id={run_sql} AND dataset_id='sec_company_facts' AND status='running' AND finished_at IS NULL
            RETURNING run_id,dataset_id,status,finished_at,rows_loaded
        """).fetchall()
        assert len(a) == len(b) == 1
        con.execute('COMMIT')
    except BaseException:
        con.execute('ROLLBACK')
        raise
    return a, b


def main() -> None:
    base = Path(__file__).parent
    out = base / 'companyfacts-archive18-headroom-recovery.json'
    if out.exists():
        raise SystemExit('Refusing repeat archive18 recovery')
    evidence = json.loads((base / 'activation-companyfacts-archive18-process-check.json').read_text(encoding='utf-8-sig'))
    guard = json.loads((base / 'activation-companyfacts-archive18-memory.json').read_text())
    assert evidence['run_id'] == 'activation-companyfacts-archive18'
    assert evidence['matching_process_count'] == evidence['original_pid_count'] == 0
    assert evidence['session_exit_code'] == 1
    assert guard['status'] == 'stopped_low_headroom'
    assert 'activation-companyfacts-archive18' in guard['command']
    now = dt.datetime.now(dt.UTC)
    checked = dt.datetime.fromisoformat(evidence['observed_at_utc'].replace('Z', '+00:00'))
    assert dt.timedelta(0) <= now - checked < dt.timedelta(minutes=5)
    result = {'observed_at_utc': now.isoformat(), 'process_check': evidence,
              'guard_status': guard['status'], 'guard_stop_headroom': guard['headroom'],
              'termination_cause': 'host headroom guard; not a recorded source failure'}
    with duckdb.connect('C:/atx/atx-db/data/warehouse.duckdb', config={
        'memory_limit': '256MB', 'threads': '1', 'preserve_insertion_order': 'false'
    }) as con:
        con.execute("SET TimeZone='UTC'")
        con.execute("SET max_temp_directory_size='2GB'")
        activation = con.execute("""
            SELECT stage,run_id,status,started_at,finished_at,rows
            FROM activation_stage_runs
            WHERE run_id='activation-companyfacts-archive18' AND stage='companyfacts_load'
        """).fetchall()
        dataset = con.execute("""
            SELECT run_id,dataset_id,status,started_at,finished_at,rows_loaded
            FROM dataset_runs
            WHERE dataset_id='sec_company_facts' AND json_valid(params_json)
              AND json_extract_string(params_json,'$.run_id')='activation-companyfacts-archive18-companyfacts'
        """).fetchall()
        assert len(activation) == len(dataset) == 1
        assert activation[0][2] == dataset[0][2] == 'running'
        assert activation[0][4] is None and dataset[0][4] is None
        run_uuid = dataset[0][0]
        run_sql = _sql_uuid(run_uuid)
        attempt = con.execute(f"""
            SELECT count(*),count(DISTINCT cik),min(cik),max(cik)
            FROM sec_company_facts WHERE run_id={run_sql}
        """).fetchone()
        assert attempt is not None
        totals = con.execute("""
            SELECT (SELECT count(*) FROM sec_company_facts),
                   (SELECT count(*) FROM fundamental_points),
                   (SELECT count(*) FROM equity_daily_bars),
                   (SELECT count(*) FROM custom_features_daily),
                   (SELECT max(version) FROM schema_migrations)
        """).fetchone()
        outcomes = con.execute(f"""
            SELECT status,count(*),sum(try_cast(json_extract_string(metadata_json,'$.rows') AS BIGINT))
            FROM raw_source_files WHERE json_valid(metadata_json)
              AND json_extract_string(metadata_json,'$.run_id')={run_sql}
            GROUP BY status ORDER BY status
        """).fetchall()
        result.update(actual_dataset_run_id=run_uuid, activation_before=activation,
                      dataset_before=dataset, retained_totals_columns=['facts','points','prices','custom_features','schema_version'],
                      retained_totals=totals, attempt_columns=['rows','ciks','first_cik','last_cik'],
                      attempt=attempt, source_outcomes_columns=['status','members','reported_rows'],
                      source_outcomes=outcomes)
        reason = ('Host headroom guard stopped the archive18 job: '
                  f"physical_free_gb={guard['headroom']['physical_free_gb']}, "
                  f"commit_free_gb={guard['headroom']['commit_free_gb']}. "
                  'Original session is terminal and all original/other warehouse guard workers are absent. '
                  f'{attempt[0]} committed attempt rows across {attempt[1]} CIKs retained; counts include replacements. '
                  'No recorded source failure is inferred. finished_at is operator ledger recovery time; '
                  'see companyfacts-archive18-headroom-recovery.json.')
        a, b = _close_ledgers(con, now=now, run_uuid=run_uuid, rows=attempt[0], reason=reason)
        result.update(activation_after=a, dataset_after=b, committed=True, checkpoint='pending')
        with out.open('x', encoding='utf-8') as handle:
            json.dump(result, handle, indent=2, default=str)
        con.execute('CHECKPOINT')
        result['checkpoint'] = 'passed'
        out.write_text(json.dumps(result, indent=2, default=str) + '\n', encoding='utf-8')
    print(json.dumps(result, default=str))


if __name__ == '__main__':
    main()
