"""Inspect only ended root-owned scratch; preserve all durable source evidence."""
import json
import os
from pathlib import Path

os.environ['ATX_FUND_STAGE'] = 'fundamentals_v11'
from atx_db.alpha_panel import common as C, fundamentals as F

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
workspace = Path(__file__).resolve().parent
out = workspace / 'disk-cleanup-audit-v2.json'
if out.exists():
    raise RuntimeError('Preserve previous audit')
root = C.build_root().resolve()
if root != Path('C:/atx/atx-db/data/alpha_panel/v1').resolve():
    raise RuntimeError('Unexpected data root')
ended = {}
for name in ('t16-existing-units-admit-v1', 't16-fund-freeze-v1', 't16-fund-freeze-v2'):
    path = workspace / f'logs/{name}.receipt.json'
    receipt = C.read_json(path)
    if receipt['status'] not in ('completed', 'failed', 'stopped_low_commit') or 'returncode' not in receipt:
        raise RuntimeError('Job has not ended: ' + name)
    ended[name] = {'guard_pid': receipt['guard_pid'], 'child_pid': receipt['child_pid'],
                   'status': receipt['status'], 'sha256': C.sha256_file(path)}

prepared = F._verify_preparation()
if len(prepared['units']) != 144:
    raise RuntimeError('Current preparation inventory changed')

def strings(value):
    if isinstance(value, str):
        yield value.replace('\\', '/').casefold()
    elif isinstance(value, dict):
        for key, child in value.items():
            yield from strings(key)
            yield from strings(child)
    elif isinstance(value, list):
        for child in value:
            yield from strings(child)

dependencies = list(strings(prepared))
candidates, locks = {}, {}
for stage, generation in (('universe_release_v1', 'r15_20261003_001'),
                          ('fundamentals_release_v1', 'r17_20261004_001')):
    directory = root / stage / 'generations' / generation
    files = {}
    if directory.exists():
        for path in directory.rglob('*'):
            if path.is_symlink():
                raise RuntimeError('Unexpected linked candidate')
            if path.is_file():
                if len(files) >= 100:
                    raise RuntimeError('Unexpected candidate inventory')
                files[str(path.relative_to(directory))] = {'sha256': C.sha256_file(path), 'bytes': path.stat().st_size}
                if path.name in ('snapshot.json', 'manifest.json'):
                    dependencies.extend(strings(C.read_json(path)))
    candidates[stage] = {'directory': str(directory), 'files': files,
                         'manifest_exists': (directory / 'manifest.json').exists()}
    lock = root / stage / '.writer.lock'
    if lock.exists():
        locks[stage] = {'path': str(lock), 'pid': int(lock.read_text()),
                        'sha256': C.sha256_file(lock), 'bytes': lock.stat().st_size}

dependencies.extend(strings(C.read_json(workspace / 'task16-fund-reader-binding-v1.json')))
targets = {
    '_tmp/universe-release-r15_20261003_001': 'Read/replay-only scratch from ended existing-unit admission; no durable unit is stored here.',
    '_tmp/fund-release-extract-verify': 'Interrupted temporary ZIP reconstruction peers; unsealed and never accepted as source or financial units.',
    'fundamentals_release_v1/_spill': 'Private DuckDB database and spill from ended root source-freeze jobs; connections use new UUID databases.',
}
rows = []
for relative, reason in targets.items():
    path = (root / relative).resolve()
    if not path.is_relative_to(root) or path == root:
        raise RuntimeError('Cleanup target escaped root')
    mentions = [value for value in dependencies if str(path).replace('\\', '/').casefold() in value or relative.casefold() in value]
    files, reparse, total = [], [], 0
    pending = [path] if path.exists() else []
    while pending:
        member = pending.pop()
        stat = member.lstat()
        if getattr(stat, 'st_file_attributes', 0) & 0x400:
            reparse.append(str(member))
        elif member.is_dir():
            pending.extend(member.iterdir())
        elif member.is_file():
            files.append({'path': str(member), 'bytes': stat.st_size, 'mtime_ns': stat.st_mtime_ns})
            total += stat.st_size
            if len(files) > 20000:
                raise RuntimeError('Bounded scratch inventory exceeded')
    rows.append({'path': str(path), 'exists': path.exists(), 'reason': reason,
                 'dependency_mentions': mentions, 'reparse_points': reparse,
                 'files': sorted(files, key=lambda row: row['path']), 'logical_bytes': total,
                 'eligible_for_explicit_removal': path.exists() and not mentions and not reparse})
result = {'status': 'AUDIT_ONLY_NO_DELETION', 'root': str(root), 'ended_runs': ended,
          'authenticated_current_preparation_units': 144, 'candidate_evidence': candidates,
          'retained_locks': locks, 'targets': rows,
          'candidate_total_logical_bytes': sum(row['logical_bytes'] for row in rows if row['eligible_for_explicit_removal'])}
C.write_json_atomic(out, result)
print(json.dumps({key: value for key, value in result.items() if key not in ('targets', 'candidate_evidence')}), flush=True)
print(json.dumps({'targets': [{key: row[key] for key in ('path', 'logical_bytes', 'eligible_for_explicit_removal')} for row in rows],
                  'candidate_evidence': candidates}), flush=True)
