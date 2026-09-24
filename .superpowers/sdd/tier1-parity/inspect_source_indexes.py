"""Read bounded live catalog evidence without initializing the warehouse."""
import json
from pathlib import Path

import duckdb

path = Path('C:/atx/atx-db/data/warehouse.duckdb')
before = path.stat()
with duckdb.connect(str(path), read_only=True,
                    config={'memory_limit': '128MB', 'threads': '1'}) as con:
    indexes = con.execute("""SELECT schema_name, index_name, table_name,
        is_unique, is_primary, expressions FROM duckdb_indexes()
        WHERE table_name IN ('sec_company_facts', 'fundamental_points')
        ORDER BY index_name""").fetchall()
    constraints = con.execute("""SELECT table_name, constraint_type, count(*)
        FROM duckdb_constraints()
        WHERE table_name IN ('sec_company_facts', 'fundamental_points')
        GROUP BY ALL ORDER BY ALL""").fetchall()
    version = con.execute('SELECT max(cast(version AS INTEGER)) FROM schema_migrations').fetchone()[0]
after = path.stat()
assert (before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns)
print(json.dumps({'schema_version': version, 'indexes': indexes,
                  'constraints': constraints, 'warehouse_bytes': after.st_size,
                  'warehouse_mtime_ns': after.st_mtime_ns, 'unchanged': True}))
