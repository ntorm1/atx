"""Read persisted migration0326 acceptance and hash its retained backup."""
import hashlib
import json
from pathlib import Path

import duckdb

warehouse = Path('C:/atx/atx-db/data/warehouse.duckdb')
before = warehouse.stat()
with duckdb.connect(str(warehouse), read_only=True,
                    config={'memory_limit': '128MB', 'threads': '1'}) as con:
    versions = [row[0] for row in con.execute(
        'SELECT cast(version AS INTEGER) FROM schema_migrations ORDER BY 1').fetchall()]
    assert max(versions) == 326 and all(version in versions for version in range(323, 327))
    assert con.execute('SELECT count(*) FROM migration_apply_lock').fetchone() == (0,)
    indexes = con.execute("""SELECT index_name FROM duckdb_indexes()
        WHERE table_name IN ('sec_company_facts', 'fundamental_points')""").fetchall()
    assert indexes == []
    constraints = con.execute("""SELECT table_name, constraint_type, count(*)
        FROM duckdb_constraints() WHERE table_name IN ('sec_company_facts','fundamental_points')
        GROUP BY ALL ORDER BY ALL""").fetchall()
    assert constraints == [('fundamental_points', 'NOT NULL', 5), ('sec_company_facts', 'NOT NULL', 9)]
    row = con.execute("""SELECT backup_path, sha256, byte_size, versions_before, versions_after
        FROM migration_backup_registry WHERE versions_after IS NOT NULL
        ORDER BY created_at DESC LIMIT 1""").fetchone()
    assert row and max(json.loads(row[3])) == 322 and max(json.loads(row[4])) == 326
backup = Path(row[0])
if not backup.is_absolute():
    backup = Path('C:/atx/atx-db') / backup
assert backup.stat().st_size == row[2]
with backup.open('rb') as source:
    digest = hashlib.file_digest(source, 'sha256').hexdigest()
assert digest == row[1]
after = warehouse.stat()
assert (before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns)
print(json.dumps({'status': 'verified', 'schema_version': 326,
                  'applied_versions': [323, 324, 325, 326], 'optional_source_indexes': indexes,
                  'source_constraints': constraints, 'migration_lock_count': 0,
                  'backup_path': str(backup), 'backup_sha256': digest, 'backup_bytes': row[2],
                  'warehouse_bytes': after.st_size, 'read_only_warehouse_unchanged': True,
                  'full_source_row_equality_measured': False}))
