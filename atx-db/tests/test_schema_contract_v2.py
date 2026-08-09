"""PF3-S2 acceptance tests for schema-contract v2."""

from __future__ import annotations

import datetime as dt
import time
from dataclasses import replace

import duckdb
import pandas as pd
import pytest

import atx_db.quality as quality_mod
from atx_db.connection import DuckDBStore
from atx_db.migrations import MIGRATIONS
from atx_db.quality import (
    SEMANTIC_CONTRACT_CHECK_NAME,
    pit_column_presence_check,
    run_warehouse_quality_checks,
    semantic_contract_check,
)
from atx_db.schema import ensure_quant_schema
from atx_db.schema_contract import (
    CONTRACT,
    SCHEMA_CONTRACT_VERSION,
    SchemaContractVersionMismatch,
    ColumnSpec,
    _infer_semantic_sign,
    _infer_semantic_unit,
    assert_schema_contract_version,
    build_contract_manifest,
    schema_contract_sha256,
)
from atx_db.warehouse import insert_frame


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


def _migration_0136():
    return next(migration for migration in MIGRATIONS if migration.version == 136)


def _migration_0137():
    return next(migration for migration in MIGRATIONS if migration.version == 137)


def _pit_fact_specs(*missing: str) -> list[ColumnSpec]:
    omitted = set(missing)
    specs = [
        ColumnSpec(
            "fixture_id",
            "VARCHAR",
            nullable=False,
            is_natural_key=True,
            declared_in="migration",
            unit="identifier",
            sign="bounded",
            scale="nominal",
        )
    ]
    if "as_of_date" not in omitted:
        specs.append(
            ColumnSpec(
                "as_of_date",
                "DATE",
                nullable=False,
                is_pit_column=True,
                declared_in="migration",
                unit="date",
                sign="bounded",
                scale="day",
            )
        )
    if "available_at" not in omitted:
        specs.append(
            ColumnSpec(
                "available_at",
                "TIMESTAMP",
                nullable=True,
                is_pit_column=True,
                declared_in="migration",
                unit="timestamp",
                sign="bounded",
                scale="second",
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
                unit="timestamp",
                sign="bounded",
                scale="second",
            )
        )
    if "run_id" not in omitted:
        specs.append(
            ColumnSpec(
                "run_id",
                "VARCHAR",
                nullable=True,
                is_pit_column=True,
                declared_in="migration",
                unit="identifier",
                sign="bounded",
                scale="nominal",
            )
        )
    if "is_latest_revision" not in omitted:
        specs.append(
            ColumnSpec(
                "is_latest_revision",
                "BOOLEAN",
                nullable=True,
                is_pit_column=True,
                declared_in="migration",
                unit="flag",
                sign="bounded",
                scale="boolean",
            )
        )
    return specs


def _semantic_fixture_manifest() -> dict[str, list[ColumnSpec]]:
    return {
        "semantic_fixture_fact": [
            *_pit_fact_specs(),
            ColumnSpec(
                "share_count",
                "DOUBLE",
                nullable=True,
                declared_in="migration",
                unit="shares",
                sign="non_negative",
                scale="1",
            ),
            ColumnSpec(
                "ownership_ratio",
                "DOUBLE",
                nullable=True,
                declared_in="migration",
                unit="ratio",
                sign="unit_interval",
                scale="1",
            ),
            ColumnSpec(
                "net_change",
                "DOUBLE",
                nullable=True,
                declared_in="migration",
                unit="shares",
                sign="signed",
                scale="1",
            ),
            ColumnSpec(
                "bounded_score",
                "DOUBLE",
                nullable=True,
                declared_in="migration",
                unit="score",
                sign="bounded",
                scale="[0,10]",
            ),
        ]
    }


