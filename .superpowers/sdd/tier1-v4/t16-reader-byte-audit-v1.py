"""Two-pass actual-byte audit of static reader products and producer declarations.

This writes evidence only, not a release binding or an economic qualification.
Run under the memory guard with exclusive source-data scheduling.
"""
import hashlib
import json
from pathlib import Path

from atx_db.alpha_panel import common as C

workspace = Path(__file__).resolve().parent
preflight_path = workspace / 'task16-reader-preflight-v1.json'
out = workspace / 'task16-reader-byte-audit-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous audit')
preflight = C.read_json(preflight_path)
root = Path(preflight['root']).resolve()


def identity(path):
    before = path.stat()
    result = {'sha256': C.sha256_file(path), 'bytes': before.st_size}
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError(f'Changed during hashing: {path}')
    return result


def one_pass():
    manifests, declarations, product_files = {}, {}, {}
    errors = []
    for name, product in preflight['products'].items():
        actual = sorted(p.relative_to(root).as_posix()
                        for p in root.glob(product['rule']['pattern']) if p.is_file())
        if actual != sorted(item['path'] for item in product['files']):
            errors.append({'product': name, 'error': 'inventory_changed_since_preflight'})
        product_files[name] = actual
        manifest = product['rule'].get('manifest')
        if manifest and manifest not in manifests:
            path = root / manifest
            manifests[manifest] = identity(path)
            doc = C.read_json(path)
            files = doc.get('files')
            if not isinstance(files, dict) or not files:
                raise RuntimeError(f'Invalid producer inventory: {manifest}')
            for member, recorded in files.items():
                member_path = (path.parent / member).resolve()
                if not member_path.is_relative_to(path.parent.resolve()):
                    raise RuntimeError(f'Escaped producer member: {manifest}/{member}')
                relative = member_path.relative_to(root).as_posix()
                declarations.setdefault(relative, []).append({
                    'manifest': manifest,
                    'identity': {'sha256': recorded.get('sha256'), 'bytes': recorded.get('bytes')},
                })
    all_files = set(declarations)
    for members in product_files.values():
        all_files.update(members)
    actual_files = {}
    for relative in sorted(all_files):
        path = (root / relative).resolve()
        if not path.is_relative_to(root):
            raise RuntimeError(f'Escaped reader product: {relative}')
        if not path.is_file():
            errors.append({'file': relative, 'error': 'declared_file_missing'})
            continue
        actual_files[relative] = identity(path)
        for declared in declarations.get(relative, []):
            if actual_files[relative] != declared['identity']:
                errors.append({'file': relative, 'manifest': declared['manifest'],
                               'error': 'actual_bytes_disagree_with_manifest',
                               'declared': declared['identity'], 'actual': actual_files[relative]})
    for manifest, before in manifests.items():
        if identity(root / manifest) != before:
            raise RuntimeError(f'Manifest changed during pass: {manifest}')
    return {'products': product_files, 'files': actual_files, 'manifests': manifests,
            'declarations': declarations, 'errors': errors}


passes = []
for number in (1, 2):
    result = one_pass()
    passes.append(result)
    print(json.dumps({'pass': number, 'files': len(result['files']),
                      'logical_bytes': sum(row['bytes'] for row in result['files'].values()),
                      'errors': len(result['errors'])}), flush=True)
stable = passes[0] == passes[1]
report = {'status': 'BYTE_MATCH_ONLY' if stable and not passes[1]['errors'] else 'FAIL',
          'root': str(root), 'two_pass_equal': stable,
          'preflight': identity(preflight_path), 'helper': identity(Path(__file__)),
          'passes': passes, 'source_quality_certified': False,
          'qualification': 'Content identity only. No keys, clocks, source quality or admission verified.'}
C.write_json_atomic(out, report)
print(json.dumps({'status': report['status'], 'two_pass_equal': stable,
                  'errors': len(passes[1]['errors']), 'output': str(out)}), flush=True)
raise SystemExit(0 if report['status'] == 'BYTE_MATCH_ONLY' else 2)
