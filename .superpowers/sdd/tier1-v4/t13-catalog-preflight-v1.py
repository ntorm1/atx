"""Bounded retained-catalog schema sample; zero document reads or HTTP."""
import json
import os
from itertools import islice
from pathlib import Path
import pyarrow.parquet as pq
from atx_db.alpha_panel import common as C, events_sources as E

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
out = Path(__file__).resolve().parent / 'task13-catalog-preflight-v1.json'
if out.exists():
    raise RuntimeError('Preserve prior preflight')
path = E.catalog_path()
before = {'bytes': path.stat().st_size, 'sha256': C.sha256_file(path)}
reader = pq.ParquetFile(path)
rows = list(islice(E.iter_catalog(lambda row: bool(row.get('form')) and row['form'].startswith(('8-K', '10-Q'))), 3))
after = {'bytes': path.stat().st_size, 'sha256': C.sha256_file(path)}
if before != after:
    raise RuntimeError('Catalog drift')
result = {'status': 'RETAINED_SCHEMA_SAMPLE_ONLY', 'path': str(path.resolve()), 'identity': before,
          'total_rows_metadata': reader.metadata.num_rows, 'schema': str(reader.schema_arrow),
          'sample_rows': rows, 'sample_limit': 3, 'source_documents_read': 0, 'http_attempts': 0,
          'qualification': 'Catalog schema/provenance leads only; no declaration, class mapping, publication clock or source-truth certification.'}
out.write_text(json.dumps(result, indent=2, default=str) + '\n', encoding='utf-8')
print(json.dumps(result, default=str), flush=True)
