"""Bounded read-only comparison of candidate index and sequential access."""
import json
from pathlib import Path

import duckdb

path = Path('C:/atx/atx-db/data/warehouse.duckdb')
before = path.stat()
candidate_id = '5b162a89-35e4-58eb-bfb5-ae11932bb2d4'
target = 'SEC-COMPANYFACTS-UNRESOLVED-CIK-0001495229'
with duckdb.connect(str(path), read_only=True, config={
    'memory_limit': '256MB', 'threads': '1', 'preserve_insertion_order': 'false',
}) as con:
    result = {'candidate_id': candidate_id, 'indexes': con.execute("""
        SELECT index_name, is_unique, is_primary, expressions, sql FROM duckdb_indexes()
        WHERE table_name='identifier_resolution_candidates' ORDER BY index_name
    """).fetchall(), 'constraints': con.execute("""
        SELECT constraint_type, constraint_text FROM duckdb_constraints()
        WHERE table_name='identifier_resolution_candidates' ORDER BY constraint_type, constraint_text
    """).fetchall(), 'catalog_estimated_rows': con.execute("""
        SELECT estimated_size FROM duckdb_tables() WHERE table_name='identifier_resolution_candidates'
    """).fetchone()[0]}
    projections = 'candidate_id,source_dataset_id,source_key_type,source_key_value,target_security_id,match_method,run_id'
    observations = {}
    for mode in ('default', 'sequential'):
        if mode == 'sequential':
            con.execute('SET index_scan_max_count=0')
            con.execute('SET index_scan_percentage=0')
        by_id = con.execute(f'SELECT {projections} FROM identifier_resolution_candidates WHERE candidate_id=?',
                            [candidate_id]).fetchall()
        by_target = con.execute(f'SELECT {projections} FROM identifier_resolution_candidates WHERE target_security_id=? ORDER BY candidate_id',
                                [target]).fetchall()
        cik_rows = con.execute(f"""SELECT {projections} FROM identifier_resolution_candidates
            WHERE source_dataset_id='sec_company_facts' AND source_key_type='CIK'
              AND try_cast(source_key_value AS BIGINT)=1495229 ORDER BY candidate_id""").fetchall()
        plan = con.execute('EXPLAIN ANALYZE SELECT candidate_id FROM identifier_resolution_candidates WHERE candidate_id=?',
                           [candidate_id]).fetchone()[1]
        observations[mode] = {'by_id': by_id, 'by_target': by_target, 'cik_rows': cik_rows,
                              'id_plan_uses_index': 'Index Scan' in plan, 'id_plan': plan}
    result['observations'] = observations
after = path.stat()
assert (before.st_size,before.st_mtime_ns)==(after.st_size,after.st_mtime_ns)
result['warehouse_unchanged'] = True
print(json.dumps(result, default=str))
