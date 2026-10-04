"""Guarded no-overwrite recovery of exact missing retained native stats sidecars."""
import json
import shutil
from pathlib import Path
from atx_db.alpha_panel import common as C

workspace = Path(__file__).resolve().parent
audit_path = workspace / 'task16-extract-layout-audit-v1.json'
out = workspace / 'logs/t16-extract-stats-recovery-v1.json'
if out.exists():
    raise RuntimeError('Preserve prior recovery receipt')
audit = C.read_json(audit_path)
root = Path('C:/atx/atx-db/data/alpha_panel/v1').resolve()
if C.build_root().resolve() != root:
    raise RuntimeError('Unexpected root')
source = (root / 'fundamentals_v10/_work/cf').resolve()
target = (root / 'fundamentals_v11/_work/cf').resolve()
for directory in (source, target):
    if not directory.is_relative_to(root) or getattr(directory.lstat(), 'st_file_attributes', 0) & 0x400:
        raise RuntimeError('Escaping/reparse extraction root')
source_audit = audit['stages']['fundamentals_v10']
target_audit = audit['stages']['fundamentals_v11']
expected = {f'batch-{i:04d}.stats.parquet' for i in range(85)}
if set(source_audit['stats']) != expected or set(target_audit['missing']) != expected or target_audit['extra'] or source_audit['missing'] or source_audit['extra']:
    raise RuntimeError('Unexpected extraction layout')
if source_audit['plan_sha256'] != target_audit['plan_sha256']:
    raise RuntimeError('Different extraction plans')
def pin(path):
    if getattr(path.lstat(), 'st_file_attributes', 0) & 0x400:
        raise RuntimeError('Reparse member refused')
    before = path.stat()
    value = {'sha256': C.sha256_file(path), 'bytes': before.st_size}
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError('Member changed during hashing')
    return value
source_members = {p.name for p in source.iterdir() if p.is_file()}
target_members = {p.name for p in target.iterdir() if p.is_file()}
shared = {'plan.json'} | {f'batch-{i:04d}{suffix}' for i in range(85) for suffix in ('.parquet', '.json')}
if source_members != shared | expected or target_members != shared:
    raise RuntimeError('Extraction inventory changed since audit')
# All171 existing files must be byte-identical before any new destination exists.
shared_pins = {}
for name in sorted(shared):
    source_pin, target_pin = pin(source / name), pin(target / name)
    if source_pin != target_pin:
        raise RuntimeError('Source/target extract disagree: ' + name)
    shared_pins[name] = source_pin
for name in sorted(expected):
    measured = pin(source / name)
    recorded = source_audit['stats'][name]
    if not recorded['native_schema_matches'] or measured != {k: recorded[k] for k in ('sha256', 'bytes')}:
        raise RuntimeError('Audited source stats changed')
copied = {}
for name in sorted(expected):
    origin, destination = source / name, target / name
    # Exclusive creation refuses concurrent or previously installed destinations.
    with origin.open('rb') as reader, destination.open('xb') as writer:
        shutil.copyfileobj(reader, writer, length=1024 * 1024)
    actual = pin(destination)
    if actual != pin(origin) or actual != {k: source_audit['stats'][name][k] for k in ('sha256', 'bytes')}:
        raise RuntimeError('Copied stats changed; preserve forensic destination')
    copied[name] = actual
if {p.name for p in target.iterdir() if p.is_file()} != source_members:
    raise RuntimeError('Recovered inventory differs')
for name, expected_pin in shared_pins.items():
    if pin(source / name) != expected_pin or pin(target / name) != expected_pin:
        raise RuntimeError('Existing extract changed during recovery')
result = {'status': 'MISSING_NATIVE_STATS_RESTORED_NOT_SOURCE_ADMITTED',
          'source': str(source), 'target': str(target), 'shared_files_verified': len(shared_pins),
          'new_files': copied, 'new_logical_bytes': sum(row['bytes'] for row in copied.values()),
          'inventory_files': len(source_members), 'audit': pin(audit_path),
          'qualification': 'No legacy financial batch admitted. Native full archive replay and source/code/preparation/unit/gate checks remain required.'}
C.write_json_atomic(out, result)
print(json.dumps({k: v for k, v in result.items() if k != 'new_files'}), flush=True)
