"""Bounded, read-only disk metadata; never grant deletion from file size alone."""
import collections
import heapq
import json
import os
import shutil
from pathlib import Path

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
workspace = Path(__file__).resolve().parent
output = workspace / 'disk-space-preflight-v3.json'
if output.exists():
    raise RuntimeError('Preserve existing inventory')
root = Path('C:/atx/atx-db/data').resolve()
if root != Path('C:/atx/atx-db/data'):
    raise RuntimeError('Unexpected data root')
free_before = shutil.disk_usage(root).free
groups = collections.defaultdict(lambda: [0, 0])
largest = []
reparse, errors, files, visited = [], [], 0, 0
pending = [root]
owned_scopes = [root / 'alpha_panel/v1/fundamentals_release_v1/_spill',
                root / 'alpha_panel/v1/_tmp/fund-release-extract-verify']
owned = {str(p): [] for p in owned_scopes}
complete = True
while pending:
    directory = pending.pop()
    try:
        with os.scandir(directory) as entries:
            for entry in entries:
                visited += 1
                if visited > 500000:
                    complete = False
                    break
                path = Path(entry.path)
                stat = entry.stat(follow_symlinks=False)
                if getattr(stat, 'st_file_attributes', 0) & 0x400:
                    reparse.append(str(path))
                    continue
                if entry.is_dir(follow_symlinks=False):
                    pending.append(path)
                    continue
                if not entry.is_file(follow_symlinks=False):
                    continue
                files += 1
                parts = path.relative_to(root).parts
                key = '/'.join(parts[:3] if len(parts) > 3 else parts[:-1])
                groups[key][0] += stat.st_size
                groups[key][1] += 1
                item = (stat.st_size, str(path))
                if len(largest) < 25:
                    heapq.heappush(largest, item)
                elif item > largest[0]:
                    heapq.heapreplace(largest, item)
                for scope in owned_scopes:
                    if path.is_relative_to(scope):
                        rows = owned[str(scope)]
                        if len(rows) >= 20000:
                            raise RuntimeError('Owned scratch inventory exceeded')
                        rows.append({'path': str(path), 'bytes': stat.st_size,
                                     'mtime_ns': stat.st_mtime_ns})
    except OSError as exc:
        errors.append({'path': str(directory), 'error': str(exc)})
    if not complete:
        break
result = {
    'status': 'READ_ONLY_DISK_INVENTORY_NOT_DELETION_AUTHORIZATION',
    'root': str(root), 'complete': complete and not errors,
    'visited_entries': visited, 'files': files,
    'logical_bytes': sum(row[0] for row in groups.values()),
    'drive_free_before_bytes': free_before,
    'drive_free_after_bytes': shutil.disk_usage(root).free,
    'groups': [{'path': key, 'logical_bytes': value[0], 'files': value[1]}
               for key, value in sorted(groups.items(), key=lambda row: row[1][0], reverse=True)],
    'largest_files': [{'path': path, 'bytes': size} for size, path in sorted(largest, reverse=True)],
    'owned_scratch_inventory': owned,
    'reparse_paths_not_followed': reparse, 'errors': errors,
    'qualification': 'File metadata only. No source payload read, hashes, deletes, or foreign ownership assumptions. Removal requires separate exact-path/dead-writer/dependency proof.'}
with output.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({key: result[key] for key in ('status', 'complete', 'files', 'logical_bytes', 'drive_free_before_bytes', 'drive_free_after_bytes')}))
print(json.dumps({'largest_groups': result['groups'][:15],
                  'owned_scratch_bytes': {key: sum(row['bytes'] for row in value) for key, value in owned.items()}}))
