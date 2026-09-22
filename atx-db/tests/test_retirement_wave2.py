"""Retired leaf modules and unscheduled valuation surfaces retain historical data."""

from __future__ import annotations

import datetime as dt
import importlib
import json
from pathlib import Path

import pytest

from atx_db.migrations.bodies_0310 import (
    DEPRECATED_TABLES,
    DEPRECATION_PREFIX,
    RETIRED_FACTOR_IDS,
    RETIREMENT_DATE,
    _retire_valuation_surfaces,
)

RETIRED_MODULES = ("abnormal_capex", "operating_leverage")
SCRIPT_BACKED_MODULES = (
    "cash_flow_profitability",
    "fundamental_signals",
    "quarterly_revenue_margin_confirmation",
)


def test_migration_0310_is_registered_without_exposing_its_body_module():
    import atx_db.migrations as migrations

    matches = [migration for migration in migrations.MIGRATIONS if migration.version == 310]
    assert len(matches) == 1
    assert matches[0].name == "retire_valuation_surfaces"
    assert matches[0].up is _retire_valuation_surfaces
    assert not hasattr(migrations, "bodies_0310")


@pytest.mark.parametrize("module_name", RETIRED_MODULES)
def test_retired_modules_and_own_tests_are_gone(module_name):
    with pytest.raises(ModuleNotFoundError, match=rf"atx_db\.{module_name}"):
        importlib.import_module(f"atx_db.{module_name}")
    assert not (Path(__file__).parent / f"test_{module_name}.py").exists()


@pytest.mark.parametrize("module_name", SCRIPT_BACKED_MODULES)
def test_script_backed_modules_are_kept(module_name):
    assert importlib.import_module(f"atx_db.{module_name}") is not None
    assert (Path(__file__).parents[1] / "scripts" / f"build_{module_name}.py").exists()


@pytest.mark.parametrize("module_name", ("enterprise_value", "valuation_multiples"))
def test_legacy_valuation_modules_stay_importable(module_name):
    assert importlib.import_module(f"atx_db.{module_name}") is not None


def test_deprecated_datasets_are_unscheduled_with_no_dangling_dependencies():
    from atx_db.jobs import DATASET_DEPENDENCIES, DATASET_REGISTRY

    assert not set(DEPRECATED_TABLES).intersection(DATASET_REGISTRY)
    assert not set(DEPRECATED_TABLES).intersection(DATASET_DEPENDENCIES)
    for dataset_id, dependencies in DATASET_DEPENDENCIES.items():
        assert set(dependencies) <= DATASET_REGISTRY.keys(), dataset_id


@pytest.mark.parametrize("table_name", DEPRECATED_TABLES)
def test_bootstrap_keeps_and_deprecates_every_legacy_table(tmp_store, table_name):
    con = tmp_store.con
    assert con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'main' AND table_name = ?",
        [table_name],
    ).fetchone() == (1,)
    table_description = con.execute(
        "SELECT description FROM table_catalog WHERE table_name = ?", [table_name]
    ).fetchone()[0]
    dataset_description, metadata_json = con.execute(
        "SELECT description, metadata_json FROM dataset_catalog WHERE dataset_id = ?",
        [table_name],
    ).fetchone()
    assert table_description.startswith(DEPRECATION_PREFIX)
    assert dataset_description.startswith(DEPRECATION_PREFIX)
    assert json.loads(metadata_json)["deprecated"] is True
    assert json.loads(metadata_json)["superseded_by"] == "market_daily_metrics"


def test_bootstrap_closes_only_the_two_retired_factor_definitions(tmp_store):
    rows = tmp_store.con.execute(
        "SELECT factor_id, valid_to, declared_in FROM factor_definition "
        "WHERE factor_id IN (?, ?) ORDER BY factor_id",
        list(RETIRED_FACTOR_IDS),
    ).fetchall()
    assert rows == [
        (factor_id, dt.date.fromisoformat(RETIREMENT_DATE), "retired")
        for factor_id in RETIRED_FACTOR_IDS
    ]


def _seed_historical_row(con, table_name):
    """Populate every required field so row retention is checked on nonempty tables."""
    columns = con.execute(
        "SELECT column_name, data_type FROM information_schema.columns "
        "WHERE table_schema = 'main' AND table_name = ? "
        "AND is_nullable = 'NO' AND column_default IS NULL ORDER BY ordinal_position",
        [table_name],
    ).fetchall()
    values_by_type = {
        "VARCHAR": "historical",
        "DOUBLE": 123.0,
        "BOOLEAN": True,
        "DATE": dt.date(2025, 1, 2),
        "TIMESTAMP": dt.datetime(2025, 1, 3),
    }
    names = ", ".join(f'"{name}"' for name, _kind in columns)
    placeholders = ", ".join("?" for _ in columns)
    con.execute(
        f"INSERT INTO {table_name} ({names}) VALUES ({placeholders})",
        [values_by_type[kind] for _name, kind in columns],
    )


