import datetime as dt
import hashlib
import json
from pathlib import Path
import duckdb
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

root = Path(__file__).resolve().parent
source = Path('C:/Users/natha/Downloads/TickerHistory3.parquet')
expected = '0ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae'
captured = source.stat()
digest = hashlib.sha256()
with source.open('rb') as handle:
    for chunk in iter(lambda: handle.read(1 << 20), b''):
        digest.update(chunk)
if digest.hexdigest() != expected:
    raise ValueError('raw source pin mismatch')
columns = ['tradingDate', 'securityID', 'close', 'volume', 'cumulReturnFactor']
start, end = dt.date(2020, 1, 2), dt.date(2020, 1, 11)
rows = []
for batch in pq.ParquetFile(source).iter_batches(batch_size=65536, columns=columns, use_threads=False):
    selected = pc.and_(pc.equal(batch.column(1), pa.scalar(39621, pa.int64())),
                       pc.and_(pc.greater_equal(batch.column(0), pa.scalar(start)),
                               pc.less(batch.column(0), pa.scalar(end))))
    rows.extend(batch.filter(pc.fill_null(selected, False)).to_pylist())
if len(rows) > 200:
    raise ValueError('diagnostic result unexpectedly large')
current = source.stat()
if (captured.st_size, captured.st_mtime_ns, captured.st_ino) != (current.st_size, current.st_mtime_ns, current.st_ino):
    raise ValueError('raw source changed')
connection = duckdb.connect(str(root / 'recent-projection-v1/projection.duckdb'), read_only=True)
connection.execute('SET threads=1')
connection.execute("SET memory_limit='128MiB'")
cached = connection.execute('SELECT d,id,raw,volume,factor FROM rows WHERE id=? AND d>=? AND d<? ORDER BY d', [39621, start, end]).fetchall()
connection.close()
report = dict(source_sha256=expected, instrument_id=39621, start=str(start), end_exclusive=str(end),
              raw_rows=sorted(rows, key=lambda r: r['tradingDate']),
              private_non_authoritative_cache_rows=cached,
              scope='fixed source records only; no portfolio return, signal selection or membership alteration')
encoded = json.dumps(report, default=str, indent=2, allow_nan=False) + '\n'
with (root / 'first-refusal-source-audit.json').open('x', encoding='utf-8') as out:
    out.write(encoded)
print(encoded)
