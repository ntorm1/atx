"""Close only archive20's ledgers after its host-headroom guard stopped it (stopped_low_headroom).

Mirrors close_companyfacts_archive18_headroom_stop.py (LR2 pattern, commits
1670833b/24cb7a82): typed SQL literals instead of scalar parameter binding (avoids
DuckDB's first-bind pandas/NumPy import), 256MB/one thread, refuse-repeat receipt,
one BEGIN..COMMIT that must touch exactly one activation row and one dataset row,
then CHECKPOINT. Same stop class as archive18 (guard TerminateJobObject 137 on
commit_free < 3GiB); liveness proof from archive20-process-check.ps1.
"""
import datetime as dt
import json
import re
import uuid
from pathlib import Path

import duckdb

CTL = Path(r'C:\atx\.superpowers\sdd\tier1-parity')
RUN = 'activation-companyfacts-archive20'
PREDECESSOR = 'e27c8a4e-2d29-47bb-b657-fc1855ea4cec'
ORIGINAL_CHILD_PID = 14644


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
    timestamp_sql = _sql_utc_timestamp(now)
    run_sql = _sql_uuid(run_uuid)
    rows_sql = _sql_count(rows)
    reason_sql = _sql_text(reason)
    con.execute('BEGIN')
    try:
        a = con.execute(f"""
            UPDATE activation_stage_runs SET status='failed',finished_at={timestamp_sql},rows={rows_sql},error={reason_sql}
            WHERE run_id={_sql_text(RUN)} AND stage='companyfacts_load'
              AND status='running' AND finished_at IS NULL
            RETURNING stage,run_id,status,finished_at,rows
        """).fetchall()
        b = con.execute(f"""
            UPDATE dataset_runs SET status='failed',finished_at={timestamp_sql},rows_loaded={rows_sql},error_message={reason_sql}
            WHERE run_id={run_sql} AND dataset_id='sec_company_facts' AND status='running' AND finished_at IS NULL
            RETURNING run_id,dataset_id,status,finished_at,rows_loaded
        """).fetchall()
        assert len(a) == len(b) == 1, (a, b)
        con.execute('COMMIT')
    except BaseException:
        con.execute('ROLLBACK')
        raise
    return a, b


def _last_progress(err_path: Path) -> str:
    last = ''
    with err_path.open('r', encoding='utf-8', errors='replace') as handle:
        for line in handle:
            if 'companyfacts processed=' in line:
                last = line.strip()
    return last


