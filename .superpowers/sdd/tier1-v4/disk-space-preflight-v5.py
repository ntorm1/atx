"""Bounded metadata census of database workspace caches and generated areas."""
import collections
import heapq
import json
import os
from pathlib import Path
import shutil
import time

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
output = base / 'disk-space-preflight-v5.json'
if output.exists():
    raise RuntimeError('Preserve prior evidence')
workspace = Path('C:/atx/atx-db').resolve(strict=True)
roots = [workspace / name for name in (
    '.mypy_cache', '.pytest_cache', '.ruff_cache', 'scratch', 'tests_scratch',
    'data/tmp', 'data/staging', 'data/maintenance', 'data/alpha_panel',
)]
result = {'status': 'METADATA_ONLY_NO_DELETION_OR_OWNERSHIP_INFERENCE',
          'source_payloads_read': False, 'files_deleted': 0,
          'drive_free_before_bytes': shutil.disk_usage(workspace).free,
          'roots': []}
started = time.monotonic()
for root in roots:
    report = {'path': str(root), 'exists': root.exists(), 'files': 0,
              'visited': 0, 'logical_bytes': 0, 'complete': True,
              'errors': [], 'reparse_paths_skipped': 0}
    result['roots'].append(report)
    if not root.exists():
        continue
    if not root.resolve(strict=True).is_relative_to(workspace):
        raise RuntimeError('Root escaped workspace')
    for component in (root, *root.parents):
        if component.is_symlink() or component.is_junction():
            raise RuntimeError('Reparse ancestor')
        if component == workspace:
            break
    groups = collections.defaultdict(lambda: [0, 0])
    largest = []
    pending = [root]
    while pending:
        directory = pending.pop()
        try:
            with os.scandir(directory) as entries:
                for entry in entries:
                    report['visited'] += 1
                    if report['visited'] > 200000 or time.monotonic() - started > 45:
                        report['complete'] = False
                        pending.clear()
                        break
                    path = Path(entry.path)
                    metadata = entry.stat(follow_symlinks=False)
                    if getattr(metadata, 'st_file_attributes', 0) & 0x400:
                        report['reparse_paths_skipped'] += 1
                        continue
                    if entry.is_dir(follow_symlinks=False):
                        pending.append(path)
                    elif entry.is_file(follow_symlinks=False):
                        report['files'] += 1
                        report['logical_bytes'] += metadata.st_size
                        parts = path.relative_to(root).parts
                        key = '/'.join(parts[:2] if len(parts) > 2 else parts[:-1])
                        groups[key][0] += metadata.st_size
                        groups[key][1] += 1
                        item = (metadata.st_size, str(path))
                        if len(largest) < 25:
                            heapq.heappush(largest, item)
                        elif item > largest[0]:
                            heapq.heapreplace(largest, item)
        except OSError as error:
            report['errors'].append({'path': str(directory), 'error': str(error)})
            report['complete'] = False
    report['groups'] = [dict(path=name, logical_bytes=values[0], files=values[1])
                        for name, values in sorted(groups.items(), key=lambda row: row[1][0], reverse=True)[:60]]
    report['largest_files'] = [dict(path=path, bytes=size)
                             for size, path in sorted(largest, reverse=True)]
result['seconds'] = round(time.monotonic() - started, 3)
result['drive_free_after_bytes'] = shutil.disk_usage(workspace).free
with output.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({key: value for key, value in result.items() if key != 'roots'}), flush=True)
for row in result['roots']:
    print(json.dumps({key: row[key] for key in ('path','files','logical_bytes','complete')}), flush=True)
