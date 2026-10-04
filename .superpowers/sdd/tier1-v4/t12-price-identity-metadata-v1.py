"""Read bounded Parquet footers only; no price, return or identity row values."""
import json
import os
import struct
from pathlib import Path
import pyarrow.parquet as pq
from atx_db.alpha_panel import common as C

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
workspace = Path(__file__).resolve().parent
out = workspace / 'task12-price-identity-metadata-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous metadata audit')
root = C.build_root()
paths = {'raw_vendor': C.TICKERHISTORY, 'prices_2020': root / 'prices/year=2020/prices.parquet',
         'identity_links': root / 'identity/link_table.parquet', 'security_lines': root / 'security_master/lines.parquet'}
rows = {}
for name, path in paths.items():
    path = path.resolve()
    if not path.exists():
        rows[name] = {'path': str(path), 'status': 'MISSING'}
        continue
    before = path.stat()
    with path.open('rb') as stream:
        stream.seek(-8, 2)
        footer = stream.read(8)
    if len(footer) != 8 or footer[4:] != b'PAR1':
        raise RuntimeError('Unexpected Parquet footer')
    metadata_bytes = struct.unpack('<I', footer[:4])[0]
    record = {'path': str(path), 'file_bytes': before.st_size, 'mtime_ns': before.st_mtime_ns,
              'footer_bytes': metadata_bytes}
    if metadata_bytes > 8 * 1024 * 1024:
        record['status'] = 'FOOTER_EXCEEDS_8MIB_BOUND'
    else:
        reader = pq.ParquetFile(path)
        record.update(status='SCHEMA_METADATA_ONLY', schema=str(reader.schema_arrow),
                      rows_metadata=reader.metadata.num_rows, row_groups=reader.metadata.num_row_groups)
        del reader
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError('File stat changed during metadata audit')
    rows[name] = record
result = {'status': 'BOUNDED_METADATA_ONLY', 'products': rows, 'value_rows_read': 0,
          'return_values_read_or_computed': False, 'http_attempts': 0,
          'qualification': 'Footer/schema leads only; no complete-file SHA or economic price/currency/class/clock qualification.'}
text = json.dumps(result, indent=2)
if len(text.encode()) > 128 * 1024:
    raise RuntimeError('Output bound exceeded')
out.write_text(text + '\n', encoding='utf-8')
print(json.dumps({'status': result['status'], 'products': {key: {k: value[k] for k in ('path', 'status', 'footer_bytes') if k in value} for key, value in rows.items()}}), flush=True)
