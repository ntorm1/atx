"""Bounded post-OOM preservation and named scratch metadata; no deletion/replay."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
output = base / 't17-financial-b0009-failure-inspection-v1.json'
if output.exists():
    raise RuntimeError('Preserve prior observation')
root = Path('C:/atx/atx-db/data/alpha_panel/v1')
stage = root / 'fundamentals_release_v1'
generation = stage / 'generations/r17_20261004_002'


def safe(path):
    path = Path(path).absolute()
    for item in (path, *path.parents):
        if item.is_symlink() or item.is_junction():
            raise RuntimeError('Reparse path refused')
    return path


def identity(path):
    path = safe(path)
    before = path.stat()
    if not stat.S_ISREG(before.st_mode):
        raise RuntimeError('Regular file required')
    h = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1 << 20):
            h.update(block)
    after = path.stat()
    key = lambda s: (s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns)
    if key(before) != key(after):
        raise RuntimeError('Changed during hash')
    return {'bytes': before.st_size, 'sha256': h.hexdigest()}


def read(path, expected=None):
    pin = identity(path)
    if pin['bytes'] > 16 << 20 or expected is not None and pin != expected:
        raise RuntimeError('Bounded original control identity differs')
    value = json.loads(Path(path).read_text(encoding='utf-8'))
    if identity(path) != pin:
        raise RuntimeError('Control changed')
    return value, pin


creator = base / 't17-financial-unit-create-v1.py'
creator_pin = {'bytes': 7880, 'sha256': 'ebbc1064facb857d31c5ee157cce0db8d72f6b294fdd3320808b0436d5dd3c58'}
if identity(creator) != creator_pin:
    raise RuntimeError('Reviewed creator changed')
eight_path = base / 't17-financial-native-create-b0008-v1.json'
eight, eight_pin = read(eight_path, {'bytes': 35992, 'sha256': '61285fbf2c346a748bce838d700bd50a6ab26c7bf19c3a6b54ee50c3b76b91bc'})
failed_path = base / 'logs/t17-financial-native-create-b0009-v1.receipt.json'
failed, failed_pin = read(failed_path)
expected_command = ['.venv/Scripts/python.exe', 'C:/atx/.superpowers/sdd/tier1-v4/t17-financial-unit-create-v1.py', '--batch', '9', '--attempt-tag', 'v1']
if (failed['command'] != expected_command or failed['status'] != 'failed' or failed['returncode'] != 1 or
        failed['cap_hit'] or failed['cap_hit_events'] or failed['job_limit_gb'] != .8 or
        failed.get('stop_reason') or failed['disk']['min_free_gb'] != 40):
    raise RuntimeError('Expected native failed attempt differs')
start_path = base / 'logs/t17-financial-native-create-b0009-v1.start.json'
start, start_pin = read(start_path)
prior = {**eight['prior_units_unchanged'], '8': eight['unit']}
if (start['controller_code'] != creator_pin or start['prior_frontier'] != list(range(9)) or
        start['prior_units'] != prior or start['batch'] != 9):
    raise RuntimeError('Failed attempt does not bind exact successful prior frontier')
snapshot_path = generation / 'snapshot.json'
snapshot_pin = {'bytes': 10411360, 'sha256': 'c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c'}
if identity(snapshot_path) != snapshot_pin:
    raise RuntimeError('Frozen snapshot changed')
stable = {str(creator): creator_pin, str(eight_path): eight_pin, str(failed_path): failed_pin,
          str(start_path): start_pin, str(snapshot_path): snapshot_pin}
for unit in prior.values():
    stable.update(unit['pins'])
for name, pin in stable.items():
    if identity(name) != pin:
        raise RuntimeError('Retained successful unit/provenance changed')
frontier = []
for bid in range(85):
    if (generation / f'units/batch-{bid:04d}/admission.json').exists():
        frontier.append(bid)
if frontier != list(range(9)):
    raise RuntimeError('Unexpected native frontier')
for path in (stage/'.writer.lock', stage/'current.json', stage/'pointer-transaction.json', generation/'manifest.json'):
    if path.exists():
        raise RuntimeError('Writer/publication state requires separate review')
nine = generation / 'units/batch-0009'
attempts = {}
for attempt in safe(nine).iterdir():
    if not attempt.is_dir() or not attempt.name.startswith('attempt-') or len(attempts) >= 8:
        raise RuntimeError('Unexpected bounded failed unit layout')
    row, pin = read(attempt/'start.json')
    if row['batch'] != 9 or row['snapshot_sha256'] != start['snapshot_sha256'] or row['attempt'] != attempt.name:
        raise RuntimeError('Failed native attempt provenance differs')
    files = {str(attempt/'start.json'): pin}
    for member in safe(attempt/'native').iterdir():
        if not member.is_file() or len(files) >= 16:
            raise RuntimeError('Unexpected bounded native attempt files')
        files[str(member)] = identity(member)
    stable.update(files)
    attempts[attempt.name] = {'start': row, 'files': files, 'admitted': False}
scratch = []
for parent in (root/'_tmp', stage/'_spill'):
    directories = 0
    for directory in safe(parent).iterdir():
        if directory.is_dir() and directory.name.startswith('spill-'):
            safe(directory)
            directories += 1
            if directories > 256:
                raise RuntimeError('Bounded spill directory census exceeded')
            files = []
            for file in directory.iterdir():
                safe(file)
                if len(files) >= 256 or not file.is_file():
                    raise RuntimeError('Bounded flat spill census differs')
                s = file.stat()
                files.append({'path': str(file), 'bytes': s.st_size, 'mtime_ns': s.st_mtime_ns})
            if files:
                scratch.append({'directory': str(directory), 'files': files,
                    'logical_bytes': sum(x['bytes'] for x in files), 'deletion_authorized_by_this_audit': False})
for name, pin in stable.items():
    if identity(name) != pin:
        raise RuntimeError('Retained evidence changed during observation')
if [bid for bid in range(85) if (generation/f'units/batch-{bid:04d}/admission.json').exists()] != frontier:
    raise RuntimeError('Frontier changed during observation')
result = {'status': 'FAILED_BATCH9_PRESERVATION_AND_SCRATCH_METADATA_ONLY',
          'prior_frontier': frontier, 'unchanged_member_count': sum(len(x['pins']) for x in prior.values()),
          'stable_file_pins': stable, 'failed_attempts': attempts, 'scratch_observations': scratch,
          'disk_free_bytes': shutil.disk_usage(root).free, 'publication': False, 'gate_promotion': False,
          'new_admissions': 0, 'deleted_files': 0,
          'qualification': 'This authenticates retained bytes and failed-attempt metadata only. '
            'Batch9 has no admission; its first-pass payloads are not newly semantically validated. '
            'Scratch inventory does not prove dead ownership, absence of open handles or deletion safety. '
            'No original source payload, SQL replay, financial promotion or cleanup is performed.'}
with output.open('x',encoding='utf-8') as stream:
    json.dump(result,stream,indent=2)
    stream.write('\n')
print(json.dumps({'status':result['status'],'prior_frontier':frontier,'failed_attempts':len(attempts),
                  'scratch_directories':len(scratch),'scratch_logical_bytes':sum(x['logical_bytes'] for x in scratch),
                  'identity':identity(output)}))
