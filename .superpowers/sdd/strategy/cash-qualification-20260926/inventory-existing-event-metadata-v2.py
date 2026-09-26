import json
from pathlib import Path
import duckdb

target = 'C:/atx/atx-db/data/warehouse.duckdb'
try:
    connection = duckdb.connect(target, read_only=True, config={'memory_limit': '128MiB', 'threads': 1})
except Exception as error:
    print(json.dumps({'status': 'read-only-open-unavailable', 'error': str(error)}))
    raise SystemExit(0)
connection.execute('SET threads=1')
connection.execute("SET memory_limit='128MiB'")
rows = connection.execute("""
SELECT table_schema,table_name,column_name,data_type
FROM information_schema.columns
WHERE regexp_matches(table_name, '(?i)delist|transition|instrument.*type|universe_us|identity_fact')
ORDER BY table_schema,table_name,ordinal_position
""").fetchall()
connection.close()
report = {'status': 'complete', 'mode': 'read-only-schema-only', 'source': target,
          'columns': [dict(zip(['schema', 'table', 'column', 'type'], row)) for row in rows]}
encoded = json.dumps(report, indent=2) + '\n'
with Path('build-equity/existing-event-metadata-inventory-v2.json').open('x', encoding='utf-8') as output:
    output.write(encoded)
print(encoded)

