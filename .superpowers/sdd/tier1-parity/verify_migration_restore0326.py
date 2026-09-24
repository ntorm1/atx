"""Bounded restore evidence after the first migration0326 checkpoint failure."""
import hashlib
import json
from pathlib import Path

import duckdb

warehouse = Path('C:/atx/atx-db/data/warehouse.duckdb')
backup = warehouse.with_name('warehouse.duckdb.pre-migrate.20260924-224104.bak')


def evidence(path):
    with duckdb.connect(str(path), read_only=True,
                        config={'memory_limit': '128MB', 'threads': '1'}) as con:
        return {
            'schema_version': con.execute(
                'SELECT max(cast(version AS INTEGER)) FROM schema_migrations').fetchone()[0],
            'catalog_estimated_rows': dict(con.execute("""SELECT table_name, estimated_size
                FROM duckdb_tables() WHERE table_name IN
                ('sec_company_facts','fundamental_points','raw_source_files','dataset_runs')""").fetchall()),
            'lock_count': con.execute('SELECT count(*) FROM migration_apply_lock').fetchone()[0],
            'source_indexes': con.execute("""SELECT index_name, table_name, is_unique, is_primary
                FROM duckdb_indexes() WHERE table_name IN ('sec_company_facts','fundamental_points')
                ORDER BY index_name""").fetchall(),
            'migration_checksums': con.execute(
                'SELECT version, checksum FROM schema_migrations ORDER BY version').fetchall(),
        }


before = warehouse.stat()
restored, retained = evidence(warehouse), evidence(backup)
assert restored['lock_count'] == 0
assert restored['schema_version'] == 322
for key in ('schema_version', 'catalog_estimated_rows', 'source_indexes', 'migration_checksums'):
    assert restored[key] == retained[key], key
with backup.open('rb') as source:
    digest = hashlib.file_digest(source, 'sha256').hexdigest()
after = warehouse.stat()
assert (before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns)
print(json.dumps({'restore_catalog_equivalence': True, 'full_source_row_equality_measured': False,
                  'schema_version': restored['schema_version'],
                  'catalog_estimated_rows': restored['catalog_estimated_rows'],
                  'cleared_lock_count': restored['lock_count'], 'backup_lock_count': retained['lock_count'],
                  'backup_path': str(backup), 'backup_sha256': digest,
                  'backup_bytes': backup.stat().st_size, 'read_only_warehouse_unchanged': True}))
