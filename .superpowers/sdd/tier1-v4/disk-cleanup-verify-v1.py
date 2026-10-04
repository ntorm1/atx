"""Guarded verification that cleanup preserved current preparation and candidates."""
import json
import os
from pathlib import Path
os.environ['ATX_FUND_STAGE'] = 'fundamentals_v11'
from atx_db.alpha_panel import common as C, fundamentals as F

workspace = Path(__file__).resolve().parent
out = workspace / 'logs/disk-cleanup-verification-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous verification')
cleanup = C.read_json(workspace / 'logs/disk-cleanup-applied-v1.json')
if any(Path(row['path']).exists() for row in cleanup['deleted']):
    raise RuntimeError('Deleted scratch target reappeared')
preparation = F._verify_preparation()
generation = C.build_root() / 'universe_release_v1/generations/r15_20261003_001'
units = {}
for receipt_path in sorted(generation.glob('year=*/receipt.json')):
    receipt = C.read_json(receipt_path)
    payload = receipt_path.parent / 'universe.parquet'
    actual = {'sha256': C.sha256_file(payload), 'bytes': payload.stat().st_size}
    if actual != receipt['payload']:
        raise RuntimeError('Candidate output differs from unit receipt')
    units[str(receipt['year'])] = {'payload': actual, 'rows': receipt['keys']['rows'],
                                  'receipt_sha256': C.sha256_file(receipt_path)}
unsealed = [str(path.relative_to(generation)) for path in generation.glob('year=*') if not (path / 'receipt.json').exists()]
lock = generation.parents[1] / '.writer.lock'
result = {'status': 'CURRENT_PREPARATION_AND_CANDIDATE_BYTES_VERIFIED',
          'preparation_units': len(preparation['units']), 'candidate_units': units,
          'unsealed_candidate_directories': unsealed,
          'retained_writer_lock': lock.read_text() if lock.exists() else None,
          'publication_exists': (generation / 'manifest.json').exists(),
          'qualification': 'Cleanup safety verification only; completed candidate units still require normal resume/full-generation admission. No final acceptance.'}
C.write_json_atomic(out, result)
print(json.dumps(result), flush=True)
