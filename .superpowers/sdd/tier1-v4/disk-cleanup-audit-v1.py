"""Guarded metadata audit of exact, owned and superseded derived scratch paths."""
import json
import os
from pathlib import Path

os.environ['ATX_FUND_STAGE'] = 'fundamentals_v11'
from atx_db.alpha_panel import common as C, fundamentals as F

workspace = Path(__file__).resolve().parent
out = workspace / 'disk-cleanup-audit-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous cleanup audit')
root = C.build_root().resolve()
if root != Path('C:/atx/atx-db/data/alpha_panel/v1').resolve():
    raise RuntimeError('Unexpected data root')
prepared = F._verify_preparation()
if len(prepared['units']) != 144:
    raise RuntimeError('Current authenticated preparation changed')


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
snapshot = root / 'universe_release_v1/generations/r15_20261003_001/snapshot.json'
dependencies.extend(strings(C.read_json(snapshot)))
targets = {
    '_tmp/universe-release-r15_20261003_001': 'Scratch databases/spills from three ended root universe runs; each new connection uses a new UUID and does not resume these databases.',
    '_tmp/release-universe-load-diagnostic-v1.duckdb': 'Completed read-only source-load diagnostic; rows/logs/receipts retained separately, never a producer input.',
    '_tmp/release-universe-load-diagnostic-v1.duckdb.wal': 'Associated ended diagnostic WAL, if any.',
    '_tmp/release-universe-year-diagnostic-v1.duckdb': 'Completed read-only year diagnostic superseded by authenticated2018unit; diagnostic receipt retained.',
    '_tmp/release-universe-year-diagnostic-v1.duckdb.wal': 'Associated ended diagnostic WAL, if any.',
    'fundamentals_v11/_interrupted_work-r2-20261003T211353Z': 'Obsolete unauthenticated interrupted derived preparation, superseded by separately verified144units; raw FSDS/archive/extract inputs preserved.',
}
rows = []
for relative, reason in targets.items():
    path = (root / relative).resolve()
    if not path.is_relative_to(root) or path == root:
        raise RuntimeError('Cleanup target escaped root')
    mentions = [value for value in dependencies if str(path).replace('\\', '/').casefold() in value or relative.casefold() in value]
    files, reparse, total = [], [], 0
    if path.exists():
        stack = [path]
        while stack:
            member = stack.pop()
            stat = member.lstat()
            if getattr(stat, 'st_file_attributes', 0) & 0x400:
                reparse.append(str(member))
                continue
            if member.is_dir():
                stack.extend(member.iterdir())
            elif member.is_file():
                files.append({'path': str(member), 'bytes': stat.st_size, 'mtime_ns': stat.st_mtime_ns})
                total += stat.st_size
                if len(files) > 20000:
                    raise RuntimeError('Cleanup audit file bound exceeded')
    rows.append({'path': str(path), 'exists': path.exists(), 'reason': reason,
                 'dependency_mentions': mentions, 'reparse_points': reparse,
                 'files': sorted(files, key=lambda row: row['path']), 'logical_bytes': total,
                 'eligible_for_explicit_removal': path.exists() and not mentions and not reparse})
result = {'status': 'AUDIT_ONLY_NO_DELETION', 'root': str(root),
          'authenticated_current_preparation_units': len(prepared['units']),
          'targets': rows, 'candidate_total_logical_bytes': sum(r['logical_bytes'] for r in rows if r['eligible_for_explicit_removal']),
          'protected': ['Raw/source archives and ledgers', 'Current144preparation units', 'Extract data/receipts',
                        'All universe generations/receipts/snapshots', 'Completed batch outputs', 'Foreign and active caches']}
C.write_json_atomic(out, result)
print(json.dumps({'status': result['status'], 'targets': [{k: row[k] for k in ('path', 'logical_bytes', 'eligible_for_explicit_removal', 'dependency_mentions', 'reparse_points')} for row in rows],
                  'candidate_total_logical_bytes': result['candidate_total_logical_bytes']}), flush=True)