def _plant_semantic_fixture_fact(
    store: DuckDBStore,
    *,
    share_count: float,
    ownership_ratio: float,
    net_change: float,
    bounded_score: float = 5.0,
) -> dict[str, list[ColumnSpec]]:
    store.con.execute(
        """
        CREATE TABLE semantic_fixture_fact (
            fixture_id VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            source_loaded_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            is_latest_revision BOOLEAN,
            share_count DOUBLE,
            ownership_ratio DOUBLE,
            net_change DOUBLE,
            bounded_score DOUBLE
        )
        """
    )
    store.con.execute(
        """
        INSERT INTO semantic_fixture_fact (
            fixture_id,
            as_of_date,
            available_at,
            source_loaded_at,
            run_id,
            is_latest_revision,
            share_count,
            ownership_ratio,
            net_change,
            bounded_score
        )
        VALUES ('fixture-1', DATE '2026-07-05', TIMESTAMP '2026-07-05 12:00:00',
                TIMESTAMP '2026-07-05 12:00:00', 'semantic-fixture', true, ?, ?, ?, ?)
        """,
        [share_count, ownership_ratio, net_change, bounded_score],
    )
    return _semantic_fixture_manifest()


def _semantic_violation_columns(result) -> set[tuple[str, str]]:
    return {
        (str(row["table_name"]), str(row["column_name"]))
        for row in result.details["violations"]
    }


def _semantic_invalid_declaration_reasons(result) -> dict[tuple[str, str], str]:
    return {
        (str(row["table_name"]), str(row["column_name"])): str(row["reason"])
        for row in result.details["invalid_declarations"]
    }


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


def test_migration_0135_catalogs_recreated_latest_view_pit_columns(tmp_store):
    expected_fields = {
        "v_fundamental_points_latest": {"is_latest_revision"},
        "v_fundamental_ttm_latest": {"run_id"},
        "v_fundamental_periods_latest": {"run_id"},
        "v_macro_latest": {"is_latest_revision", "run_id"},
    }

    missing_live_columns = {}
    missing_catalog_rows = {}
    for table_name, field_names in expected_fields.items():
        live_columns = {
            row[0]
            for row in tmp_store.con.execute(
                """
                SELECT column_name
                FROM duckdb_columns()
                WHERE schema_name = 'main'
                  AND table_name = ?
                """,
                [table_name],
            ).fetchall()
        }
        catalog_rows = {
            row[0]
            for row in tmp_store.con.execute(
                """
                SELECT field_name
                FROM field_catalog
                WHERE table_name = ?
                """,
                [table_name],
            ).fetchall()
        }

        if missing := sorted(field_names - live_columns):
            missing_live_columns[table_name] = missing
        if missing := sorted(field_names - catalog_rows):
            missing_catalog_rows[table_name] = missing

    assert missing_live_columns == {}
    assert missing_catalog_rows == {}


@pytest.mark.slow
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


def test_insert_frame_populates_migrated_pit_columns(tmp_store):
    feature_frame = pd.DataFrame(
        [
            {
                "feature_set": "fixture_set",
                "feature_name": "fixture_feature",
                "security_id": "SEC-1",
                "symbol": "AAA",
                "as_of_date": dt.date(2001, 2, 3),
                "value": 1.25,
                "input_hash": "hash-feature",
                "available_at": dt.datetime(2001, 2, 3, 9, 30),
                "source": "test",
                "run_id": "run-feature",
            }
        ]
    )
    alpha_frame = pd.DataFrame(
        [
            {
                "alpha_signal_id": "signal-1",
                "alpha_id": "alpha-1",
                "security_id": "SEC-1",
                "symbol": "AAA",
                "as_of_date": dt.date(2002, 3, 4),
                "signal_value": 2.5,
                "rank_value": 1.0,
                "weight": 0.5,
                "cross_section_count": 10,
                "available_at": dt.datetime(2002, 3, 4, 9, 30),
                "input_hash": "hash-alpha",
                "source": "test",
                "run_id": "run-alpha",
                "source_loaded_at": pd.NaT,
                "is_latest_revision": None,
            }
        ]
    )

    insert_frame(tmp_store, feature_frame, "feature_values", "feature_values_insert_fixture")
    insert_frame(tmp_store, alpha_frame, "alpha_signal_values", "alpha_signal_values_insert_fixture")

    rows = dict(
        tmp_store.con.execute(
            """
            SELECT 'feature_values' AS table_name,
                   source_loaded_at IS NOT NULL AND is_latest_revision AS pit_populated
            FROM feature_values
            WHERE feature_set = 'fixture_set'
            UNION ALL
            SELECT 'alpha_signal_values' AS table_name,
                   source_loaded_at IS NOT NULL AND is_latest_revision AS pit_populated
            FROM alpha_signal_values
            WHERE alpha_signal_id = 'signal-1'
            """
        ).fetchall()
    )

    assert rows == {"feature_values": True, "alpha_signal_values": True}


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


