"""Pin the five-year bounded native result; no economic acceptance inference."""
import ast
import json
import os
from pathlib import Path
import pyarrow.parquet as pq
from atx_db.alpha_panel import common as C, release_inputs as R, universe_release as U

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
workspace = Path(__file__).resolve().parent
out = workspace / 't16-universe-progress-audit-v2.json'
if out.exists():
    raise RuntimeError('Preserve prior audit')
root = Path('C:/atx/atx-db/data/alpha_panel/v1')
generation = root / 'universe_release_v1/generations/r15_20261003_001'
guard_path = workspace / 'logs/t16-universe-r15-resume-20261004-v2.receipt.json'
stdout_path = workspace / 'logs/t16-universe-r15-resume-20261004-v2.out.log'
guard = C.read_json(guard_path)
expected_command = ['.venv/Scripts/python.exe', '-m', 'atx_db.alpha_panel.universe_release',
                    'build', '--bindings', 'C:/atx/.superpowers/sdd/tier1-v4/task16-reader-binding-v1.json',
                    '--generation', 'r15_20261003_001', '--max-new-years', '2']
if guard['status'] != 'completed' or guard['returncode'] != 0 or guard['cap_hit'] or guard['command'] != expected_command:
    raise RuntimeError('Native bounded execution not proven')
native = ast.literal_eval(stdout_path.read_text(encoding='utf-8').strip())
if native != {'status': 'partial_cache_only', 'published': False,
              'missing_years': list(range(2023, 2027)), 'qualification': 'BYTE_IDENTITY_BOUND',
              'readiness': 'NOT_PRODUCTION_READY'}:
    raise RuntimeError('Unexpected native bounded result')
snapshot_path = generation / 'snapshot.json'
snapshot = C.read_json(snapshot_path)
U.validate_snapshot_contract(root, snapshot)
pins = {str(path): R.identity(path) for path in (guard_path, stdout_path, snapshot_path)}
R.recheck(root, snapshot)
rows = []
for year in range(2018, 2023):
    receipt_path = generation / f'year={year}/receipt.json'
    payload = receipt_path.with_name('universe.parquet')
    receipt = C.read_json(receipt_path)
    payload_pin = R.identity(payload)
    if type(receipt['year']) is not int or receipt['year'] != year or receipt['snapshot_sha256'] != R.digest(snapshot):
        raise RuntimeError('Year/snapshot receipt mismatch')
    if receipt['payload'] != payload_pin or pq.ParquetFile(payload).metadata.num_rows != receipt['keys']['rows']:
        raise RuntimeError('Payload identity or footer count mismatch')
    if receipt['daily_sha256'] != R.digest(receipt['daily']) or set(receipt['daily']) != set(receipt['keys']['dates']):
        raise RuntimeError('Daily receipt identity/date mismatch')
    pins[str(receipt_path)] = R.identity(receipt_path)
    pins[str(payload)] = payload_pin
    rows.append({'year': year, 'rows': receipt['keys']['rows'],
                 'sessions': len(receipt['keys']['dates']), 'payload': payload_pin,
                 'receipt': pins[str(receipt_path)]})
prior_path = workspace / 't16-universe-progress-audit-v1.json'
prior = C.read_json(prior_path)
if rows[:3] != prior['years'] or pins[str(snapshot_path)] != prior['evidence'][str(snapshot_path)]:
    raise RuntimeError('Previously admitted year or snapshot identity changed')
pins[str(prior_path)] = R.identity(prior_path)
for path, pin in pins.items():
    if R.identity(Path(path)) != pin:
        raise RuntimeError('Evidence changed during audit')
if (generation / 'manifest.json').exists() or (root / 'universe_release_v1/current.json').exists():
    raise RuntimeError('Unexpected complete publication')
result = {'status': 'FIVE_BOUNDED_NATIVE_YEARS_IDENTITIES_VERIFIED', 'years': rows,
          'missing_years': list(range(2023, 2027)), 'evidence': pins,
          'native_run_seconds': guard['run_seconds'], 'native_peak_bytes': guard['native_peak_job_memory_bytes'],
          'qualification': 'Current complete input-byte recheck and output/footer/receipt identities only. Native job performed full typed keys/counters replay. Original source/economic gates and full publication remain open.'}
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({key: result[key] for key in ('status', 'years', 'missing_years', 'qualification')}), flush=True)
