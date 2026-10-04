"""Bounded read-only publication metadata audit; never reads feature/return rows."""
import hashlib
import json
import os
from pathlib import Path

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
root = Path('C:/atx/atx-db/data/alpha_panel/v1')
out = Path(__file__).with_name('gold-readiness-preflight-v2.json')
if out.exists():
    raise RuntimeError('Preserve prior audit')
stages = ('gold', 'golden_panel', 'characteristics', 'char_monthly',
          'panel_goldready', 'labels_goldready')
results = []
for name in stages:
    directory = root / name
    row = {'stage': name, 'path': str(directory), 'directory_exists': directory.is_dir(),
           'manifest_exists': False, 'value_rows_read': 0, 'data_payload_hashes_checked': False}
    if directory.is_symlink():
        row['status'] = 'REPARSE_TARGET_NOT_INSPECTED'
        results.append(row)
        continue
    manifest = directory / 'manifest.json'
    if manifest.is_file():
        before = manifest.stat()
        if before.st_size > 4 * 1024 * 1024:
            raise RuntimeError('Manifest exceeds4MiB metadata bound')
        payload = manifest.read_bytes()
        doc = json.loads(payload)
        if manifest.read_bytes() != payload:
            raise RuntimeError('Manifest changed during metadata audit')
        row.update(manifest_exists=True, manifest_sha256=hashlib.sha256(payload).hexdigest(),
                   manifest_bytes=len(payload), manifest_status_reported=doc.get('status'),
                   declared_file_count=len(doc.get('files', {})),
                   declared_schema=doc.get('schema'),
                   reported_gate_status=doc.get('gate_status'))
    if directory.is_dir():
        entries = []
        with os.scandir(directory) as stream:
            for entry in stream:
                if len(entries) >= 256:
                    raise RuntimeError('Stage immediate entry bound exceeded')
                entries.append({'name': entry.name, 'directory': entry.is_dir(follow_symlinks=False),
                                'bytes': entry.stat(follow_symlinks=False).st_size if entry.is_file(follow_symlinks=False) else None})
        row['immediate_entries'] = sorted(entries, key=lambda x: x['name'])
    row['status'] = 'MANIFEST_METADATA_ONLY_UNQUALIFIED' if row['manifest_exists'] else 'NO_ROOT_PUBLICATION_MANIFEST'
    results.append(row)
report = {'status': 'GOLD_PUBLICATION_METADATA_ONLY', 'stages': results,
          'scope': 'Six exact canonical stage roots; immediate entries and root manifests only. No alternate root, generation payload, source/PIT/quality/evaluation or consumer certification.',
          'feature_or_return_rows_read': 0, 'http_attempts': 0}
encoded = json.dumps(report, indent=2).encode()
if len(encoded) > 128 * 1024:
    raise RuntimeError('Output exceeds128KiB bound')
with out.open('xb') as stream:
    stream.write(encoded + b'\n')
print(json.dumps([{'stage': r['stage'], 'exists': r['directory_exists'], 'status': r['status'],
                   'manifest_status': r.get('manifest_status_reported'),
                   'immediate_entries': len(r.get('immediate_entries', []))} for r in results]))