def test_column_spec_validates_semantic_sign_values():
    ColumnSpec("value", "DOUBLE", nullable=False, unit="USD", sign="non_negative", scale="1")
    ColumnSpec("label", "VARCHAR", nullable=True, sign=None)

    with pytest.raises(ValueError):
        ColumnSpec("value", "DOUBLE", nullable=False, sign="positiveish")


def test_schema_contract_hash_includes_semantic_fields_and_stays_reorder_stable():
    base = {
        "fact_a": [
            ColumnSpec(
                "fact_id",
                "VARCHAR",
                nullable=False,
                is_natural_key=True,
                declared_in="migration",
                unit="identifier",
                sign="bounded",
                scale="nominal",
            ),
            ColumnSpec(
                "value",
                "DOUBLE",
                nullable=False,
                declared_in="migration",
                unit="USD",
                sign="non_negative",
                scale="1",
            ),
        ],
        "fact_b": [
            ColumnSpec(
                "weight",
                "DOUBLE",
                nullable=True,
                declared_in="migration",
                unit="ratio",
                sign="unit_interval",
                scale="1",
            ),
        ],
    }
    changed = {
        **base,
        "fact_a": [
            base["fact_a"][0],
            ColumnSpec(
                "value",
                "DOUBLE",
                nullable=False,
                declared_in="migration",
                unit="USD",
                sign="signed",
                scale="1",
            ),
        ],
    }
    reordered = {
        "fact_b": list(reversed(base["fact_b"])),
        "fact_a": list(reversed(base["fact_a"])),
    }

    assert schema_contract_sha256(base) != schema_contract_sha256(changed)
    assert schema_contract_sha256(base) == schema_contract_sha256(reordered)


def test_schema_contract_table_has_s2_1_semantic_columns_catalogued(tmp_store):
    expected = {"unit", "sign", "scale", "natural_key"}
    live_columns = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT column_name FROM duckdb_columns() WHERE table_name = 'schema_contract'"
        ).fetchall()
    }
    field_rows = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT field_name FROM field_catalog WHERE table_name = 'schema_contract'"
        ).fetchall()
    }

    assert expected <= live_columns
    assert expected <= field_rows


def test_every_manifest_fact_column_resolves_unit_sign_and_scale(tmp_store):
    manifest = build_contract_manifest(tmp_store.con)
    missing = {}
    for table_name, specs in manifest.items():
        if not any(spec.is_pit_column for spec in specs):
            continue
        offenders = [
            spec.name
            for spec in specs
            if spec.unit is None or spec.sign is None or spec.scale is None
        ]
        if offenders:
            missing[table_name] = offenders

    assert missing == {}


@pytest.mark.parametrize(
    "name",
    [
        "shares_change_pct",
        "net_call_share_change_pct",
        "holder_count_change_pct",
        "market_share_growth",
    ],
)
def test_semantic_unit_inference_prefers_ratio_markers_before_share_tokens(name):
    assert _infer_semantic_unit(name, "DOUBLE") == "ratio"


