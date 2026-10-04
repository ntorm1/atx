"""Read-only guarded reconciliation after a host-stopped candidate continuation."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C

workspace = Path(__file__).resolve().parent
out = workspace / 'task16-interruption-inspection-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous inspection')
stage = C.build_root() / 'universe_release_v1'
generation = stage / 'generations/r15_20261003_001'
members = {}
for path in sorted(generation.rglob('*')):
    if path.is_file():
        before = path.stat()
        row = {'bytes': before.st_size, 'sha256': C.sha256_file(path)}
        after = path.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise RuntimeError(f'Candidate changed during inspection: {path}')
        if path.name == 'receipt.json':
            doc = C.read_json(path)
            row['receipt'] = {'year': doc.get('year'), 'snapshot_sha256': doc.get('snapshot_sha256'),
                              'payload': doc.get('payload'), 'rows': doc.get('keys', {}).get('rows')}
        members[path.relative_to(generation).as_posix()] = row
stage_files = {}
for path in stage.iterdir():
    if path.is_file():
        stage_files[path.name] = {'bytes': path.stat().st_size, 'sha256': C.sha256_file(path),
                                 'text': path.read_text(encoding='utf-8') if path.stat().st_size < 10000 else None}
result = {'status': 'INTERRUPTED_CANDIDATE_READ_ONLY_INVENTORY', 'generation': str(generation),
          'members': members, 'stage_files': stage_files,
          'final_manifest_exists': (generation / 'manifest.json').exists(),
          'current_pointer_exists': (stage / 'current.json').exists(),
          'readiness': 'NOT_PRODUCTION_READY',
          'qualification': 'SHA inventory only, no resume admission or completion claim.'}
C.write_json_atomic(out, result)
print(json.dumps(result), flush=True)
