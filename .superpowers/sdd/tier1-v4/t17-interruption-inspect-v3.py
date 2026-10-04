"""Bounded read-only reconciliation of the third R23 host-pressure stop."""
import json
import os
from pathlib import Path
from atx_db.alpha_panel import fund_batch_release as N

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
workspace = Path(__file__).resolve().parent
out = workspace / 't17-interruption-inspection-v3.json'
if out.exists():
    raise RuntimeError('Preserve previous inspection')
guard_path = workspace / 'logs/t17-runtime-pilot-b0000-v3.receipt.json'
guard = N.read(guard_path)
if guard['status'] != 'stopped_low_commit' or guard['returncode'] != 137 or guard['cap_hit']:
    raise RuntimeError('Unexpected stopped-job evidence')
stage = Path('C:/atx/atx-db/data/alpha_panel/v1/fundamentals_release_v1')
generation = stage / 'generations/r17_20261004_002'
snapshot = generation / 'snapshot.json'
if N.identity(snapshot)['sha256'] != 'c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c':
    raise RuntimeError('Previously admitted snapshot changed')
members = {}
unit = generation / 'units/batch-0000'
for path in unit.rglob('*'):
    if path.is_symlink():
        raise RuntimeError('Linked native member')
    if path.is_file():
        if len(members) >= 100:
            raise RuntimeError('Unexpected native inventory size')
        members[str(path)] = N.identity(path)
lock = stage / '.writer.lock'
lock_identity = N.identity(lock) if lock.exists() else None
lock_text = lock.read_text(encoding='utf-8') if lock.exists() and lock.stat().st_size < 32 else None
admission_path = unit / 'admission.json'
admission = N.read(admission_path) if admission_path.exists() else None
for path, pin in members.items():
    if N.identity(Path(path)) != pin:
        raise RuntimeError('Native member changed during inspection')
result = {
    'status': 'HOST_STOPPED_FINANCIAL_CANDIDATE_INSPECTED',
    'guard': N.identity(guard_path), 'snapshot': N.identity(snapshot),
    'lock_identity': lock_identity, 'lock_text': lock_text,
    'admission': admission, 'native_members': members,
    'final_manifest_exists': (generation / 'manifest.json').exists(),
    'current_pointer_exists': (stage / 'current.json').exists(),
    'qualification': 'Read-only byte inventory. Retained unit admission, if any, still requires normal full native source replay. No financial or production acceptance.'
}
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({key: result[key] for key in ('status', 'lock_identity', 'lock_text', 'admission', 'final_manifest_exists', 'current_pointer_exists', 'qualification')}), flush=True)
