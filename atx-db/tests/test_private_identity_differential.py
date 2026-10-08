"""D0 full-row and fixed exclusion semantics on small physical Parquet files."""
import hashlib

import duckdb
import pytest

from atx_db.alpha_panel.private_identity_differential import compare_d0


@pytest.fixture
def connection(tmp_path):
    con = duckdb.connect(str(tmp_path / 'd0.duckdb'), config={
        'memory_limit': '128MB', 'threads': 1, 'temp_directory': str(tmp_path / 'spill')})
    con.execute("SET TimeZone='UTC'")
    try:
        yield con
    finally:
        con.close()


def parquet(con, path, query):
    con.execute(f"COPY ({query}) TO '{path.as_posix()}' (FORMAT PARQUET)")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_r4_ignores_only_created_at_and_preserves_full_rows_and_keys(connection, tmp_path):
    con = connection
    a, b = tmp_path / 'a.parquet', tmp_path / 'b.parquet'
    base = "SELECT 1::BIGINT perm_security_id, 2::BIGINT perm_company_id, DATE '2019-01-01' link_start, TIMESTAMP '2019-01-02 22:00:00' available_at, 'P' link_primary, {clock} created_at"
    pa = parquet(con, a, base.format(clock="TIMESTAMP '2026-01-01 00:00:00'"))
    pb = parquet(con, b, base.format(clock="TIMESTAMP '2026-02-01 00:00:00'"))
    result = compare_d0(con, rebuilt=a, published=b, rebuilt_sha256=pa, published_sha256=pb, kind='r4')
    assert result['d0_pass'] and result['excluded_columns'] == ['created_at']
    # Same keys and changed non-key content must count one full row in each direction.
    b.unlink()
    pb = parquet(con, b, base.replace("'P' link_primary", "'J' link_primary").format(clock="NULL::TIMESTAMP"))
    result = compare_d0(con, rebuilt=a, published=b, rebuilt_sha256=pa, published_sha256=pb, kind='r4')
    assert (result['rebuilt_only_rows'], result['published_only_rows']) == (1, 1)
    assert not result['d0_pass']
    # Multiset equality is insufficient when the native key is duplicated.
    a.unlink()
    query = base.format(clock="NULL::TIMESTAMP")
    pa = parquet(con, a, query + ' UNION ALL ' + query)
    result = compare_d0(con, rebuilt=a, published=a, rebuilt_sha256=pa, published_sha256=pa, kind='r4')
    assert result['rows_identical'] and result['rebuilt_duplicate_key_groups'] == 1
    assert not result['d0_pass']


def test_pit_full_rows_multiplicity_schema_and_stale_pin(connection, tmp_path):
    con = connection
    a, b = tmp_path / 'a.parquet', tmp_path / 'b.parquet'
    pa = parquet(con, a, "SELECT 7::BIGINT sr_id, 'P' primary_role UNION ALL SELECT 7, 'P'")
    pb = parquet(con, b, "SELECT 7::BIGINT sr_id, 'P' primary_role")
    result = compare_d0(con, rebuilt=a, published=b, rebuilt_sha256=pa, published_sha256=pb, kind='pit')
    assert (result['rebuilt_only_rows'], result['published_only_rows']) == (1, 0)
    assert result['excluded_columns'] == [] and not result['d0_pass']
    b.unlink()
    newer = parquet(con, b, "SELECT 7::BIGINT sr_id, 'P' primary_role, 1 extra_column")
    with pytest.raises(ValueError, match='original pin'):
        compare_d0(con, rebuilt=a, published=b, rebuilt_sha256=pa, published_sha256=pb, kind='pit')
    result = compare_d0(con, rebuilt=a, published=b, rebuilt_sha256=pa, published_sha256=newer, kind='pit')
    assert not result['schema_matches'] and result['rebuilt_only_rows'] is None
    assert not result['d0_pass']
    assert not con.execute("SELECT view_name FROM duckdb_views() WHERE view_name LIKE 'a0_%'").fetchall()


def test_mutation_during_difference_refuses_result_and_cleans_views(connection, tmp_path):
    con = connection
    a, b = tmp_path / 'a.parquet', tmp_path / 'b.parquet'
    pa = parquet(con, a, 'SELECT 7::BIGINT sr_id')
    pb = parquet(con, b, 'SELECT 8::BIGINT sr_id')

    class ChangedInput:
        changed = False

        def execute(self, sql):
            result = con.execute(sql)
            if 'EXCEPT ALL' in sql and not self.changed:
                self.changed = True
                with b.open('ab') as stream:
                    stream.write(b'changed')
            return result

    with pytest.raises(ValueError, match='changed during comparison'):
        compare_d0(ChangedInput(), rebuilt=a, published=b, rebuilt_sha256=pa,
                   published_sha256=pb, kind='link_table')
    assert not con.execute("SELECT view_name FROM duckdb_views() WHERE view_name LIKE 'a0_%'").fetchall()
