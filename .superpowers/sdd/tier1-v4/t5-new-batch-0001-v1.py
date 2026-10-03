"""Bounded new-only computation, not certification of legacy batch reuse."""
import json
import os
from pathlib import Path

os.environ['ATX_FUND_STAGE'] = 'fundamentals_v11'
from atx_db.alpha_panel import common as C, fund_depth as FD, fundamentals as F

workspace = Path(__file__).resolve().parent
receipt = workspace / 'logs/t5-new-batch-0001-v1.json'
if receipt.exists():
    raise RuntimeError('Preserve prior operational receipt')
stage = C.stage_dir('fundamentals_v11')
if (stage / 'manifest.json').exists() or any(stage.glob('*.parquet')):
    raise RuntimeError('Refuse existing stage publication')
existing = list(F.parts_dir().glob('*0001*'))
if existing:
    raise RuntimeError(f'Refuse prior complete or partial batch evidence: {existing}')
extract = FD.reuse_extract(C.stage_dir('fundamentals_v10'), stage)
preparation = F._verify_preparation()
code = preparation['code']
before = {'status': 'NEW_BATCH_COMPUTATION_STARTED_NOT_ADMITTED',
          'extract': extract, 'preparation': preparation, 'batch': 1,
          'qualification': 'No legacy batch reuse or final dataset publication authorized.'}
C.write_json_atomic(receipt, before)
print(json.dumps({'status': before['status'], 'preparation_units': len(preparation['units'])}), flush=True)
F.run_batches((1, 1))
after = F._verify_preparation()
if after != preparation:
    raise RuntimeError('Preparation/code/source identity changed during batch')
outputs = {}
for path in sorted(F.parts_dir().glob('*0001*')):
    outputs[path.name] = {'sha256': C.sha256_file(path), 'bytes': path.stat().st_size}
result = {**before, 'status': 'NEW_BATCH_COMPUTED_NOT_ADMITTED', 'outputs': outputs,
          'qualification': 'Observed new output bytes only; independent keys/source/code/batch admission pending.'}
C.write_json_atomic(receipt, result)
print(json.dumps({'status': result['status'], 'outputs': outputs}), flush=True)
