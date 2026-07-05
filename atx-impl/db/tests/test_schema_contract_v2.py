"""PF3-S2 S2-0 acceptance tests for the PIT-gap close."""

from __future__ import annotations

import pytest

from db.quality import pit_column_presence_check
from db.schema_contract import ColumnSpec


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
