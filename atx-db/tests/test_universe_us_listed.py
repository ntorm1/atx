"""Tier1-S4 T1/T2: the point-in-time US-listed universe."""

from __future__ import annotations

import json

import pytest

UNIVERSE_TABLE = "universe_us_listed_membership"

EXPECTED_COLUMNS = (
    "membership_id",
    "universe_id",
    "security_id",
    "symbol",
    "valid_from",
    "valid_to",
    "available_at",
    "security_type",
    "exchange_code",
    "has_cik",
    "cik",
    "market_cap_decile",
    "reason",
    "rules_json",
    "decision_count",
    "as_of_date",
    "is_latest_revision",
    "source",
    "run_id",
    "source_loaded_at",
)


def _columns(store, relation):
    rows = store.con.execute(
        """
        SELECT column_name
        FROM duckdb_columns()
        WHERE schema_name = 'main' AND table_name = ?
        ORDER BY column_index
        """,
        [relation],
    ).fetchall()
    return tuple(str(row[0]) for row in rows)


def test_membership_table_has_the_pit_interval_shape(tmp_store):
    assert _columns(tmp_store, UNIVERSE_TABLE) == EXPECTED_COLUMNS


def test_membership_table_is_catalogued(tmp_store):
    row = tmp_store.con.execute(
        "SELECT layer, grain, natural_key_json FROM table_catalog WHERE table_name = ?",
        [UNIVERSE_TABLE],
    ).fetchone()
    assert row is not None
    assert row[0] == "serving"
    assert row[1] == "universe_id,security_id,valid_from"
    assert json.loads(row[2]) == ["universe_id", "security_id", "valid_from"]


def test_membership_fields_are_catalogued(tmp_store):
    count = tmp_store.con.execute(
        "SELECT count(*) FROM field_catalog WHERE table_name = ?",
        [UNIVERSE_TABLE],
    ).fetchone()[0]
    assert int(count) == len(EXPECTED_COLUMNS)


def test_membership_has_a_lake_partition_spec(tmp_store):
    row = tmp_store.con.execute(
        "SELECT partition_columns_json, watermark_column FROM lake_partition_specs WHERE object_name = ?",
        [UNIVERSE_TABLE],
    ).fetchone()
    assert row is not None
    assert json.loads(row[0]) == ["as_of_date"]
    assert row[1] == "available_at"


@pytest.mark.parametrize(
    "check_name,severity",
    [
        ("universe_us_listed_overlapping_intervals", "critical"),
        ("universe_us_listed_missing_decile", "warning"),
    ],
)
def test_membership_quality_checks_are_registered(tmp_store, check_name, severity):
    row = tmp_store.con.execute(
        "SELECT severity, threshold_value, comparator, enabled FROM quality_check_registry WHERE check_name = ?",
        [check_name],
    ).fetchone()
    assert row is not None
    assert row[0] == severity
    assert float(row[1]) == 0.0
    assert row[2] == "eq"
    assert bool(row[3]) is True


def test_membership_is_a_default_lake_export_object():
    from atx_db.lake import DEFAULT_EXPORT_OBJECTS

    assert UNIVERSE_TABLE in DEFAULT_EXPORT_OBJECTS