@pytest.mark.parametrize(
    ("name", "data_type", "unit"),
    [
        ("shares_change", "DOUBLE", "shares"),
        ("net_call_share_change", "DOUBLE", "count"),
        ("holder_count_change", "BIGINT", "ratio"),
        ("cash_amount_delta", "DOUBLE", "USD"),
    ],
)
def test_semantic_sign_inference_treats_change_delta_net_values_as_signed(name, data_type, unit):
    assert _infer_semantic_sign(name, data_type, unit) == "signed"


def test_semantic_sign_inference_keeps_percentile_change_bounded():
    assert _infer_semantic_sign("short_interest_change_pct_percentile", "DOUBLE", "ratio") == "unit_interval"


def test_semantic_sign_inference_keeps_percentile_rank_unit_interval():
    assert _infer_semantic_sign("daily_return_cs_pct_rank", "DOUBLE", "percentile") == "unit_interval"


def test_manifest_percentile_rank_example_is_unit_interval(tmp_store):
    specs = {spec.name: spec for spec in build_contract_manifest(tmp_store.con)["equity_price_metrics"]}

    assert (specs["daily_return_cs_pct_rank"].unit, specs["daily_return_cs_pct_rank"].sign) == (
        "percentile",
        "unit_interval",
    )


def test_manifest_change_delta_net_examples_are_signed(tmp_store):
    manifest = build_contract_manifest(tmp_store.con)
    expected = {
        ("thirteenf_position_metrics", "shares_change"): ("shares", "signed"),
        ("thirteenf_position_metrics", "shares_change_pct"): ("ratio", "signed"),
        ("thirteenf_option_metrics", "net_call_share_change"): ("count", "signed"),
        ("thirteenf_option_metrics", "net_call_share_change_pct"): ("ratio", "signed"),
        ("thirteenf_concentration_metrics", "holder_count_change"): ("ratio", "signed"),
    }

    for (table_name, column_name), (unit, sign) in expected.items():
        spec = {spec.name: spec for spec in manifest[table_name]}[column_name]
        assert (spec.unit, spec.sign) == (unit, sign)


def test_persisted_schema_contract_fact_rows_have_semantics(tmp_store):
    manifest = build_contract_manifest(tmp_store.con)
    fact_tables = {table_name for table_name, specs in manifest.items() if any(spec.is_pit_column for spec in specs)}
    assert fact_tables

    rows = tmp_store.con.execute(
        """
        SELECT table_name, column_name, unit, sign, scale, natural_key
        FROM schema_contract
        ORDER BY table_name, column_name
        """
    ).fetchall()
    missing = [
        (table_name, column_name)
        for table_name, column_name, unit, sign, scale, natural_key in rows
        if table_name in fact_tables
        and (unit is None or sign is None or scale is None or natural_key is None)
    ]

    assert missing == []


@pytest.mark.parametrize(
    ("assignment", "missing_field"),
    [
        ("unit = NULL", "unit"),
        ("sign = NULL", "sign"),
        ("scale = NULL", "scale"),
        ("natural_key = NULL", "natural_key"),
    ],
)
def test_semantic_contract_check_flags_persisted_incomplete_semantic_declarations(
    tmp_store, assignment, missing_field
):
    tmp_store.con.execute(
        f"""
        UPDATE schema_contract
        SET {assignment}
        WHERE table_name = 'market_cap'
          AND column_name = 'market_cap'
        """
    )

    result = semantic_contract_check(tmp_store)

    invalid = _semantic_invalid_declaration_reasons(result)
    assert result.status == "failed"
    assert result.observed_value == 1.0
    assert invalid[("market_cap", "market_cap")] == (
        f"missing semantic declaration fields: {missing_field}"
    )


