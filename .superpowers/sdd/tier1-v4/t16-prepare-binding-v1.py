"""Assemble an explicit BYTE-ONLY binding from independently measured evidence.

Reads generated audit artifacts only. It does not inspect or certify source data.
"""
import hashlib
import json
from pathlib import Path

workspace = Path(__file__).resolve().parent
audit_path = workspace / 'task16-reader-byte-audit-v1.json'
preflight_path = workspace / 'task16-reader-preflight-v1.json'
out = workspace / 'task16-reader-binding-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous binding')
audit = json.loads(audit_path.read_text(encoding='utf-8'))
preflight = json.loads(preflight_path.read_text(encoding='utf-8'))
if audit['status'] != 'BYTE_MATCH_ONLY' or not audit['two_pass_equal'] or audit['passes'][0] != audit['passes'][1]:
    raise RuntimeError('Audit does not establish stable declared byte identities')
evidence = audit['passes'][1]
if evidence['errors'] or set(evidence['products']) != set(preflight['products']):
    raise RuntimeError('Incomplete or failed source evidence')
audit_sha = hashlib.sha256(audit_path.read_bytes()).hexdigest()
products = {}
for name, members in evidence['products'].items():
    producer = preflight['products'][name]['rule'].get('manifest')
    products[name] = {
        'files': {member: evidence['files'][member] for member in members},
        'source': f'Retained alpha_panel/v1 reader product {name}; producer declaration {producer or "standalone calendar"}. Actual retained bytes verified against all declared producer members; original-source economic qualification remains unestablished.',
        'clock': 'Preserve the native reader clock and historical vintage limitations. This byte audit does not certify row availability or permit a clock override; strict GC1 admission and original source gaps remain binding.',
        'admission': f'BYTE_IDENTITY_ONLY. Independent root two-pass audit {audit_path.as_posix()} SHA256={audit_sha}: identical complete actual file sets, zero producer mismatches. Require reviewed R15 source/code/key/counter validation at build and consumption. No economic, membership or production certification; original French/I4 FAIL remains.',
    }
out.write_text(json.dumps({'version': 'reader_binding_v1', 'products': products}, indent=2, sort_keys=True) + '\n', encoding='utf-8')
print(json.dumps({'status': 'BYTE_ONLY_BINDING_REQUIRES_REVIEW', 'products': len(products),
                  'sha256': hashlib.sha256(out.read_bytes()).hexdigest(), 'path': str(out)}))
