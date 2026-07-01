"""S1-0: schema tests for the canonical item dimension.

Covers migrations 0061 (schema + table_catalog/field_catalog seed) and 0062
(indexes) for fundamental_item, fundamental_item_alias, and
fundamental_item_vendor_map. This task lays schema only; seed data lands in
S1-1, so these tests assert structure, catalog rows, idempotency, and the
fundamental_item.item_id uniqueness constraint, not row counts.
"""

from __future__ import annotations

import duckdb
import pytest


NEW_TABLES = (
    "fundamental_item",
    "fundamental_item_alias",
    "fundamental_item_vendor_map",
)

EXPECTED_COLUMNS = {
    "fundamental_item": (
        "item_id",
        "canonical_code",
        "statement",
        "section",
        "data_type",
        "unit_type",
        "sign_convention",
        "is_derived",
        "definition",
        "citation",
    ),
    "fundamental_item_alias": (
        "item_id",
        "alias_scheme",
        "alias_code",
        "coalesce_priority",
        "valid_from",
        "valid_to",
    ),
    "fundamental_item_vendor_map": (
        "item_id",
        "vendor",
        "vendor_field",
        "sign_note",
    ),
}


def _columns(con: duckdb.DuckDBPyConnection, table_name: str) -> list[str]:
    rows = con.execute(
        """
        SELECT column_name
        FROM information_schema.columns
        WHERE table_schema = 'main' AND table_name = ?
        ORDER BY ordinal_position
        """,
        [table_name],
    ).fetchall()
    return [row[0] for row in rows]


def _table_exists(con: duckdb.DuckDBPyConnection, table_name: str) -> bool:
    count = con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name = ?",
        [table_name],
    ).fetchone()[0]
    return count == 1


def test_item_registry_tables_exist(tmp_store):
    """All three item-registry tables exist after apply_pending_migrations."""
    for table_name in NEW_TABLES:
        assert _table_exists(tmp_store.con, table_name), f"{table_name} missing"


def test_item_registry_table_columns(tmp_store):
    """Each table has exactly the columns specified in the S1-0 brief."""
    for table_name, expected_cols in EXPECTED_COLUMNS.items():
        assert _columns(tmp_store.con, table_name) == list(expected_cols)


def test_table_catalog_has_three_new_rows(tmp_store):
    """table_catalog gains exactly one row per new table."""
    rows = tmp_store.con.execute(
        "SELECT table_name FROM table_catalog WHERE table_name = ANY(?)",
        [list(NEW_TABLES)],
    ).fetchall()
    catalogued = {row[0] for row in rows}
    assert catalogued == set(NEW_TABLES), (
        f"Expected table_catalog rows for {sorted(NEW_TABLES)}, found {sorted(catalogued)}"
    )


def test_field_catalog_has_one_row_per_column(tmp_store):
    """field_catalog carries one row per column for each new table."""
    for table_name, expected_cols in EXPECTED_COLUMNS.items():
        rows = tmp_store.con.execute(
            "SELECT field_name FROM field_catalog WHERE table_name = ?",
            [table_name],
        ).fetchall()
        catalogued = {row[0] for row in rows}
        expected = set(expected_cols)
        assert catalogued == expected, (
            f"{table_name} field_catalog mismatch; "
            f"missing={sorted(expected - catalogued)}, "
            f"extra={sorted(catalogued - expected)}"
        )


def test_fundamental_item_item_id_unique(tmp_store):
    """fundamental_item.item_id rejects duplicate inserts (PK/unique constraint)."""
    con = tmp_store.con
    con.execute(
        """
        INSERT INTO fundamental_item (item_id, canonical_code)
        VALUES (999001, 'test_metric_a')
        """
    )
    with pytest.raises(duckdb.Error):
        con.execute(
            """
            INSERT INTO fundamental_item (item_id, canonical_code)
            VALUES (999001, 'test_metric_b')
            """
        )