@pytest.mark.parametrize(
    ("field_name", "expected_missing"),
    [
        ("unit", "unit"),
        ("sign", "sign"),
        ("scale", "scale"),
    ],
)
def test_semantic_contract_check_flags_manifest_incomplete_semantic_declarations(
    tmp_store, field_name, expected_missing
):
    manifest = _plant_semantic_fixture_fact(
        tmp_store,
        share_count=10.0,
        ownership_ratio=0.4,
        net_change=-5.0,
    )
    manifest["semantic_fixture_fact"] = [
        replace(spec, **{field_name: None}) if spec.name == "share_count" else spec
        for spec in manifest["semantic_fixture_fact"]
    ]

    result = semantic_contract_check(tmp_store, manifest=manifest)

    invalid = _semantic_invalid_declaration_reasons(result)
    assert result.status == "failed"
    assert result.observed_value == 1.0
    assert invalid[("semantic_fixture_fact", "share_count")] == (
        f"missing semantic declaration fields: {expected_missing}"
    )


def test_semantic_contract_check_flags_negative_non_negative_share_count(tmp_store):
    manifest = _plant_semantic_fixture_fact(
        tmp_store,
        share_count=-1.0,
        ownership_ratio=0.4,
        net_change=-5.0,
    )

    result = semantic_contract_check(tmp_store, manifest=manifest)

    assert result.status == "failed"
    assert result.severity == "critical"
    assert result.observed_value == 1.0
    assert _semantic_violation_columns(result) == {("semantic_fixture_fact", "share_count")}


@pytest.mark.parametrize(
    "share_count",
    [
        pytest.param(float("nan"), id="nan"),
        pytest.param(float("inf"), id="positive-infinity"),
        pytest.param(float("-inf"), id="negative-infinity"),
    ],
)
def test_semantic_contract_check_flags_non_finite_constrained_numeric_values(
    tmp_store, share_count
):
    manifest = _plant_semantic_fixture_fact(
        tmp_store,
        share_count=share_count,
        ownership_ratio=0.4,
        net_change=-5.0,
    )

    result = semantic_contract_check(tmp_store, manifest=manifest)

    assert result.status == "failed"
    assert result.observed_value == 1.0
    assert _semantic_violation_columns(result) == {("semantic_fixture_fact", "share_count")}


def test_semantic_contract_check_flags_out_of_bound_unit_interval(tmp_store):
    manifest = _plant_semantic_fixture_fact(
        tmp_store,
        share_count=10.0,
        ownership_ratio=1.2,
        net_change=-5.0,
    )

    result = semantic_contract_check(tmp_store, manifest=manifest)

    assert result.status == "failed"
    assert result.observed_value == 1.0
    assert _semantic_violation_columns(result) == {("semantic_fixture_fact", "ownership_ratio")}


def test_semantic_contract_check_flags_declared_bounded_domain(tmp_store):
    manifest = _plant_semantic_fixture_fact(
        tmp_store,
        share_count=10.0,
        ownership_ratio=0.4,
        net_change=-5.0,
        bounded_score=12.0,
    )

    result = semantic_contract_check(tmp_store, manifest=manifest)

    assert result.status == "failed"
    assert result.observed_value == 1.0
    assert _semantic_violation_columns(result) == {("semantic_fixture_fact", "bounded_score")}


def test_semantic_contract_check_allows_legitimate_negative_signed_value(tmp_store):
    manifest = _plant_semantic_fixture_fact(
        tmp_store,
        share_count=10.0,
        ownership_ratio=0.4,
        net_change=-5.0,
    )

    result = semantic_contract_check(tmp_store, manifest=manifest)

    assert result.status == "passed"
    assert result.observed_value == 0.0
    assert result.details["violations"] == []
    assert result.details["signed_columns"] == 1


