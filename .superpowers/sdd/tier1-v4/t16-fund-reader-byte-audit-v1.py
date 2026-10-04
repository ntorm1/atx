"""Guarded two-pass independent fund reader/archive byte evidence; no binding."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C, fund_batch_release as R

workspace = Path(__file__).resolve().parent
preflight_path = workspace / 'task16-fund-reader-preflight-v1.json'
output = workspace / 'task16-fund-reader-byte-audit-v1.json'
if output.exists():
    raise RuntimeError('Preserve previous audit')
preflight = C.read_json(preflight_path)
root = Path(preflight['paths']['root']['path']).resolve()
if preflight['missing']:
    raise RuntimeError('Preflight incomplete')

def one_pass():
    products, manifests, declarations, errors = {}, {}, {}, []
    selected = set()
    for name, rule in R.reader_contract(root).items():
        paths = R._actual(rule)
        if name == 'fx':
            paths = [root / 'reference/fx_daily.parquet']
        if rule.get('optional') and not any(path.exists() for path in paths):
            if preflight['products'][name] is not None:
                raise RuntimeError('Optional inventory changed: ' + name)
            products[name] = None
            continue
        actual = sorted(str(p.resolve()) for p in paths)
        expected = sorted(row['path'] for row in preflight['products'][name])
        if actual != expected or not actual:
            raise RuntimeError('Reader inventory changed: ' + name)
        products[name] = actual
        selected.update(actual)
        if name.endswith('_manifest'):
            manifest = Path(actual[0])
            manifests[str(manifest)] = R.identity(manifest)
            files = C.read_json(manifest).get('files')
            if not isinstance(files, dict) or not files:
                raise RuntimeError('Invalid producer inventory: ' + str(manifest))
            for relative, recorded in files.items():
                member = R._member(manifest.parent, relative)
                declarations.setdefault(str(member), []).append({
                    'manifest': str(manifest),
                    'identity': {'sha256': recorded.get('sha256'), 'bytes': recorded.get('bytes')}})
    archive = preflight['paths']['archive']['path']
    selected.update(declarations)
    selected.add(archive)
    files = {}
    for name in sorted(selected):
        path = Path(name)
        if not path.is_file():
            errors.append({'file': name, 'error': 'missing'})
            continue
        files[name] = R.identity(path)
        for item in declarations.get(name, []):
            if files[name] != item['identity']:
                errors.append({'file': name, 'error': 'producer_manifest_mismatch',
                               'manifest': item['manifest'], 'recorded': item['identity'],
                               'actual': files[name]})
    if any(R.identity(path) != pin for path, pin in manifests.items()):
        raise RuntimeError('Manifest changed during pass')
    return {'products': products, 'files': files, 'manifests': manifests,
            'declarations': declarations, 'archive': archive, 'errors': errors}

passes = []
for number in (1, 2):
    result = one_pass()
    passes.append(result)
    print(json.dumps({'pass': number, 'files': len(result['files']),
                      'logical_bytes': sum(p['bytes'] for p in result['files'].values()),
                      'errors': len(result['errors'])}), flush=True)
stable = passes[0] == passes[1]
status = 'BYTE_MATCH_ONLY' if stable and not passes[1]['errors'] else 'FAIL'
report = {'status': status, 'two_pass_equal': stable, 'passes': passes,
          'preflight': R.identity(preflight_path), 'helper': R.identity(Path(__file__)),
          'source_quality_certified': False,
          'qualification': 'Only reader/archive byte identities. Preparation, archive/extraction semantics, keys, source economics and original gates require native admission and independent qualification.'}
C.write_json_atomic(output, report)
print(json.dumps({'status': status, 'errors': len(passes[1]['errors'])}), flush=True)
raise SystemExit(0 if status == 'BYTE_MATCH_ONLY' else 2)
