"""Physical source-index changes must preserve filing history and constraints."""

from __future__ import annotations

from collections.abc import Iterator

import duckdb
import pytest

from atx_db.migrations.bodies_0326 import _bounded_source_indexes

_INDEX_SQL = (
    "CREATE INDEX idx_sec_company_facts_security_asof ON sec_company_facts(security_id, filed_date)",
    "CREATE INDEX idx_sec_company_facts_entity_asof ON sec_company_facts(entity_id, filed_date)",
    "CREATE INDEX idx_fundamental_points_metric_asof ON fundamental_points(metric, as_of_date)",
)
_TABLES = ("sec_company_facts", "fundamental_points", "source_index_control")


@pytest.fixture
def source_conn() -> Iterator[duckdb.DuckDBPyConnection]:
    with duckdb.connect(config={"threads": 1, "memory_limit": "128MB"}) as con:
        con.execute("""
            CREATE TABLE sec_company_facts (
                source VARCHAR NOT NULL, security_id VARCHAR NOT NULL, entity_id VARCHAR,
                cik VARCHAR NOT NULL, taxonomy VARCHAR NOT NULL, concept VARCHAR NOT NULL,
                unit VARCHAR NOT NULL, period_end DATE, filed_date DATE NOT NULL,
                accession_number VARCHAR, value DOUBLE, available_at TIMESTAMP,
                source_url VARCHAR NOT NULL
            );
            CREATE TABLE fundamental_points (
                source VARCHAR NOT NULL, security_id VARCHAR NOT NULL, metric VARCHAR NOT NULL,
                taxonomy VARCHAR, unit VARCHAR, period_end DATE, as_of_date DATE NOT NULL,
                accession_number VARCHAR, value DOUBLE, available_at TIMESTAMP
            )
        """)
        yield con


def _seed(con: duckdb.DuckDBPyConnection) -> None:
    con.execute("""
        INSERT INTO sec_company_facts (
            source, security_id, entity_id, cik, taxonomy, concept, unit, period_end,
            filed_date, accession_number, value, available_at, source_url
        ) VALUES
            ('SEC companyfacts', 'issuer-a', 'entity-a', '0000000001', 'us-gaap',
             'EarningsPerShareDiluted', 'USD/shares', DATE '2024-03-31', DATE '2024-05-01',
             'original-a', 1.0, TIMESTAMP '2024-05-01 20:00:00', 'retained-archive'),
            ('SEC companyfacts', 'issuer-a', 'entity-a', '0000000001', 'us-gaap',
             'EarningsPerShareDiluted', 'USD/shares', DATE '2024-03-31', DATE '2024-08-01',
             'amended-a', 0.8, TIMESTAMP '2024-08-01 20:00:00', 'retained-archive'),
            ('SEC companyfacts', 'issuer-b', 'entity-b', '0000000002', 'us-gaap',
             'EarningsPerShareDiluted', 'USD/shares', DATE '2024-03-31', DATE '2024-05-02',
             'original-b', NULL, TIMESTAMP '2024-05-02 20:00:00', 'retained-archive');
        INSERT INTO fundamental_points (
            source, security_id, metric, taxonomy, unit, period_end, as_of_date,
            accession_number, value, available_at
        ) SELECT source, security_id, concept, taxonomy, unit, period_end, filed_date,
                 accession_number, value, available_at FROM sec_company_facts;
        CREATE TABLE source_index_control (id INTEGER PRIMARY KEY, token VARCHAR NOT NULL);
        CREATE UNIQUE INDEX source_index_control_token ON source_index_control(token);
        INSERT INTO source_index_control VALUES (1, 'retained')
    """)
    for statement in _INDEX_SQL:
        con.execute(statement)


def _snapshot(con: duckdb.DuckDBPyConnection) -> tuple[object, ...]:
    columns = con.execute("""
        SELECT table_name, column_name, column_index, data_type, is_nullable, column_default
        FROM duckdb_columns() WHERE table_name = ANY(?) ORDER BY table_name, column_index
    """, [list(_TABLES)]).fetchall()
    constraints = con.execute("""
        SELECT table_name, constraint_type, constraint_text FROM duckdb_constraints()
        WHERE table_name = ANY(?) ORDER BY table_name, constraint_type, constraint_text
    """, [list(_TABLES)]).fetchall()
    return (
        columns,
        constraints,
        con.execute("SELECT * FROM sec_company_facts ORDER BY accession_number").fetchall(),
        con.execute("SELECT * FROM fundamental_points ORDER BY accession_number").fetchall(),
        con.execute("SELECT * FROM source_index_control").fetchall(),
    )


