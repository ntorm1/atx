"""Close archive13's stale dataset ledger after terminal commit OOM; preserve stage error."""
import datetime as dt
import json
from pathlib import Path

import duckdb

base = Path(__file__).parent
out = base / 'companyfacts-archive13-oom-recovery.json'
if out.exists():
    raise SystemExit('Refusing repeat archive13 recovery')
evidence = json.loads((base / 'activation-companyfacts-archive13-process-check.json').read_text(encoding='utf-8-sig'))
guard = json.loads((base / 'activation-companyfacts-archive13-memory.json').read_text())
assert evidence['run_id'] == 'activation-companyfacts-archive13'
assert evidence['matching_process_count'] == evidence['original_pid_count'] == 0
assert guard['status'] == 'failed' and guard['returncode'] == 1
assert 'activation-companyfacts-archive13' in guard['command']
now = dt.datetime.now(dt.UTC)
checked = dt.datetime.fromisoformat(evidence['observed_at_utc'].replace('Z', '+00:00'))
assert dt.timedelta(0) <= now - checked < dt.timedelta(minutes=5)
run_uuid = '22d51d47-2992-4043-95ef-54763a9dd45d'
result = {'observed_at_utc': now.isoformat(), 'process_check': evidence,
          'guard_status': guard['status'], 'guard_final_headroom': guard['headroom'],
          'actual_dataset_run_id': run_uuid,
          'termination_cause': 'DuckDB COMMIT OOM at512MB; not a host-headroom stop'}
with duckdb.connect('C:/atx/atx-db/data/warehouse.duckdb', config={
    'memory_limit': '1GB', 'threads': '1', 'preserve_insertion_order': 'false'
}) as con:
    con.execute("SET TimeZone='UTC'")
    con.execute("SET max_temp_directory_size='2GB'")
    activation = con.execute("""
        SELECT stage,run_id,status,started_at,finished_at,rows,error
        FROM activation_stage_runs
        WHERE run_id='activation-companyfacts-archive13' AND stage='companyfacts_load'
    """).fetchall()
    dataset = con.execute("""
        SELECT run_id,dataset_id,status,started_at,finished_at,rows_loaded
        FROM dataset_runs WHERE run_id=? AND dataset_id='sec_company_facts'
          AND json_valid(params_json)
          AND json_extract_string(params_json,'$.run_id')='activation-companyfacts-archive13-companyfacts'
    """, [run_uuid]).fetchall()
    assert len(activation) == len(dataset) == 1
    assert activation[0][2] == 'failed' and activation[0][4] is not None
    assert 'Out of Memory Error' in activation[0][6]
    assert dataset[0][2] == 'running' and dataset[0][4] is None
    attempt = con.execute('SELECT count(*),count(DISTINCT cik) FROM sec_company_facts WHERE run_id=?', [run_uuid]).fetchone()
    assert attempt == (0, 0) and activation[0][5] == 0
    result.update(activation_before=activation, dataset_before=dataset,
                  attempt_columns=['rows','ciks'], attempt=attempt)
    reason = ('Archive13 terminated after DuckDB COMMIT failed at512MB with Out of Memory Error. '
              'Failure-ledger write used invalidated connection. Original worker is absent; '
              '0 committed fact rows from this attempt. Existing activation failure is preserved. '
              'finished_at is operator recovery time; see companyfacts-archive13-oom-recovery.json.')
    con.execute('BEGIN')
    try:
        changed = con.execute("""
            UPDATE dataset_runs SET status='failed',finished_at=?,rows_loaded=?,error_message=?
            WHERE run_id=? AND dataset_id='sec_company_facts' AND status='running' AND finished_at IS NULL
            RETURNING run_id,dataset_id,status,finished_at,rows_loaded
        """, [now, attempt[0], reason, run_uuid]).fetchall()
        assert len(changed) == 1
        assert con.execute("SELECT stage,run_id,status,started_at,finished_at,rows,error FROM activation_stage_runs WHERE run_id='activation-companyfacts-archive13' AND stage='companyfacts_load'").fetchall() == activation
        con.execute('COMMIT')
    except BaseException:
        con.execute('ROLLBACK')
        raise
    result.update(dataset_after=changed, activation_unchanged=True, committed=True, checkpoint='pending')
    with out.open('x', encoding='utf-8') as handle:
        json.dump(result, handle, indent=2, default=str)
    con.execute('CHECKPOINT')
    result['checkpoint'] = 'passed'
    out.write_text(json.dumps(result, indent=2, default=str) + '\n', encoding='utf-8')
print(json.dumps({key: result[key] for key in ('actual_dataset_run_id','attempt','committed','checkpoint','activation_unchanged')}, default=str))
