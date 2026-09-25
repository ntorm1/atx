"""Tiny in-memory acceptance of the actual literal-only recovery helpers."""
import argparse
import ast
import datetime as dt
import importlib.util
import json
import sys
from pathlib import Path

import duckdb

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
assert not args.output.exists(), 'Refusing to overwrite acceptance evidence'
source = Path(__file__).with_name('close_companyfacts_archive18_headroom_stop.py')
tree = ast.parse(source.read_text(encoding='utf-8'))
for node in ast.walk(tree):
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == 'execute':
        assert len(node.args) == 1 and not node.keywords, 'Recovery still binds Python parameters'
spec = importlib.util.spec_from_file_location('literal_recovery', source)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
run_uuid = 'ae5ab920-6ef2-4d69-bf21-04c850000018'
wrong_uuid = 'ae5ab920-6ef2-4d69-bf21-04c850000019'
now = dt.datetime(2026, 9, 25, 0, 12, 34, 567890, tzinfo=dt.UTC)
reason = "operator's reason'; UPDATE dataset_runs SET status='wrong'; --"
checks = []
with duckdb.connect(':memory:', config={'memory_limit': '64MB', 'threads': '1'}) as con:
    con.execute("SET TimeZone='UTC'")
    values = con.execute(f'SELECT {module._sql_text(reason)}, {module._sql_uuid(run_uuid)}, '
                         f'{module._sql_count(7)}, {module._sql_utc_timestamp(now)}').fetchone()
    assert values == (reason, run_uuid, 7, now)
    checks.append('escaped_text_canonical_uuid_integer_aware_utc_microseconds_equal')
    for helper, value in [(module._sql_text, 'NUL\x00text'), (module._sql_uuid, run_uuid.upper()),
                          (module._sql_uuid, "bad'uuid"), (module._sql_count, -1),
                          (module._sql_count, True), (module._sql_count, '1'),
                          (module._sql_utc_timestamp, now.replace(tzinfo=None))]:
        try:
            helper(value)
        except ValueError:
            pass
        else:
            raise AssertionError(f'Unsafe literal input accepted: {helper.__name__}')
    checks.append('invalid_literal_inputs_refused')
    con.execute('''CREATE TABLE activation_stage_runs (
        stage VARCHAR, run_id VARCHAR, status VARCHAR, finished_at TIMESTAMP, rows BIGINT, error VARCHAR);
        CREATE TABLE dataset_runs (
        run_id VARCHAR, dataset_id VARCHAR, status VARCHAR, finished_at TIMESTAMP,
        rows_loaded BIGINT, error_message VARCHAR)''')
    con.execute('''INSERT INTO activation_stage_runs VALUES
        ('companyfacts_load','activation-companyfacts-archive18','running',NULL,0,NULL),
        ('companyfacts_load','activation-companyfacts-archive19','running',NULL,0,NULL),
        ('other_stage','activation-companyfacts-archive18','running',NULL,0,NULL)''')
    con.execute(f'''INSERT INTO dataset_runs VALUES
        ({module._sql_uuid(run_uuid)},'sec_company_facts','running',NULL,0,NULL),
        ({module._sql_uuid(wrong_uuid)},'other_dataset','running',NULL,0,NULL)''')

    def snapshot():
        return (con.execute('SELECT * FROM activation_stage_runs ORDER BY stage,run_id').fetchall(),
                con.execute('SELECT * FROM dataset_runs ORDER BY run_id').fetchall())

    before = snapshot()
    try:
        module._close_ledgers(con, now=now, run_uuid=wrong_uuid, rows=7, reason=reason)
    except AssertionError:
        pass
    else:
        raise AssertionError('Wrong dataset scope was accepted')
    assert snapshot() == before
    checks.append('wrong_dataset_rolls_back_both_ledgers')
    a, b = module._close_ledgers(con, now=now, run_uuid=run_uuid, rows=7, reason=reason)
    assert len(a) == len(b) == 1 and a[0][3] == b[0][3] == now.replace(tzinfo=None)
    assert a[0][4] == b[0][4] == 7
    after = snapshot()
    changed_activation = [row for row in after[0] if row[2] == 'failed']
    changed_dataset = [row for row in after[1] if row[2] == 'failed']
    assert len(changed_activation) == len(changed_dataset) == 1
    assert changed_activation[0][-1] == changed_dataset[0][-1] == reason
    assert [row for row in after[0] if row[2] == 'running'] == [
        row for row in before[0] if row[0] != 'companyfacts_load' or row[1] != 'activation-companyfacts-archive18'
    ]
    assert [row for row in after[1] if row[2] == 'running'] == [row for row in before[1] if row[0] != run_uuid]
    checks.append('exact_one_per_ledger_updated_other_rows_preserved')
    checks.append('aware_utc_literal_stores_original_naive_utc_timestamp_with_microseconds')
    try:
        module._close_ledgers(con, now=now, run_uuid=run_uuid, rows=7, reason=reason)
    except AssertionError:
        pass
    else:
        raise AssertionError('Repeat recovery was accepted')
    assert snapshot() == after
    checks.append('repeat_recovery_refused_without_changes')
assert 'pandas' not in sys.modules and 'numpy' not in sys.modules
checks.append('no_pandas_numpy_imports_no_python_parameter_binding')
result = {'status': 'passed', 'duckdb_version': duckdb.__version__, 'memory_limit': '64MB',
          'threads': 1, 'database': ':memory:', 'checks': checks,
          'finished_at': dt.datetime.now(dt.UTC).isoformat()}
args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result))
