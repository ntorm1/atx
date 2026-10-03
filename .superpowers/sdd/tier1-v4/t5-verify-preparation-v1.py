"""Read-only validation of completed reviewed v11 preparation; no batch certification."""
import hashlib
import json
import os
from pathlib import Path

os.environ['ATX_FUND_STAGE'] = 'fundamentals_v11'
from atx_db.alpha_panel import common as C, fundamentals as F

out = Path(__file__).resolve().parent / 'logs/t5-preparation-verification-v1.json'
if out.exists():
    raise RuntimeError('Preserve prior verification')
prepare_path = F.work_dir() / 'prepare.json'
before = C.sha256_file(prepare_path)
binding = F._verify_preparation()
if C.sha256_file(prepare_path) != before:
    raise RuntimeError('Preparation summary changed during verification')
result = {
    'status': 'AUTHENTICATED_PREPARATION',
    'preparation_summary_sha256': before,
    'verified_binding_sha256': hashlib.sha256(json.dumps(binding, sort_keys=True).encode()).hexdigest(),
    'units': len(binding['units']),
    'fsds_manifest': binding['source']['manifest'],
    'code': binding['code'],
    'batch_receipt_names': sorted(p.name for p in F.parts_dir().glob('batch-*.json')),
    'batch_integrity': 'NOT_CERTIFIED; legacy batch resume checks existence and metadata only',
    'acceptance': 'NOT_ESTABLISHED; this validates preparation, not financial gates or final publication',
}
C.write_json_atomic(out, result)
print(json.dumps({k: v for k, v in result.items() if k != 'code'}), flush=True)
