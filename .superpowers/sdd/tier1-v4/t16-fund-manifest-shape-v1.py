"""Bounded retained-manifest schema inspection after native-contract refusal."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C

root = C.build_root()
out = Path(__file__).resolve().parent / 'task16-fund-manifest-shape-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous inspection')
result = {}
for stage in ('identity', 'sec_filings', 'reference'):
    path = root / stage / 'manifest.json'
    if path.stat().st_size > 8 * 1024 * 1024:
        raise RuntimeError('Manifest inspection bound exceeded')
    raw = C.read_json(path)
    fields = {}
    for key, value in raw.items():
        if isinstance(value, dict):
            fields[key] = {'type': 'dict', 'count': len(value), 'keys': list(value)[:15]}
        elif isinstance(value, list):
            fields[key] = {'type': 'list', 'count': len(value), 'examples': value[:2]}
        else:
            fields[key] = {'type': type(value).__name__, 'value': str(value)[:1024]}
    result[stage] = {'path': str(path), 'sha256': C.sha256_file(path),
                     'bytes': path.stat().st_size, 'fields': fields}
result['qualification'] = 'Manifest shape and byte identity only; native reader contract refusal is not waived.'
C.write_json_atomic(out, result)
print(json.dumps(result), flush=True)
