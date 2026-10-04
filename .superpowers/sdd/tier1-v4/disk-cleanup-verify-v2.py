"""Verify protected preparation, candidates, extraction statistics and locks."""
import json
import os
from pathlib import Path
os.environ['ATX_FUND_STAGE'] = 'fundamentals_v11'
from atx_db.alpha_panel import common as C, fundamentals as F

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
workspace = Path(__file__).resolve().parent
out = workspace / 'logs/disk-cleanup-verification-v2.json'
if out.exists():
    raise RuntimeError('Preserve previous verification')
audit = C.read_json(workspace / 'disk-cleanup-audit-v2.json')
cleanup = C.read_json(workspace / 'logs/disk-cleanup-applied-v2.json')
if C.sha256_file(workspace / 'disk-cleanup-audit-v2.json') != cleanup['audit_sha256']:
    raise RuntimeError('Cleanup audit identity changed')
if any(Path(row['path']).exists() for row in cleanup['deleted']):
    raise RuntimeError('Deleted scratch reappeared')
prepared = F._verify_preparation()
if len(prepared['units']) != 144:
    raise RuntimeError('Preparation inventory changed')
for stage, candidate in audit['candidate_evidence'].items():
    directory = Path(candidate['directory'])
    actual = {str(path.relative_to(directory)): {'sha256': C.sha256_file(path), 'bytes': path.stat().st_size}
              for path in directory.rglob('*') if path.is_file()} if directory.exists() else {}
    if actual != candidate['files']:
        raise RuntimeError('Candidate file inventory or bytes changed: ' + stage)
for lock in cleanup['preserved_orphan_locks']:
    path = Path(lock['destination'])
    if Path(lock['source']).exists() or C.sha256_file(path) != lock['sha256'] or path.stat().st_size != lock['bytes']:
        raise RuntimeError('Orphan lock preservation changed')
recovery = C.read_json(workspace / 'logs/t16-extract-stats-recovery-v1.json')
extract = C.build_root() / 'fundamentals_v11/_work/cf'
if len(recovery['new_files']) != 85:
    raise RuntimeError('Restored stats inventory changed')
for name, pin in recovery['new_files'].items():
    path = extract / name
    if {'sha256': C.sha256_file(path), 'bytes': path.stat().st_size} != pin:
        raise RuntimeError('Essential extraction stats changed')
result = {'status': 'PROTECTED_BYTES_VERIFIED_AFTER_CLEANUP', 'preparation_units': 144,
          'restored_extraction_stats_verified': 85, 'candidate_evidence': audit['candidate_evidence'],
          'preserved_orphan_locks': cleanup['preserved_orphan_locks'],
          'qualification': 'Byte/cleanup safety only. No full source replay, candidate admission, economic gate or production acceptance.'}
C.write_json_atomic(out, result)
print(json.dumps(result), flush=True)
