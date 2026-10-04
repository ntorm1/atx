"""Read-only bounded inspection after the stopped eight-new-unit request."""
import json
import os
from pathlib import Path
from atx_db.alpha_panel import fund_batch_release as N

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
out = base / 't17-interruption-inspection-v5.json'
if out.exists():
    raise RuntimeError('Preserve previous inspection')
guard_path = base / 'logs/t17-runtime-b0007-b0014-v1.receipt.json'
guard_pin = N.identity(guard_path)
guard = N.read(guard_path)
if (guard['status'] != 'stopped_low_commit' or guard['returncode'] != 137 or
        guard['cap_hit'] is not False or guard['guard_pid'] != 24904 or
        guard['child_pid'] != 20900 or guard['command'][:4] !=
        ['.venv/Scripts/python.exe', '-m', 'atx_db.alpha_panel.fund_release_runtime', 'build']):
    raise RuntimeError('Unexpected stopped-job evidence')
options = dict(zip(guard['command'][4::2], guard['command'][5::2], strict=True))
if (len(guard['command']) != 4 + 2 * len(options) or options.get('--max-new-batches') != '8' or
        options.get('--generation') != 'r17_20261004_002' or
        options.get('--expected-snapshot-sha256') != 'baf8f5cc238e9b2c38ab6583463fa0d23166928b645a348137eb5e43d0092f3d'):
    raise RuntimeError('Unexpected native candidate invocation')
stage = Path('C:/atx/atx-db/data/alpha_panel/v1/fundamentals_release_v1')
generation = stage / 'generations/r17_20261004_002'
snapshot = generation / 'snapshot.json'
snapshot_pin = {'bytes': 10411360, 'sha256': 'c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c'}
if N.identity(snapshot) != snapshot_pin:
    raise RuntimeError('Previously admitted snapshot changed')
members = {}
admissions = {}
units_root = generation / 'units'
allowed_names = {f'batch-{bid:04d}' for bid in range(85)}
total_bytes = 0
unit_names = []
for unit in sorted(units_root.iterdir()):
    if unit.is_symlink() or unit.is_junction() or not unit.is_dir() or unit.name not in allowed_names:
        raise RuntimeError('Unexpected native unit directory')
    unit_names.append(unit.name)
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
        admissions[str(int(unit.name[-4:]))] = N.read(admission_path)
lock = stage / '.writer.lock'
lock_identity = N.identity(lock) if lock.exists() else None
lock_text = lock.read_text(encoding='utf-8') if lock.exists() and lock.stat().st_size < 32 else None
for path, pin in members.items():
    if N.identity(Path(path)) != pin:
        raise RuntimeError('Native member changed during inspection')
if (N.identity(snapshot) != snapshot_pin or N.identity(guard_path) != guard_pin or
        sorted(path.name for path in units_root.iterdir()) != unit_names or
        (N.identity(lock) if lock.exists() else None) != lock_identity):
    raise RuntimeError('Inspection identities or frontier changed')
result = {
    'status': 'HOST_STOPPED_FINANCIAL_CONTINUATION_INSPECTED',
    'guard': guard_pin, 'snapshot': snapshot_pin,
    'lock_identity': lock_identity, 'lock_text': lock_text,
    'admissions': admissions, 'native_members': members,
    'retained_admission_batch_ids': sorted(int(x) for x in admissions),
    'previous_independently_verified_batch_ids': list(range(7)),
    'inspected_native_member_count': len(members), 'inspected_native_bytes': total_bytes,
    'final_manifest_exists': (generation / 'manifest.json').exists(),
    'current_pointer_exists': (stage / 'current.json').exists(),
    'qualification': 'Read-only two-pass retained byte inventory, not new financial source admission. Units0..6 have prior independent checks recorded in readinessv8; this inspection does not repeat those semantic checks or grant additional credit. Any additional native admission remains provisional. Failed attempts and owner lock remain unchanged. Process liveness and exact safe lock recovery are separate prerequisites before future native work.',
}
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({k: result[k] for k in ('status', 'lock_identity', 'lock_text',
      'retained_admission_batch_ids', 'previous_independently_verified_batch_ids',
      'inspected_native_member_count', 'inspected_native_bytes',
      'final_manifest_exists', 'current_pointer_exists', 'qualification')}), flush=True)
