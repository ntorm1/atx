"""Bounded original identity-manifest declaration inspection, not admission."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C

path = C.build_root() / 'identity/manifest.json'
output = Path(__file__).resolve().parent / 'task16-identity-manifest-pins-v1.json'
if output.exists() or path.stat().st_size > 1024 * 1024:
    raise RuntimeError('Evidence/output bound refusal')
expected = '0df3dc103b49f8494002b48983c6895a30f707b4a75eb61f6d6c1f9f169f2e76'
if C.sha256_file(path) != expected:
    raise RuntimeError('Identity manifest changed')
raw = C.read_json(path)
result = {'path': str(path), 'sha256': expected, 'outputs': raw['outputs'],
          'sources': raw['sources'], 'stage': raw['stage'], 'code_version': raw['code_version'],
          'qualification': 'Producer declarations only; referenced files not read or qualified by this inspection.'}
if C.sha256_file(path) != expected:
    raise RuntimeError('Identity manifest changed during inspection')
C.write_json_atomic(output, result)
print(json.dumps(result), flush=True)