def test_fundamental_item_canonical_code_unique(tmp_store):
    """fundamental_item.canonical_code rejects duplicate canonical items."""
    con = tmp_store.con
    con.execute(
        """
        INSERT INTO fundamental_item (item_id, canonical_code)
        VALUES (999101, 'test_unique_metric')
        """
    )
    with pytest.raises(duckdb.Error):
        con.execute(
            """
            INSERT INTO fundamental_item (item_id, canonical_code)
            VALUES (999102, 'test_unique_metric')
            """
        )


def test_fundamental_item_alias_rejects_exact_duplicate_null_window(tmp_store):
    """Alias uniqueness treats NULL validity windows as equal for duplicate rows."""
    con = tmp_store.con
    con.execute(
        """
        INSERT INTO fundamental_item_alias (
            item_id, alias_scheme, alias_code, coalesce_priority, valid_from, valid_to
        )
        VALUES (999201, 'us-gaap', 'TestAliasConcept', 10, NULL, NULL)
        """
    )
    with pytest.raises(duckdb.Error):
        con.execute(
            """
            INSERT INTO fundamental_item_alias (
                item_id, alias_scheme, alias_code, coalesce_priority, valid_from, valid_to
            )
            VALUES (999201, 'us-gaap', 'TestAliasConcept', 10, NULL, NULL)
            """
        )


def test_fundamental_item_vendor_map_rejects_duplicate_key(tmp_store):
    """Vendor map uniqueness rejects duplicate item/vendor/field rows."""
    con = tmp_store.con
    con.execute(
        """
        INSERT INTO fundamental_item_vendor_map (
            item_id, vendor, vendor_field, sign_note
        )
        VALUES (999301, 'compustat', 'test_field', 'same sign')
        """
    )
    with pytest.raises(duckdb.Error):
        con.execute(
            """
            INSERT INTO fundamental_item_vendor_map (
                item_id, vendor, vendor_field, sign_note
            )
            VALUES (999301, 'compustat', 'test_field', 'same sign')
            """
        )


def test_item_registry_indexes_exist(tmp_store):
    """0062 creates lookup and uniqueness indexes for the registry."""
    rows = tmp_store.con.execute(
        "SELECT index_name, table_name FROM duckdb_indexes() WHERE table_name = ANY(?)",
        [list(NEW_TABLES)],
    ).fetchall()
    index_names = {row[0] for row in rows}
    assert "idx_fundamental_item_alias_lookup" in index_names
    assert "idx_fundamental_item_canonical" in index_names
    assert "idx_fundamental_item_alias_unique" in index_names
    assert "idx_fundamental_item_vendor_map_unique" in index_names


def test_migrations_0061_and_0062_recorded(tmp_store):
    """schema_migrations records versions 61 and 62 after bootstrap."""
    rows = tmp_store.con.execute(
        "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
    ).fetchall()
    versions = {row[0] for row in rows}
    assert 61 in versions, f"Migration 0061 not recorded; found: {sorted(versions)}"
    assert 62 in versions, f"Migration 0062 not recorded; found: {sorted(versions)}"


def test_apply_pending_migrations_is_idempotent(tmp_store):
    """Re-running apply_pending_migrations after bootstrap is a no-op."""
    from db.migrations import apply_pending_migrations

    result = apply_pending_migrations(tmp_store.con)
    assert result == [], f"Expected [] (no-op) but got {result}"


def test_item_registry_migration_bodies_are_idempotent(tmp_store):
    """Directly re-running 0061/0062 helper bodies is safe and does not duplicate catalog."""
    from db.migrations import (
        _fundamental_item_registry_indexes,
        _fundamental_item_registry_schema,
    )

    con = tmp_store.con
    _fundamental_item_registry_schema(con)
    _fundamental_item_registry_indexes(con)

    table_count = con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = ANY(?)",
        [list(NEW_TABLES)],
    ).fetchone()[0]
    field_count = con.execute(
        "SELECT count(*) FROM field_catalog WHERE table_name = ANY(?)",
        [list(NEW_TABLES)],
    ).fetchone()[0]
    assert table_count == len(NEW_TABLES)
    assert field_count == sum(len(cols) for cols in EXPECTED_COLUMNS.values())