def test_upgrade_disables_existing_jobs_preserves_rows_and_is_idempotent(tmp_store):
    from atx_db.jobs import JobManager

    con = tmp_store.con
    historical_rows = {}
    for table_name in DEPRECATED_TABLES:
        _seed_historical_row(con, table_name)
        historical_rows[table_name] = con.execute(f"SELECT * FROM {table_name}").fetchall()
        con.execute(
            "UPDATE table_catalog SET description = 'original table' WHERE table_name = ?",
            [table_name],
        )
        con.execute(
            "UPDATE dataset_catalog SET description = 'original dataset', "
            "metadata_json = '{\"owner_note\":\"keep\"}' WHERE dataset_id = ?",
            [table_name],
        )
        # Include a custom job name to prove retirement matches dataset_id.
        con.execute(
            "INSERT INTO etl_job_definitions (job_name, dataset_id, params_json, enabled) "
            "VALUES (?, ?, '{}', true)",
            [f"custom_{table_name}", table_name],
        )
    con.execute(
        "INSERT INTO etl_job_definitions (job_name, dataset_id, params_json, enabled) "
        "VALUES ('keep', 'sec_security_master', '{}', true)"
    )
    con.execute(
        "UPDATE factor_definition SET valid_to = NULL, declared_in = 'legacy' "
        "WHERE factor_id = ?",
        [RETIRED_FACTOR_IDS[0]],
    )
    con.execute(
        "UPDATE factor_definition SET valid_to = DATE '2025-01-01', declared_in = 'earlier' "
        "WHERE factor_id = ?",
        [RETIRED_FACTOR_IDS[1]],
    )
    other_factors = con.execute(
        "SELECT * FROM factor_definition WHERE factor_id NOT IN (?, ?) ORDER BY factor_id",
        list(RETIRED_FACTOR_IDS),
    ).fetchall()

    _retire_valuation_surfaces(con)
    assert con.execute(
        "SELECT job_name, enabled FROM etl_job_definitions ORDER BY job_name"
    ).fetchall() == [(f"custom_{name}", False) for name in DEPRECATED_TABLES] + [("keep", True)]
    assert JobManager(tmp_store).enabled_job_order() == ["keep"]
    assert con.execute(
        "SELECT factor_id, valid_to, declared_in FROM factor_definition "
        "WHERE factor_id IN (?, ?) ORDER BY factor_id",
        list(RETIRED_FACTOR_IDS),
    ).fetchall() == [
        (RETIRED_FACTOR_IDS[0], dt.date.fromisoformat(RETIREMENT_DATE), "retired"),
        (RETIRED_FACTOR_IDS[1], dt.date(2025, 1, 1), "earlier"),
    ]
    assert con.execute(
        "SELECT * FROM factor_definition WHERE factor_id NOT IN (?, ?) ORDER BY factor_id",
        list(RETIRED_FACTOR_IDS),
    ).fetchall() == other_factors
    for table_name in DEPRECATED_TABLES:
        assert con.execute(f"SELECT * FROM {table_name}").fetchall() == historical_rows[table_name]
        metadata_json = con.execute(
            "SELECT metadata_json FROM dataset_catalog WHERE dataset_id = ?", [table_name]
        ).fetchone()[0]
        assert json.loads(metadata_json) == {
            "owner_note": "keep", "deprecated": True, "superseded_by": "market_daily_metrics"
        }

    catalog_before = con.execute("SELECT * FROM dataset_catalog ORDER BY dataset_id").fetchall()
    jobs_before = con.execute("SELECT * FROM etl_job_definitions ORDER BY job_name").fetchall()
    _retire_valuation_surfaces(con)
    assert con.execute("SELECT * FROM dataset_catalog ORDER BY dataset_id").fetchall() == catalog_before
    assert con.execute("SELECT * FROM etl_job_definitions ORDER BY job_name").fetchall() == jobs_before
    for table_name in DEPRECATED_TABLES:
        description = con.execute(
            "SELECT description FROM table_catalog WHERE table_name = ?", [table_name]
        ).fetchone()[0]
        assert description == DEPRECATION_PREFIX + "original table"


def test_public_api_snapshot_excludes_only_the_retired_modules():
    snapshot = json.loads((Path(__file__).parent / "data/public_api_snapshot.json").read_text())
    assert not set(RETIRED_MODULES).intersection(snapshot["atx_db"])
    assert set(SCRIPT_BACKED_MODULES) <= set(snapshot["atx_db"])
    assert {"enterprise_value", "valuation_multiples"} <= set(snapshot["atx_db"])
