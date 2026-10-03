"""Correct metadata evidence without changing the original preservation receipt."""
import datetime as dt
import json
from pathlib import Path
from atx_db.alpha_panel import common as C

workspace = Path(__file__).resolve().parent
old_path = workspace / 'logs/t5-preserved-batch-0001-v1.json'
out = workspace / 'logs/t5-preserved-batch-0001-audit-v2.json'
if out.exists():
    raise RuntimeError('Preserve previous audit')
old = json.loads(old_path.read_text(encoding='utf-8-sig'))
path = Path(old['destination'])
expected = Path('C:/atx/atx-db/data/alpha_panel/v1/fundamentals_v11/_interrupted_batch_0001-20261003T2317Z/history-0001.parquet.partial')
if path.resolve() != expected.resolve():
    raise RuntimeError('Unexpected preserved file')
before = path.stat()
sha = C.sha256_file(path)
after = path.stat()
if sha != old['sha256'] or (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
    raise RuntimeError('Preserved file changed')
result = {'status': 'PRESERVED_PARTIAL_BYTE_IDENTITY_RECHECKED',
          'path': str(path), 'sha256': sha, 'bytes': after.st_size,
          'preserved_file_mtime_utc': dt.datetime.fromtimestamp(after.st_mtime, dt.UTC).isoformat(),
          'prior_receipt': {'path': str(old_path), 'sha256': C.sha256_file(old_path)},
          'correction': 'Prior PowerShell FileInfo was accessed after moving its path; null size and 1601 timestamp are invalid metadata. Prior before/after SHA comparison was valid. This observes current preserved metadata and does not fabricate a prior source clock.',
          'qualification': 'Forensic partial only; no valid batch or source-clock certification.'}
C.write_json_atomic(out, result)
print(result)
