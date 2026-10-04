"""One fresh original256MB replay of failed9; no native admission or reuse."""
import json
import os
from pathlib import Path
import time

from atx_db.alpha_panel import fund_batch_release as N, fund_release_runtime as R

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
result_path = base / 't17-financial-b0009-fresh-replay-diagnostic-v1.json'
start_path = base / 'logs/t17-financial-b0009-fresh-replay-diagnostic-v1.start.json'
stage = Path('C:/atx/atx-db/data/alpha_panel/v1/fundamentals_release_v1')
generation = stage / 'generations/r17_20261004_002'
target = Path('C:/atx/atx-db/data/alpha_panel/v1/_tmp/fund-release-fresh-replay9-v1')
inspection_path = base / 't17-financial-b0009-failure-inspection-v1.json'
inspection_pin = {'bytes': 26771, 'sha256': '82df3cf5ea14fb911f3c9cb70dcb1505aaae38e7accf307d837995de70d24fc7'}


def safe(path):
    path = Path(path).absolute()
    if '..' in path.parts:
        raise RuntimeError('Parent traversal refused')
    for item in (path,*path.parents):
        if item.is_symlink() or item.is_junction():
            raise RuntimeError('Diagnostic reparse path refused')
    return path


for path in (base,stage,generation,target,inspection_path,result_path,start_path):
    safe(path)
if any(path.exists() for path in (result_path,start_path,target)):
    raise RuntimeError('Preserve earlier diagnostic evidence')
if N.identity(inspection_path) != inspection_pin:
    raise RuntimeError('Independently reviewed preservation observation differs')
inspection = N.read(inspection_path)
attempts = inspection['failed_attempts']
if set(attempts) != {'attempt-8e024c20ad76493298df97805f7223ff'}:
    raise RuntimeError('Exact failed attempt required')
first = generation / 'units/batch-0009/attempt-8e024c20ad76493298df97805f7223ff/native'
snapshot_path = generation/'snapshot.json'
snapshot_pin = {'bytes': 10411360, 'sha256': 'c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c'}
snapshot_sha = 'baf8f5cc238e9b2c38ab6583463fa0d23166928b645a348137eb5e43d0092f3d'
contract = Path('C:/atx/atx-db/docs/datasets/fund_release_runtime.execution.json')
runtime = R.load_runtime(contract,
    '45ea46d78385ad7969551cce8accd511f6c2d3c93e6b4b5bb70e4cdb20eaf49e',
    'e50412ff21622ca3710420baec9c6fc982ac8cb0ba7fe22347c2269b185a8594')
stable = {**inspection['stable_file_pins'], str(inspection_path): inspection_pin,
          str(Path(__file__).resolve()): N.identity(Path(__file__).resolve())}


def preserved():
    for path,pin in stable.items():
        if N.identity(safe(path)) != pin:
            raise RuntimeError('Failed/successful prior evidence changed')
    safe(target)
    frontier = [bid for bid in range(85) if (generation/f'units/batch-{bid:04d}/admission.json').exists()]
    if frontier != list(range(9)):
        raise RuntimeError('Diagnostic must not create or change native admissions')
    if any(path.exists() for path in (stage/'current.json',stage/'pointer-transaction.json',generation/'manifest.json')):
        raise RuntimeError('Unexpected publication state')


started = time.monotonic()
preserved()
snapshot = N.read(snapshot_path)
if N.identity(snapshot_path) != snapshot_pin or N.digest(snapshot) != snapshot_sha:
    raise RuntimeError('Frozen snapshot differs')
with R.connection(stage,runtime) as (con,resources), N.writer_lock(stage):
    proof = R.check_proof(stage,generation,runtime,snapshot_sha)
    # Retain the original full source recheck and original semantic helpers.
    N.recheck(snapshot,con)
    R._recheck_proof(stage,generation,runtime,snapshot_sha,proof)
    safe(target).mkdir(parents=False,exist_ok=False)
    N._write_immutable(start_path,{'status':'FRESH_ORIGINAL_256MB_REPLAY9_DIAGNOSTIC_STARTED',
        'snapshot':snapshot_pin,'snapshot_sha256':snapshot_sha,'resources':resources,
        'runtime_proof':proof,'inspection':inspection_pin,'target':str(target),
        'controller':stable[str(Path(__file__).resolve())],'publication':False,'new_admissions':0})
    stable[str(start_path)] = N.identity(start_path)
    try:
        N._replay(snapshot,con,9,target)
        source = Path(snapshot['binding']['extract_root'])/'batch-0009.parquet'
        # Full typed/schema/population comparison, including every failure counter.
        first_files = N.validate_payloads(con,first,9,source,target)
        if N.receipt_semantics(first/'batch-0009.json') != N.receipt_semantics(target/'batch-0009.json'):
            raise RuntimeError('Native receipt/failure counters differ')
        N.recheck(snapshot,con)
        R._recheck_proof(stage,generation,runtime,snapshot_sha,proof)
        diagnostic_files = {str(p):N.identity(safe(p)) for p in N.payloads(target,9).values()}
        diagnostic_files[str(target/'batch-0009.json')] = N.identity(target/'batch-0009.json')
        record = {'status':'FRESH_CONNECTION_NATIVE_REPLAY9_DIAGNOSTIC_MATCHED',
            'resources':resources,'snapshot':snapshot_pin,'snapshot_sha256':snapshot_sha,
            'runtime_proof':proof,'inspection':inspection_pin,'first_pass_files':first_files,
            'diagnostic_files':diagnostic_files,'start':{'path':str(start_path),**N.identity(start_path)},
            'controller':stable[str(Path(__file__).resolve())],'new_admissions':0,
            'publication':False,'gate_promotion':False,'source_quality_certified':False,
            'qualification':'Fresh-connection diagnostic only. Full native replay and typed/counter comparison '
              'against retained failed first-pass outputs do not retroactively make the failed creator successful. '
              'No output is moved into a native unit or sealed. A new connection-sequencing adapter would require '
              'separate reviewed execution provenance, all original ordering/rechecks/two derivations and '
              'interruption/stale-input equivalence evidence. Final whole85 and all release gates remain open.'}
    finally:
        preserved()
# A diagnostic completion is written only after successful original runtime exit.
preserved()
record['seconds'] = round(time.monotonic()-started,3)
N._write_immutable(result_path,record)
print(json.dumps({'status':record['status'],'identity':N.identity(result_path),'new_admissions':0,'publication':False}))