def main() -> None:
    out = CTL / 'companyfacts-archive20-headroom-recovery.json'
    if out.exists():
        raise SystemExit('Refusing repeat archive20 recovery')
    evidence = json.loads((CTL / 'activation-companyfacts-archive20-process-check.json').read_text(encoding='utf-8-sig'))
    guard = json.loads((CTL / 'activation-companyfacts-archive20-memory.json').read_text(encoding='utf-8'))
    assert evidence['run_id'] == RUN
    assert evidence['matching_process_count'] == evidence['original_pid_count'] == 0
    assert ORIGINAL_CHILD_PID in evidence['original_pids']
    assert evidence['session_exit_code'] == 137
    assert evidence['warehouse_exclusive_open_ok'] is True
    assert guard['status'] == 'stopped_low_headroom' and guard['child_pid'] == ORIGINAL_CHILD_PID
    assert RUN in guard['command'] and PREDECESSOR in guard['command']
    now = dt.datetime.now(dt.UTC)
    checked = dt.datetime.fromisoformat(evidence['observed_at_utc'].replace('Z', '+00:00'))
    assert dt.timedelta(0) <= now - checked < dt.timedelta(minutes=5), (now, checked)
    last_progress = _last_progress(CTL / 'activation-companyfacts-archive20.err')
    match = re.search(r'processed=(\d+) total=(\d+) loaded=(\d+) empty=(\d+) unavailable=(\d+) failed=(\d+) rows=(\d+)',
                      last_progress)
    assert match is not None, last_progress
    progress = dict(zip(('processed', 'total', 'loaded', 'empty', 'unavailable', 'failed', 'rows'),
                        (int(g) for g in match.groups())))
    result = {'observed_at_utc': now.isoformat(), 'process_check': evidence,
              'guard_status': guard['status'], 'guard_child_pid': guard['child_pid'],
              'guard_stop_headroom': guard['headroom'], 'last_progress_line': last_progress,
              'last_progress': progress,
              'termination_cause': ('host headroom guard (commit_free_gb < 3) while concurrent '
                                    'other-lane pytest/bench processes held ~3.9GB private; '
                                    'not a recorded source failure')}
    with duckdb.connect('C:/atx/atx-db/data/warehouse.duckdb', config={
        'memory_limit': '256MB', 'threads': '1', 'preserve_insertion_order': 'false'
    }) as con:
        con.execute("SET TimeZone='UTC'")
        con.execute("SET max_temp_directory_size='2GB'")
        activation_all = con.execute(f"""
            SELECT stage,run_id,status,started_at,finished_at,rows
            FROM activation_stage_runs WHERE run_id={_sql_text(RUN)}
        """).fetchall()
        dataset = con.execute(f"""
            SELECT run_id,dataset_id,status,started_at,finished_at,rows_loaded,
                   json_extract_string(params_json,'$.resume_from_run_id')
            FROM dataset_runs
            WHERE dataset_id='sec_company_facts' AND json_valid(params_json)
              AND json_extract_string(params_json,'$.run_id')={_sql_text(RUN + '-companyfacts')}
        """).fetchall()
        activation = [row for row in activation_all if row[0] == 'companyfacts_load']
        assert len(activation_all) == len(activation) == len(dataset) == 1, (activation_all, dataset)
        assert activation[0][2] == dataset[0][2] == 'running'
        assert activation[0][4] is None and dataset[0][4] is None
        assert dataset[0][6] == PREDECESSOR, dataset[0][6]
        run_uuid = dataset[0][0]
        run_sql = _sql_uuid(run_uuid)
        attempt = con.execute(f"""
            SELECT count(*),count(DISTINCT cik),min(cik),max(cik),min(period_end),max(period_end)
            FROM sec_company_facts WHERE run_id={run_sql}
        """).fetchone()
        attempt_points = con.execute(f"""
            SELECT count(*),count(DISTINCT security_id),min(period_end),max(period_end)
            FROM fundamental_points WHERE run_id={run_sql}
        """).fetchone()
        assert attempt is not None and attempt_points is not None
        totals = con.execute("""
            SELECT (SELECT count(*) FROM sec_company_facts),
                   (SELECT count(*) FROM fundamental_points),
                   (SELECT count(*) FROM equity_daily_bars),
                   (SELECT count(*) FROM custom_features_daily),
                   (SELECT max(version) FROM schema_migrations)
        """).fetchone()
        outcomes = con.execute(f"""
            SELECT status,count(*),sum(try_cast(json_extract_string(metadata_json,'$.rows') AS BIGINT)),
                   min(json_extract_string(metadata_json,'$.cik')),max(json_extract_string(metadata_json,'$.cik')),
                   min(fetched_at),max(fetched_at)
            FROM raw_source_files WHERE dataset_id='sec_company_facts' AND json_valid(metadata_json)
              AND json_extract_string(metadata_json,'$.run_id')={run_sql}
            GROUP BY status ORDER BY status
        """).fetchall()
        result.update(actual_dataset_run_id=run_uuid, resume_from_run_id=dataset[0][6],
                      activation_before=activation, dataset_before=[dataset[0][:6]],
                      retained_totals_columns=['facts', 'points', 'prices', 'custom_features', 'schema_version'],
                      retained_totals=totals,
                      attempt_columns=['rows', 'ciks', 'first_cik', 'last_cik', 'min_period_end', 'max_period_end'],
                      attempt=attempt,
                      attempt_points_columns=['rows', 'security_ids', 'min_period_end', 'max_period_end'],
                      attempt_points=attempt_points,
                      source_outcomes_columns=['status', 'members', 'reported_rows', 'first_cik', 'last_cik',
                                               'first_fetched_at', 'last_fetched_at'],
                      source_outcomes=outcomes)
        receipts = ', '.join(f'{row[0]}={row[1]}' for row in outcomes) or 'none'
        reason = ('Host headroom guard stopped the archive20 job: '
                  f"physical_free_gb={guard['headroom']['physical_free_gb']}, "
                  f"commit_free_gb={guard['headroom']['commit_free_gb']} (guard exit 137); "
                  f"last progress {progress['processed']}/{progress['total']} "
                  f"loaded={progress['loaded']} empty={progress['empty']} unavailable={progress['unavailable']} "
                  f"failed={progress['failed']} rows={progress['rows']}. "
                  'Original guard/child and all warehouse workers are absent; exclusive open verified. '
                  f'{attempt[0]} committed attempt fact rows across {attempt[1]} CIKs retained; '
                  f'{attempt_points[0]} attempt point rows; source receipts: {receipts}. '
                  'No recorded source failure is inferred. finished_at is operator ledger recovery time; '
                  'see companyfacts-archive20-headroom-recovery.json.')
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
