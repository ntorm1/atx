"""Read-only inspection of checkpoints retained by the interrupted continuation."""
import json
import os
from pathlib import Path
from atx_db.alpha_panel import fund_batch_release as N

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
out = base / 't17-interruption-inspection-v4.json'
if out.exists():
    raise RuntimeError('Preserve previous inspection')
guard_path = base / 'logs/t17-runtime-b0001-b0004-v1.receipt.json'
guard = N.read(guard_path)
if guard['status'] != 'stopped_low_commit' or guard['returncode'] != 137 or guard['cap_hit']:
    raise RuntimeError('Unexpected stopped-job evidence')
stage = Path('C:/atx/atx-db/data/alpha_panel/v1/fundamentals_release_v1')
generation = stage / 'generations/r17_20261004_002'
snapshot = generation / 'snapshot.json'
expected_snapshot = 'c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c'
if N.identity(snapshot)['sha256'] != expected_snapshot:
    raise RuntimeError('Previously admitted snapshot changed')
members = {}
admissions = {}
total_bytes = 0
for bid in range(5):
    unit = generation / 'units' / f'batch-{bid:04d}'
    if not unit.exists():
        continue
    for path in unit.rglob('*'):
        if path.is_symlink() or path.is_junction():
            raise RuntimeError('Linked native member')
        if path.is_file():
            total_bytes += path.stat().st_size
            if len(members) >= 500 or total_bytes > 256 * 1024 * 1024:
                raise RuntimeError('Bounded retained-member inventory exceeded')
            members[str(path)] = N.identity(path)
    admission_path = unit / 'admission.json'
    if admission_path.exists():
        admissions[str(bid)] = N.read(admission_path)
lock = stage / '.writer.lock'
lock_identity = N.identity(lock) if lock.exists() else None
lock_text = lock.read_text(encoding='utf-8') if lock.exists() and lock.stat().st_size < 32 else None
for path, pin in members.items():
    if N.identity(Path(path)) != pin:
        raise RuntimeError('Native member changed during inspection')
if N.identity(snapshot)['sha256'] != expected_snapshot:
    raise RuntimeError('Snapshot changed during inspection')
result = {
    'status': 'HOST_STOPPED_FINANCIAL_CONTINUATION_INSPECTED',
    'guard': N.identity(guard_path), 'snapshot': N.identity(snapshot),
    'lock_identity': lock_identity, 'lock_text': lock_text,
    'admissions': admissions, 'native_members': members,
    'retained_admission_batch_ids': [int(x) for x in admissions],
    'previous_independently_verified_batch_ids': [0],
    'final_manifest_exists': (generation / 'manifest.json').exists(),
    'current_pointer_exists': (stage / 'current.json').exists(),
    'qualification': 'Read-only two-pass retained byte inventory, not new financial source admission. Batch0 was independently checked before this continuation. Any additional native admission is provisional until normal full native source and typed replay under unchanged R23 protocol. Failed/partial attempts and owner lock are preserved.',
}
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({k: result[k] for k in ('status', 'lock_identity', 'lock_text',
      'retained_admission_batch_ids', 'previous_independently_verified_batch_ids',
      'final_manifest_exists', 'current_pointer_exists', 'qualification')}), flush=True)
