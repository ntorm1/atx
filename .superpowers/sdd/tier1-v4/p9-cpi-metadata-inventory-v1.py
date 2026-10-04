"""Bounded read-only configured macro metadata; never a CPI admission."""
import json
import os
from pathlib import Path
import threading
import uuid
import duckdb
from atx_db.connection import resolve_default_db_path

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
output = base / 'p9-cpi-metadata-inventory-v1.json'
if output.exists():
    raise RuntimeError('Preserve prior inventory')
database = resolve_default_db_path().absolute()
result = {
    'status': 'CONFIGURED_CPI_METADATA_LEADS_ONLY', 'database': str(database),
    'exists': database.exists(), 'database_opened': False,
    'macro_values_read': False, 'original_source_payloads_read': False,
    'network_attempts': 0, 'gate_promotion': False,
    'qualification': 'Only the configured database metadata is inventoried. Presence, latest-revision series descriptions, source URLs and stored timestamps do not establish NSA original publication vintages or original release clocks. Absence here does not establish absence elsewhere.',
}


def stat(path):
    row = path.stat()
    return {'bytes': row.st_size, 'mtime_ns': row.st_mtime_ns}


def rows(con, sql, parameters=()):
    cursor = con.execute(sql, parameters)
    names = [column[0] for column in cursor.description]
    data = cursor.fetchmany(1001)
    if len(data) > 1000:
        raise RuntimeError('Bounded metadata result exceeds1000rows')
    return [dict(zip(names, row, strict=True)) for row in data]


if database.exists():
    current = database
    while current != current.parent:
        if current.is_symlink() or current.is_junction():
            raise RuntimeError('Reparse warehouse path requires separate inspection')
        current = current.parent
    if not database.is_file():
        raise RuntimeError('Configured warehouse is not a regular file')
    initial = stat(database)
    spill = base / 'scratch' / ('cpi-metadata-' + uuid.uuid4().hex)
    spill.parent.mkdir(exist_ok=True)
    # No store initialization, migration, loader, network, or database write.
    with duckdb.connect(str(database), read_only=True, config={
        'memory_limit': '128MB', 'threads': 1, 'TimeZone': 'UTC',
        'temp_directory': str(spill), 'max_temp_directory_size': '512MB',
        'preserve_insertion_order': False,
    }) as con:
        timer = threading.Timer(45, con.interrupt)
        timer.daemon = True
        timer.start()
        try:
            con.execute('BEGIN TRANSACTION')
            result['database_opened'] = True
            result['tables'] = rows(con, "SELECT table_schema,table_name FROM information_schema.tables WHERE table_name IN ('macro_series','macro_observations','raw_source_files') ORDER BY table_schema,table_name")
            result['columns'] = rows(con, "SELECT table_schema,table_name,column_name,data_type FROM information_schema.columns WHERE table_name IN ('macro_series','macro_observations','raw_source_files') ORDER BY table_schema,table_name,ordinal_position")
            present = {row['table_name'] for row in result['tables'] if row['table_schema'] == 'main'}
            if 'macro_series' in present:
                result['series_metadata'] = rows(con, 'SELECT * FROM main.macro_series ORDER BY source,series_id LIMIT 1001')
            if 'raw_source_files' in present:
                result['source_metadata'] = rows(con, "SELECT source_id,dataset_id,source_url,cache_path,sha256,byte_count,fetched_at,status,metadata_json FROM main.raw_source_files WHERE dataset_id='fred_macro' OR source_url LIKE '%bls.gov/%' OR source_url LIKE '%CUUR0000SA0%' OR source_url LIKE '%CPIAUCNS%' ORDER BY dataset_id,source_url LIMIT 1001")
            con.execute('ROLLBACK')
        finally:
            timer.cancel()
    if stat(database) != initial:
        raise RuntimeError('Warehouse stat changed during metadata inspection; preserve attempt')
    result['database_stat_before_after'] = initial
    result['database_content_hashed'] = False
    result['private_spill_path'] = str(spill)
    result['private_spill_remains'] = spill.exists()
with output.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2, default=str)
    stream.write('\n')
print(json.dumps({'status': result['status'], 'database': result['database'],
                  'exists': result['exists'], 'database_opened': result['database_opened'],
                  'series_metadata_rows': len(result.get('series_metadata', [])),
                  'source_metadata_rows': len(result.get('source_metadata', [])),
                  'gate_promotion': False}), flush=True)
