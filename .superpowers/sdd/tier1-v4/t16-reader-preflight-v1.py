"""Guarded metadata-only inventory; never supplies production source admission."""
import json
from pathlib import Path

from atx_db.alpha_panel import common as C, release_inputs as R

out = Path(__file__).resolve().parent / 'task16-reader-preflight-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous preflight')
root = C.build_root()
products = {}
manifests = {}
for name, rule in R.reader_contract().items():
    members = []
    for path in sorted(root.glob(rule['pattern'])):
        if path.is_file():
            stat = path.stat()
            members.append({'path': path.relative_to(root).as_posix(), 'bytes': stat.st_size,
                            'mtime_ns': stat.st_mtime_ns})
    manifest = rule.get('manifest')
    if manifest and manifest not in manifests:
        path = root / manifest
        record = {'exists': path.is_file()}
        if path.is_file():
            stat = path.stat()
            doc = C.read_json(path)
            inventory = doc.get('files')
            record.update(bytes=stat.st_size, mtime_ns=stat.st_mtime_ns,
                          declared_count=len(inventory) if isinstance(inventory, dict) else None,
                          stage=doc.get('stage'), code_version=doc.get('code_version'))
            record['declared_consumed'] = {
                item['path']: (isinstance(inventory, dict) and
                               Path(item['path']).relative_to(Path(manifest).parent).as_posix() in inventory)
                for item in members
            }
        manifests[manifest] = record
    if manifest and manifests[manifest].get('exists'):
        inventory = C.read_json(root / manifest).get('files')
        manifests[manifest]['declared_consumed'].update({
            item['path']: (isinstance(inventory, dict) and
                           Path(item['path']).relative_to(Path(manifest).parent).as_posix() in inventory)
            for item in members
        })
    products[name] = {'rule': rule, 'files': members}
result = {'status': 'METADATA_ONLY_NOT_AUTHENTICATED', 'root': str(root),
          'products': products, 'manifests': manifests,
          'missing_products': [name for name, row in products.items() if not row['files']],
          'missing_manifests': [name for name, row in manifests.items() if not row['exists']],
          'source_quality_certified': False,
          'qualification': 'No actual content SHA, row keys, clocks or economics verified.'}
C.write_json_atomic(out, result)
print(json.dumps({'status': result['status'], 'products': len(products),
                  'files': sum(len(row['files']) for row in products.values()),
                  'missing_products': result['missing_products'],
                  'missing_manifests': result['missing_manifests']}, sort_keys=True))
