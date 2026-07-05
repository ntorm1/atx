"""PF3-S2 S2-0 acceptance tests for the PIT-gap close."""

from __future__ import annotations

import datetime as dt

import duckdb
import pytest

from db.connection import DuckDBStore
from db.migrations import MIGRATIONS
from db.quality import pit_column_presence_check
from db.schema import ensure_quant_schema
from db.schema_contract import ColumnSpec


def _bootstrap_through_migration_0134(store: DuckDBStore) -> None:
    con = store.con
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS dataset_runs (
            run_id VARCHAR PRIMARY KEY,
            dataset_id VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            started_at TIMESTAMP NOT NULL,
            finished_at TIMESTAMP,
            rows_loaded BIGINT,
            source VARCHAR,
            params_json VARCHAR,
            error_message VARCHAR
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS dataset_watermarks (
            dataset_id VARCHAR NOT NULL,
            watermark_name VARCHAR NOT NULL,
            watermark_value VARCHAR NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (dataset_id, watermark_name)
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS security_identifiers (
            symbol VARCHAR NOT NULL,
            id_type VARCHAR NOT NULL,
            id_value VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (symbol, id_type, id_value)
        )
        """
    )
    ensure_quant_schema(store)
    for migration in MIGRATIONS:
        if migration.version >= 135:
            break
        migration.up(con)


def _migration_0135():
    return next(migration for migration in MIGRATIONS if migration.version == 135)


def _pit_fact_specs(*missing: str) -> list[ColumnSpec]:
    omitted = set(missing)
    specs = [
        ColumnSpec("fixture_id", "VARCHAR", nullable=False, is_natural_key=True, declared_in="migration")
    ]
    if "as_of_date" not in omitted:
        specs.append(
            ColumnSpec("as_of_date", "DATE", nullable=False, is_pit_column=True, declared_in="migration")
        )
    if "available_at" not in omitted:
        specs.append(
            ColumnSpec(
                "available_at",
                "TIMESTAMP",
                nullable=True,
                is_pit_column=True,
                declared_in="migration",
            )
        )
    if "source_loaded_at" not in omitted:
        specs.append(
            ColumnSpec(
                "source_loaded_at",
                "TIMESTAMP",
                nullable=False,
                is_pit_column=True,
                declared_in="migration",
            )
        )
    if "run_id" not in omitted:
        specs.append(
            ColumnSpec("run_id", "VARCHAR", nullable=True, is_pit_column=True, declared_in="migration")
        )
    if "is_latest_revision" not in omitted:
        specs.append(
            ColumnSpec(
                "is_latest_revision",
                "BOOLEAN",
                nullable=True,
                is_pit_column=True,
                declared_in="migration",
            )
        )
    return specs


def test_pit_exemption_registry_exists_and_is_catalogued(tmp_store):
    live_columns = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT column_name FROM duckdb_columns() WHERE table_name = 'pit_exemption'"
        ).fetchall()
    }
    expected_columns = {
        "table_name",
        "missing_columns",
        "reason",
        "exempted_by",
        "exempted_at",
        "source_loaded_at",
    }
    assert live_columns == expected_columns

    table_catalog_count = tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'pit_exemption'"
    ).fetchone()[0]
    assert table_catalog_count == 1

    field_rows = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT field_name FROM field_catalog WHERE table_name = 'pit_exemption'"
        ).fetchall()
    }
    assert field_rows == expected_columns

    seeded = tmp_store.con.execute(
        "SELECT table_name, reason FROM pit_exemption ORDER BY table_name"
    ).fetchall()
    assert seeded
    assert all(str(reason).strip() for _table_name, reason in seeded)


def test_bootstrapped_warehouse_has_zero_pit_column_presence_after_s2_0(tmp_store):
    result = pit_column_presence_check(tmp_store)

    assert result.status == "passed"
    assert result.observed_value == 0.0
    assert result.threshold_value == 0.0
    assert result.severity == "critical"
    assert result.details["tables_missing_pit_columns"] == {}
    assert result.details["invalid_pit_exemptions"] == []


def test_migration_0135_recovers_source_loaded_at_from_computed_at(tmp_path):
    db_path = tmp_path / "pre_0135.duckdb"
    store = DuckDBStore(db_path)
    store.connection = duckdb.connect(str(db_path))
    store._configure_session(store.connection)
    try:
        _bootstrap_through_migration_0134(store)
        feature_computed_at = dt.datetime(2001, 2, 3, 4, 5, 6)
        alpha_computed_at = dt.datetime(2002, 3, 4, 5, 6, 7)
        store.con.execute(
            """
            INSERT INTO feature_values (
                feature_set, feature_name, security_id, symbol, as_of_date,
                value, available_at, source, run_id, computed_at
            )
            VALUES (
                'fixture_set', 'fixture_feature', 'SEC-1', 'AAA', DATE '2001-02-03',
                1.25, TIMESTAMP '2001-02-03 09:30:00', 'test', 'run-feature',
                TIMESTAMP '2001-02-03 04:05:06'
            )
            """
        )
        store.con.execute(
            """
            INSERT INTO alpha_signal_values (
                alpha_signal_id, alpha_id, security_id, symbol, as_of_date,
                signal_value, rank_value, weight, cross_section_count,
                available_at, input_hash, source, run_id, computed_at
            )
            VALUES (
                'signal-1', 'alpha-1', 'SEC-1', 'AAA', DATE '2002-03-04',
                2.5, 1.0, 0.5, 10, TIMESTAMP '2002-03-04 09:30:00',
                'hash-alpha', 'test', 'run-alpha', TIMESTAMP '2002-03-04 05:06:07'
            )
            """
        )

        _migration_0135().up(store.con)
        _migration_0135().up(store.con)

        recovered = dict(
            store.con.execute(
                """
                SELECT 'feature_values' AS table_name, source_loaded_at
                FROM feature_values
                WHERE feature_set = 'fixture_set'
                UNION ALL
                SELECT 'alpha_signal_values' AS table_name, source_loaded_at
                FROM alpha_signal_values
                WHERE alpha_signal_id = 'signal-1'
                """
            ).fetchall()
        )
        assert recovered == {
            "feature_values": feature_computed_at,
            "alpha_signal_values": alpha_computed_at,
        }
    finally:
        if store.connection is not None:
            store.connection.close()


def test_non_exempt_fact_missing_available_at_still_goes_red(tmp_store):
    result = pit_column_presence_check(
        tmp_store,
        manifest={"unregistered_fact": _pit_fact_specs("available_at")},
    )

    assert result.status == "failed"
    assert result.observed_value == 1.0
    assert result.details["tables_missing_pit_columns"] == {
        "unregistered_fact": ["available_at"]
    }
    assert result.details["exempted_pit_columns"] == {}


def test_pit_exemption_requires_non_empty_reason(tmp_store):
    with pytest.raises(Exception):
        tmp_store.con.execute(
            """
            INSERT INTO pit_exemption (
                table_name, missing_columns, reason, exempted_by
            )
            VALUES ('empty_reason_fixture', '["available_at"]', '   ', 'test')
            """
        )


def test_pit_exemption_only_subtracts_declared_table_and_columns(tmp_store):
    tmp_store.con.execute(
        """
        INSERT OR REPLACE INTO pit_exemption (
            table_name, missing_columns, reason, exempted_by
        )
        VALUES (
            'widget_fact',
            '["available_at"]',
            'Fixture exemption for one declared column only.',
            'test'
        )
        """
    )
    manifest = {
        "widget_fact": _pit_fact_specs("available_at", "is_latest_revision"),
        "other_fact": _pit_fact_specs("available_at"),
    }

    result = pit_column_presence_check(tmp_store, manifest=manifest)

    assert result.status == "failed"
    assert result.observed_value == 2.0
    assert result.details["exempted_pit_columns"] == {"widget_fact": ["available_at"]}
    assert result.details["tables_missing_pit_columns"] == {
        "other_fact": ["available_at"],
        "widget_fact": ["is_latest_revision"],
    }
