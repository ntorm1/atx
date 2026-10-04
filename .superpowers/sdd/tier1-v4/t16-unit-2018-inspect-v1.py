"""Guarded copy of completed unit metadata plus fresh payload identity check."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C

workspace = Path(__file__).resolve().parent
out = workspace / 'task16-unit-2018-inspection-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous inspection')
generation = C.build_root() / 'universe_release_v1/generations/r15_20261003_001'
receipt_path = generation / 'year=2018/receipt.json'
payload = generation / 'year=2018/universe.parquet'
receipt_before = C.sha256_file(receipt_path)
receipt = C.read_json(receipt_path)
before = payload.stat()
actual = {'sha256': C.sha256_file(payload), 'bytes': before.st_size}
after = payload.stat()
if actual != receipt['payload'] or C.sha256_file(receipt_path) != receipt_before or (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
    raise RuntimeError('Completed unit changed')
result = {'status': 'PARTIAL_CANDIDATE_UNIT_BYTE_MATCH', 'generation': str(generation),
          'year': 2018, 'rows': receipt['keys']['rows'], 'sessions': len(receipt['keys']['dates']),
          'panel_keys_sha256': receipt['keys']['panel_keys_sha256'],
          'payload': actual, 'receipt_sha256': receipt_before,
          'snapshot_sha256': receipt['snapshot_sha256'],
          'final_manifest_exists': (generation / 'manifest.json').exists(),
          'current_pointer_exists': (generation.parents[1] / 'current.json').exists(),
          'admission_scope': 'Root build already checked exact keys/schema/clocks and replayed native counters; this inspection rechecks bytes only. Eight years and all final gates remain missing.',
          'source_quality_certified': False, 'readiness': 'NOT_PRODUCTION_READY'}
C.write_json_atomic(out, result)
print(json.dumps(result), flush=True)