def test_semantic_contract_check_registered_gate_ready_and_runs_from_persisted_tier(tmp_store):
    registry_row = tmp_store.con.execute(
        """
        SELECT
            dataset_id,
            table_name,
            severity,
            threshold_value,
            comparator,
            enabled,
            failure_status,
            source
        FROM quality_check_registry
        WHERE check_name = ?
        """,
        [SEMANTIC_CONTRACT_CHECK_NAME],
    ).fetchone()
    assert registry_row == (
        "schema_contract",
        "schema_contract",
        "critical",
        0.0,
        "eq",
        True,
        "failed",
        "schema_contract",
    )

    results = {
        result.check_name: result
        for result in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=(SEMANTIC_CONTRACT_CHECK_NAME,),
        )
    }

    result = results[SEMANTIC_CONTRACT_CHECK_NAME]
    assert result.status == "passed"
    assert result.severity == "critical"
    assert result.details["source"] == "schema_contract"
    assert result.observed_value == 0.0


def test_disabled_semantic_contract_registry_entry_suppresses_selected_scan(
    tmp_store, monkeypatch
):
    tmp_store.con.execute(
        """
        UPDATE quality_check_registry
        SET enabled = false
        WHERE check_name = ?
        """,
        [SEMANTIC_CONTRACT_CHECK_NAME],
    )

    def fail_if_scanned(*args, **kwargs):
        raise AssertionError("semantic_contract_check should not run when disabled")

    monkeypatch.setattr(quality_mod, "semantic_contract_check", fail_if_scanned)

    results = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        check_names=(SEMANTIC_CONTRACT_CHECK_NAME,),
    )

    assert results == []


def test_contract_table_column_uses_field_catalog_unit_fallback(tmp_store):
    manifest = build_contract_manifest(tmp_store.con)
    contract_applied_at = next(
        spec for spec in CONTRACT["schema_migrations"] if spec.name == "applied_at"
    )
    manifest_applied_at = next(
        spec for spec in manifest["schema_migrations"] if spec.name == "applied_at"
    )
    persisted_unit = tmp_store.con.execute(
        """
        SELECT unit
        FROM schema_contract
        WHERE table_name = 'schema_migrations'
          AND column_name = 'applied_at'
        """
    ).fetchone()[0]

    assert contract_applied_at.unit is None
    assert manifest_applied_at.unit == "timestamp"
    assert persisted_unit == "timestamp"


