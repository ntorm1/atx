"""One isolated native batch at 256MB; no candidate admission or publication."""
import json
import os
import time
from pathlib import Path

from atx_db.alpha_panel import common as C, fund_batch_release as R

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
workspace = Path(__file__).resolve().parent
root = Path('C:/atx/atx-db/data/alpha_panel/v1')
snapshot_path = root / 'fundamentals_release_v1/generations/r17_20261004_002/snapshot.json'
expected_snapshot = 'c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c'
admission_path = workspace / 'task16-extract-admission-r19-v1.json'
expected_admission = '168b3ff61f4580066db74624bf58efb803993e8d2a0f1060daf4dfec1024ab1f'
scratch = root / '_tmp/fund-native-runtime-diagnostic-v1'
evidence_path = workspace / 'logs/t16-fund-runtime-diagnostic-v1.json'
if scratch.exists() or evidence_path.exists():
    raise RuntimeError('Preserve prior diagnostic attempt')
if R.identity(snapshot_path)['sha256'] != expected_snapshot:
    raise RuntimeError('Root-admitted source snapshot changed')
snapshot = R.read(snapshot_path)
R.require_snapshot(snapshot)
helper_pin = R.identity(Path(__file__).resolve())
output = scratch / 'fundamentals_v11'
output.mkdir(parents=True, exist_ok=False)
start = {'purpose': 'native batch resource diagnostic only', 'batch': 0,
         'duckdb_memory': '256MB', 'threads': 1, 'snapshot_sha256': expected_snapshot,
         'extract_admission_sha256': expected_admission, 'helper': helper_pin,
         'candidate_admission_allowed': False}
C.write_json_atomic(scratch / 'start.json', start)
started = time.monotonic()
con = C.connect(memory='256MB', threads=1, temp_dir=scratch / '_spill', db_file='diagnostic.duckdb')
try:
    R.recheck(snapshot, con, full=True, extract_admission=admission_path,
              expected_admission_sha256=expected_admission, force_full_replay=False)
    print(json.dumps({'phase': 'source_verified', 'seconds': round(time.monotonic() - started, 2)}), flush=True)
    R._replay(snapshot, con, 0, output)
    files = R.validate_payloads(con, output, 0, Path(snapshot['binding']['extract_root']) / 'batch-0000.parquet')
    print(json.dumps({'phase': 'native_batch_completed', 'seconds': round(time.monotonic() - started, 2)}), flush=True)
    R.recheck(snapshot, con, full=True, extract_admission=admission_path,
              expected_admission_sha256=expected_admission, force_full_replay=False)
    if R.identity(snapshot_path)['sha256'] != expected_snapshot or R.identity(Path(__file__).resolve()) != helper_pin:
        raise RuntimeError('Diagnostic code or frozen snapshot changed')
    result = {**start, 'status': 'ONE_NATIVE_BATCH_RESOURCE_DIAGNOSTIC_COMPLETED',
              'elapsed_seconds': round(time.monotonic() - started, 2),
              'files': files, 'native_receipt': R.identity(output / 'batch-0000.json'),
              'output_logical_bytes': sum(path.stat().st_size for path in output.iterdir() if path.is_file()),
              'qualification': 'Resource and structural diagnostic only; no independent financial replay/admission, no final gates, no candidate or consumer publication.'}
    with evidence_path.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({key: result[key] for key in ('status', 'elapsed_seconds', 'output_logical_bytes', 'qualification')}), flush=True)
finally:
    con.close()
