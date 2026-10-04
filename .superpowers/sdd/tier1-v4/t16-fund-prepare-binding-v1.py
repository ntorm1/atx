"""Prepare an explicitly unqualified candidate binding from independent evidence."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C, fund_batch_release as R

workspace = Path(__file__).resolve().parent
output = workspace / 'task16-fund-reader-binding-v1.json'
audit_path = workspace / 'task16-fund-reader-byte-audit-v2.json'
preflight_path = workspace / 'task16-fund-reader-preflight-v1.json'
if output.exists():
    raise RuntimeError('Preserve previous binding')
audit = C.read_json(audit_path)
preflight = C.read_json(preflight_path)
if audit['status'] != 'BYTE_MATCH_ONLY' or not audit['two_pass_equal']:
    raise RuntimeError('Independent byte audit failed')
if audit['preflight'] != R.identity(preflight_path):
    raise RuntimeError('Preflight evidence changed')
evidence = audit['passes'][1]
if evidence['errors'] or evidence != audit['passes'][0]:
    raise RuntimeError('Unstable or mismatched byte evidence')
paths = preflight['paths']
root = Path(paths['root']['path']).resolve()
products = {}
for name, selected in evidence['products'].items():
    if selected is None:
        products[name] = None
        continue
    products[name] = {
        'source': 'Explicit actual native reader ' + name + '; independently measured two-pass actual bytes and producer declarations in ' + str(audit_path),
        'clock': 'Retained native clocks must be replayed and independently qualified; this byte evidence grants no point-in-time or public-availability certification.',
        'admission': 'BYTE_MATCH_ONLY for an isolated NOT_PRODUCTION_READY candidate. Source economics, original gates and downstream consumer use remain unqualified.',
        'files': {path: evidence['files'][path] for path in selected}}
archive_path = evidence['archive']
binding = {'version': R.VERSION, 'semantic_producer': R.SEMANTIC_STAGE,
           **{name: paths[name]['path'] for name in ('root', 'prep_root', 'extract_root', 'fsds_root')},
           'archive': {'path': archive_path, **evidence['files'][archive_path]},
           'products': products,
           'external_evidence': {'byte_audit': {'path': str(audit_path), **R.identity(audit_path)},
                                 'preflight': {'path': str(preflight_path), **R.identity(preflight_path)}},
           'readiness': 'NOT_PRODUCTION_READY', 'source_quality_certified': False}
if set(products) != set(R.reader_contract(root)):
    raise RuntimeError('Reader contract changed')
C.write_json_atomic(output, binding)
print(json.dumps({'output': str(output), 'identity': R.identity(output),
                  'products': len(products), 'qualification': 'BYTE_MATCH_ONLY_NOT_ADMITTED'}), flush=True)