def test_migration_0136_prunes_stale_snapshot_row_without_hash_pollution(tmp_store):
    tmp_store.con.execute(
        """
        INSERT OR REPLACE INTO schema_contract (
            table_name, column_name, data_type, nullable, is_natural_key, is_pit_column,
            declared_in, manifest_sha256, source_loaded_at, unit, sign, scale, natural_key
        )
        VALUES (
            '_schema_contract_0136_existing', 'table_name', 'VARCHAR', false, false, false,
            'migration', 'stale-temp-hash', ?, 'identifier', 'bounded', 'nominal', false
        )
        """,
        [dt.datetime(2001, 1, 1)],
    )

    _migration_0136().up(tmp_store.con)
    manifest = build_contract_manifest(tmp_store.con)
    manifest_sha = schema_contract_sha256(manifest)
    hashes = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT DISTINCT manifest_sha256 FROM schema_contract"
        ).fetchall()
    }
    stale_count = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM schema_contract
        WHERE table_name = '_schema_contract_0136_existing'
        """
    ).fetchone()[0]

    assert "_schema_contract_0136_existing" not in manifest
    assert stale_count == 0
    assert hashes == {manifest_sha}


def test_migration_0136_schema_contract_semantic_seed_is_idempotent(tmp_store):
    def stable_rows():
        return tmp_store.con.execute(
            """
            SELECT
                table_name, column_name, data_type, nullable, is_natural_key, is_pit_column,
                declared_in, manifest_sha256, unit, sign, scale, natural_key, source_loaded_at
            FROM schema_contract
            ORDER BY table_name, column_name
            """
        ).fetchall()

    _migration_0136().up(tmp_store.con)
    before_rows = stable_rows()
    before_sha = tmp_store.con.execute("SELECT DISTINCT manifest_sha256 FROM schema_contract").fetchall()

    time.sleep(0.05)
    _migration_0136().up(tmp_store.con)
    _migration_0136().up(tmp_store.con)

    after_rows = stable_rows()
    after_sha = tmp_store.con.execute("SELECT DISTINCT manifest_sha256 FROM schema_contract").fetchall()

    assert after_rows == before_rows
    assert after_sha == before_sha
    assert len(after_sha) == 1


def test_schema_contract_version_table_pins_v2_manifest_hash_and_is_catalogued(tmp_store):
    expected_columns = {
        "version",
        "manifest_sha256",
        "declared_by_migration",
        "source_loaded_at",
    }
    live_columns = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT column_name FROM duckdb_columns() WHERE table_name = 'schema_contract_version'"
        ).fetchall()
    }
    field_rows = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT field_name FROM field_catalog WHERE table_name = 'schema_contract_version'"
        ).fetchall()
    }
    table_catalog_count = tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'schema_contract_version'"
    ).fetchone()[0]
    manifest_sha = schema_contract_sha256(build_contract_manifest(tmp_store.con))
    version_row = tmp_store.con.execute(
        """
        SELECT version, manifest_sha256, declared_by_migration
        FROM schema_contract_version
        """
    ).fetchone()

    assert live_columns == expected_columns
    assert field_rows == expected_columns
    assert table_catalog_count == 1
    assert version_row == (SCHEMA_CONTRACT_VERSION, manifest_sha, "0137")
    assert_schema_contract_version(tmp_store.con)


def test_schema_contract_version_hash_mismatch_fails_pin_check(tmp_store):
    tmp_store.con.execute(
        """
        UPDATE schema_contract_version
        SET manifest_sha256 = repeat('0', 64)
        WHERE version = ?
        """,
        [SCHEMA_CONTRACT_VERSION],
    )

    with pytest.raises(SchemaContractVersionMismatch, match="live manifest"):
        assert_schema_contract_version(tmp_store.con)


def test_schema_contract_version_bump_without_persisted_row_fails_pin_check(tmp_store):
    with pytest.raises(SchemaContractVersionMismatch, match="not persisted"):
        assert_schema_contract_version(tmp_store.con, expected_version="v3")


def test_panel_contract_module_imports_with_stable_hash_and_pit_keys():
    import importlib

    import atx_db.panel_contract as panel_contract

    reloaded = importlib.reload(panel_contract)
    column_names = {spec.name for spec in reloaded.PANEL_CONTRACT}
    keys = tuple(
        spec.name
        for spec in reloaded.PANEL_CONTRACT
        if spec.is_panel_key
    )

    assert reloaded.PANEL_PIT_KEYS == ("security_id", "as_of_date")
    assert keys == reloaded.PANEL_PIT_KEYS
    assert reloaded.panel_contract_sha256(reloaded.PANEL_CONTRACT) == reloaded.PANEL_CONTRACT_SHA256
    assert reloaded.panel_contract_sha256(reloaded.PANEL_CONTRACT) == panel_contract.panel_contract_sha256(
        panel_contract.PANEL_CONTRACT
    )
    assert all(spec.unit and spec.sign and spec.scale for spec in reloaded.PANEL_CONTRACT)
    assert {
        "security_id",
        "as_of_date",
        "factor_id",
        "value",
        "available_at",
        "source_loaded_at",
        "run_id",
        "input_lineage_json",
    } <= column_names
    assert {
        "close",
        "simple_return",
        "log_return",
        "volume",
        "dollar_volume",
        "market_cap",
        "shares_outstanding",
    }.isdisjoint(column_names)


def test_panel_contract_table_catalog_rows_are_seeded_and_idempotent(tmp_store):
    from atx_db.panel_contract import PANEL_CONTRACT, PANEL_CONTRACT_SHA256, PANEL_PIT_KEYS

    expected_columns = {
        "column_name",
        "data_type",
        "is_panel_key",
        "key_ordinal",
        "unit",
        "sign",
        "scale",
        "panel_contract_sha256",
        "source_loaded_at",
    }

    def stable_rows():
        return tmp_store.con.execute(
            """
            SELECT
                column_name,
                data_type,
                is_panel_key,
                key_ordinal,
                unit,
                sign,
                scale,
                panel_contract_sha256,
                source_loaded_at
            FROM panel_contract
            ORDER BY column_name
            """
        ).fetchall()

    _migration_0137().up(tmp_store.con)

    live_columns = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT column_name FROM duckdb_columns() WHERE table_name = 'panel_contract'"
        ).fetchall()
    }
    field_rows = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT field_name FROM field_catalog WHERE table_name = 'panel_contract'"
        ).fetchall()
    }
    table_catalog_count = tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'panel_contract'"
    ).fetchone()[0]
    before_rows = stable_rows()
    key_rows = tmp_store.con.execute(
        """
        SELECT column_name, key_ordinal
        FROM panel_contract
        WHERE is_panel_key
        ORDER BY key_ordinal
        """
    ).fetchall()
    hashes = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT DISTINCT panel_contract_sha256 FROM panel_contract"
        ).fetchall()
    }
    panel_columns = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT column_name FROM panel_contract"
        ).fetchall()
    }

    time.sleep(0.05)
    _migration_0137().up(tmp_store.con)
    after_rows = stable_rows()

    assert live_columns == expected_columns
    assert field_rows == expected_columns
    assert table_catalog_count == 1
    assert len(before_rows) == len(PANEL_CONTRACT)
    assert key_rows == [(PANEL_PIT_KEYS[0], 1), (PANEL_PIT_KEYS[1], 2)]
    assert hashes == {PANEL_CONTRACT_SHA256}
    assert {"factor_id", "value", "input_lineage_json"} <= panel_columns
    assert {"close", "volume", "market_cap"}.isdisjoint(panel_columns)
    assert after_rows == before_rows


def test_semantic_contract_quality_registry_registration_is_owned_by_0137(tmp_store):
    tmp_store.con.execute(
        "DELETE FROM quality_check_registry WHERE check_name = ?",
        [SEMANTIC_CONTRACT_CHECK_NAME],
    )

    _migration_0136().up(tmp_store.con)
    row_after_0136 = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM quality_check_registry
        WHERE check_name = ?
        """,
        [SEMANTIC_CONTRACT_CHECK_NAME],
    ).fetchone()[0]

    _migration_0137().up(tmp_store.con)
    _migration_0137().up(tmp_store.con)
    row_after_0137 = tmp_store.con.execute(
        """
        SELECT dataset_id, table_name, severity, threshold_value, comparator, enabled,
               failure_status, source
        FROM quality_check_registry
        WHERE check_name = ?
        """,
        [SEMANTIC_CONTRACT_CHECK_NAME],
    ).fetchone()

    assert row_after_0136 == 0
    assert row_after_0137 == (
        "schema_contract",
        "schema_contract",
        "critical",
        0.0,
        "eq",
        True,
        "failed",
        "schema_contract",
    )


def test_semantic_contract_quality_registry_registration_is_append_only(tmp_store):
    tmp_store.con.execute(
        """
        UPDATE quality_check_registry
        SET enabled = false,
            severity = 'warning',
            updated_at = TIMESTAMP '2001-01-01 00:00:00'
        WHERE check_name = ?
        """,
        [SEMANTIC_CONTRACT_CHECK_NAME],
    )

    _migration_0137().up(tmp_store.con)

    row = tmp_store.con.execute(
        """
        SELECT severity, enabled, updated_at
        FROM quality_check_registry
        WHERE check_name = ?
        """,
        [SEMANTIC_CONTRACT_CHECK_NAME],
    ).fetchone()

    assert row == ("warning", False, dt.datetime(2001, 1, 1))


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
