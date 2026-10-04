"""Bounded workspace/test-scratch size leads after a new low-disk refusal."""
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
output = base / 'disk-space-preflight-v4.json'
if output.exists():
    raise RuntimeError('Preserve prior disk evidence')
roots = [Path('C:/Users/natha/AppData/Local/Temp/pytest-of-natha'), Path('C:/atx')]
result = {'status': 'BOUNDED_DISK_METADATA_LEADS_NOT_DELETION_AUTHORIZATION',
          'drive_free_before_bytes': shutil.disk_usage('C:/atx').free,
          'roots': [], 'source_payloads_read': False, 'files_deleted': 0,
          'qualification': 'Sizes identify leads only. Reparse paths and Git/installed-dependency trees are not traversed. Foreign ownership, old PID names and file dates do not authorize deletion. Failed/forensic caches and original sources remain protected; any removal requires exact ownership/dependency and resolved-path checks.'}
started = time.monotonic()
for root in roots:
    report = {'path': str(root), 'exists': root.exists(), 'visited': 0,
              'files': 0, 'errors': [], 'skipped_directories': [], 'complete': True}
    result['roots'].append(report)
    if not root.exists():
        continue
    if root.is_symlink() or root.is_junction():
        raise RuntimeError('Reparse root')
    groups = collections.defaultdict(lambda: [0, 0])
    largest = []
    pending = [root]
    while pending:
        directory = pending.pop()
        try:
            with os.scandir(directory) as entries:
                for entry in entries:
                    report['visited'] += 1
                    if report['visited'] > 500000 or time.monotonic() - started > 45:
                        report['complete'] = False
                        pending.clear()
                        break
                    path = Path(entry.path)
                    metadata = entry.stat(follow_symlinks=False)
                    if getattr(metadata, 'st_file_attributes', 0) & 0x400:
                        report['skipped_directories'].append(str(path))
                        continue
                    if entry.is_dir(follow_symlinks=False):
                        if entry.name in {'.git', '.venv', 'node_modules'}:
                            report['skipped_directories'].append(str(path))
                        else:
                            pending.append(path)
                        continue
                    if not entry.is_file(follow_symlinks=False):
                        continue
                    report['files'] += 1
                    relative = path.relative_to(root).parts
                    depth = 1 if root.name == 'pytest-of-natha' else 3
                    key = '/'.join(relative[:depth] if len(relative) > depth else relative[:-1])
                    groups[key][0] += metadata.st_size
                    groups[key][1] += 1
                    item = (metadata.st_size, str(path))
                    if len(largest) < 30:
                        heapq.heappush(largest, item)
                    elif item > largest[0]:
                        heapq.heapreplace(largest, item)
        except OSError as error:
            report['errors'].append({'path': str(directory), 'error': str(error)})
            report['complete'] = False
    report['groups'] = [{'path': name, 'logical_bytes': values[0], 'files': values[1]}
                        for name, values in sorted(groups.items(), key=lambda row: row[1][0], reverse=True)]
    report['largest_files'] = [{'path': path, 'bytes': size} for size, path in sorted(largest, reverse=True)]
result['seconds'] = round(time.monotonic() - started, 3)
result['drive_free_after_bytes'] = shutil.disk_usage('C:/atx').free
with output.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': result['status'], 'seconds': result['seconds'],
                  'free_bytes': result['drive_free_after_bytes'],
                  'roots': [{key: row[key] for key in ('path','exists','visited','files','complete')}
                            for row in result['roots']]}), flush=True)