def _pit_eps(con: duckdb.DuckDBPyConnection, cutoff: str) -> list[tuple[object, ...]]:
    return con.execute("""
        SELECT security_id, value FROM fundamental_points
        WHERE metric='EarningsPerShareDiluted'
          AND available_at <= CAST(? AS TIMESTAMP) AND as_of_date <= CAST(? AS DATE)
        QUALIFY row_number() OVER (
            PARTITION BY security_id, metric, unit, period_end
            ORDER BY as_of_date DESC, available_at DESC, accession_number DESC
        ) = 1 ORDER BY security_id
    """, [cutoff, cutoff]).fetchall()


def test_removal_preserves_source_history_constraints_and_asof_results(source_conn):
    con = source_conn
    _seed(con)
    before = _snapshot(con)
    for _ in range(2):
        _bounded_source_indexes(con)
        assert _snapshot(con) == before
    assert _pit_eps(con, "2024-06-01") == [("issuer-a", 1.0), ("issuer-b", None)]
    assert _pit_eps(con, "2024-09-01") == [("issuer-a", 0.8), ("issuer-b", None)]
    assert con.execute("SELECT index_name FROM duckdb_indexes() ORDER BY index_name").fetchall() == [
        ("source_index_control_token",),
    ]
    with pytest.raises(duckdb.ConstraintException):
        con.execute("UPDATE sec_company_facts SET source=NULL WHERE accession_number='original-a'")
    with pytest.raises(duckdb.ConstraintException):
        con.execute("UPDATE fundamental_points SET as_of_date=NULL WHERE accession_number='original-a'")
    with pytest.raises(duckdb.ConstraintException):
        con.execute("INSERT INTO source_index_control VALUES (1, 'new-token')")
    with pytest.raises(duckdb.ConstraintException):
        con.execute("INSERT INTO source_index_control VALUES (2, 'retained')")
    assert _snapshot(con) == before


@pytest.mark.parametrize("collision", ["unique", "wrong_table"])
def test_nonoptional_collision_is_refused_before_any_drop(source_conn, collision):
    con = source_conn
    con.execute(_INDEX_SQL[0])
    con.execute(_INDEX_SQL[1])
    if collision == "unique":
        con.execute("CREATE UNIQUE INDEX idx_fundamental_points_metric_asof ON fundamental_points(metric)")
    else:
        con.execute("CREATE TABLE unrelated_index_owner (metric VARCHAR)")
        con.execute("CREATE INDEX idx_fundamental_points_metric_asof ON unrelated_index_owner(metric)")
    before = con.execute("SELECT index_name, table_name, is_unique FROM duckdb_indexes() ORDER BY index_name").fetchall()
    with pytest.raises(RuntimeError, match="0326 refuses to remove a non-optional index"):
        _bounded_source_indexes(con)
    assert con.execute("SELECT index_name, table_name, is_unique FROM duckdb_indexes() ORDER BY index_name").fetchall() == before


def test_real_bootstrap_preserves_source_history_without_recreating_indexes(tmp_store):
    from atx_db.schema import ensure_quant_schema

    con = tmp_store.con
    assert con.execute("SELECT count(*) FROM schema_migrations WHERE CAST(version AS INTEGER)=326").fetchone() == (1,)
    assert con.execute("""
        SELECT count(*) FROM duckdb_indexes()
        WHERE table_name IN ('sec_company_facts', 'fundamental_points')
    """).fetchone() == (0,)
    _seed(con)
    before = _snapshot(con)
    _bounded_source_indexes(con)
    ensure_quant_schema(tmp_store)
    assert _snapshot(con) == before
    assert con.execute("""
        SELECT count(*) FROM duckdb_indexes()
        WHERE table_name IN ('sec_company_facts', 'fundamental_points')
    """).fetchone() == (0,)
    assert _pit_eps(con, "2024-06-01") == [("issuer-a", 1.0), ("issuer-b", None)]
    assert _pit_eps(con, "2024-09-01") == [("issuer-a", 0.8), ("issuer-b", None)]
