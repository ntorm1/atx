"""Independent two-pass reader bytes with explicit native Stage I declarations."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C, fund_batch_release as R, identity_links as I

workspace = Path(__file__).resolve().parent
preflight_path = workspace / 'task16-fund-reader-preflight-v1.json'
output = workspace / 'task16-fund-reader-byte-audit-v2.json'
if output.exists():
    raise RuntimeError('Preserve previous audit')
preflight = C.read_json(preflight_path)
root = Path(preflight['paths']['root']['path']).resolve()
if preflight['missing']:
    raise RuntimeError('Preflight incomplete')

def one_pass():
    products, manifests, declarations, errors = {}, {}, {}, []
    selected = set()
    def declare(path, pin, manifest):
        declarations.setdefault(str(path), []).append({
            'manifest': str(manifest), 'identity': {'sha256': pin.get('sha256'), 'bytes': pin.get('bytes')}})
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
        if actual != sorted(row['path'] for row in preflight['products'][name]) or not actual:
            raise RuntimeError('Reader inventory changed: ' + name)
        products[name] = actual
        selected.update(actual)
        if not name.endswith('_manifest'):
            continue
        manifest = Path(actual[0])
        manifests[str(manifest)] = R.identity(manifest)
        doc = C.read_json(manifest)
        if name == 'identity_manifest':
            if (doc.get('stage'), doc.get('code_version'), doc.get('rule')) != ('I', 'identity-links-v1', 'r4-links-asof-v1') or 'files' in doc:
                raise RuntimeError('Unknown or ambiguous Stage I schema')
            if set(doc.get('outputs', {})) != {'links.parquet'}:
                raise RuntimeError('Wrong Stage I output inventory')
            source_paths = {'security_company_links': I.LINKS_SRC, 'security_permanent_ids': I.PERM_IDS_SRC, 'r4_manifest': I.R4_MANIFEST}
            if set(doc.get('sources', {})) != set(source_paths):
                raise RuntimeError('Wrong Stage I source inventory')
            pairs = [(root / 'identity/links.parquet', doc['outputs']['links.parquet'])]
            pairs += [(source_paths[key], pin) for key, pin in doc['sources'].items()]
            for expected, pin in pairs:
                raw_path = pin.get('path')
                if not isinstance(raw_path, str) or not Path(raw_path).is_absolute() or Path(raw_path).resolve() != expected.resolve():
                    raise RuntimeError('Stage I declared absolute path mismatch')
                declare(expected.resolve(), pin, manifest)
        else:
            files = doc.get('files')
            if not isinstance(files, dict) or not files:
                raise RuntimeError('Invalid generic producer inventory')
            for relative, pin in files.items():
                declare(R._member(manifest.parent, relative), pin, manifest)
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
                               'manifest': item['manifest'], 'recorded': item['identity'], 'actual': files[name]})
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
          'qualification': 'Only reader/archive byte identities. Native admission must verify typed schemas/rows/keys, complete preparation, archive/extraction replay, source economics and original gates.'}
C.write_json_atomic(output, report)
print(json.dumps({'status': status, 'errors': len(passes[1]['errors'])}), flush=True)
raise SystemExit(0 if status == 'BYTE_MATCH_ONLY' else 2)
