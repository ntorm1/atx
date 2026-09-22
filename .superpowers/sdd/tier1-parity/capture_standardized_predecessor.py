"""Capture the real 0319 public standardized contract without migrating it."""
import datetime as dt
import json
from pathlib import Path

import duckdb

out = Path(__file__).parent / 'standardized-2.0.0-production-predecessor.json'
if out.exists():
    raise SystemExit('Refusing to overwrite predecessor evidence')
with duckdb.connect('C:/atx/atx-db/data/warehouse.duckdb', read_only=True, config={
    'memory_limit': '1GB', 'threads': '1', 'preserve_insertion_order': 'false',
}) as con:
    def rows(query):
        cursor = con.execute(query)
        names = [column[0] for column in cursor.description]
        return [dict(zip(names, row, strict=True)) for row in cursor.fetchall()]

    schemas = rows("""SELECT * FROM api_schema_catalog
        WHERE dataset_id='ATX.US.FUNDAMENTALS' AND schema_code='standardized'
          AND schema_version='2.0.0'""")
    fields = rows("""SELECT * FROM api_field_catalog
        WHERE dataset_id='ATX.US.FUNDAMENTALS' AND schema_code='standardized'
          AND schema_version='2.0.0' ORDER BY ordinal""")
    version = con.execute('SELECT max(CAST(version AS INTEGER)) FROM schema_migrations').fetchone()[0]
    assert version == 319 and len(schemas) == 1 and fields
    result = {
        'observed_at_utc': dt.datetime.now(dt.UTC).isoformat(),
        'source': 'read-only existing production warehouse before 0320/0321',
        'production_migration': version, 'schemas': schemas, 'fields': fields,
    }
with out.open('x', encoding='utf-8') as handle:
    json.dump(result, handle, indent=2, default=str)
print(json.dumps({'production_migration': version, 'schema_sha256': schemas[0]['schema_sha256'],
                  'field_count': len(fields), 'artifact': str(out)}))
