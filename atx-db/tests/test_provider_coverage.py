from __future__ import annotations

import datetime as dt
import json
from typing import Any, cast

import pytest
from fastapi.testclient import TestClient

from atx_db.api.app import create_app
from atx_db.api.auth import ApiPrincipal, StaticApiKeyAuthenticator
from atx_db.api.service import WarehouseReadService
from atx_db.connection import DuckDBStore
from atx_db.provider_coverage import (
    DEFAULT_PROVIDER_COVERAGE_SLOS,
    ProviderCoverageOptions,
    refresh_provider_coverage,
)
from atx_db.quality._runner import run_warehouse_quality_checks

API_KEY = "atx_coverage_00000000000000000000"
OBSERVED_AT = dt.datetime(2024, 2, 2, 12)


def _insert_standardized(store: DuckDBStore) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id,source,upstream_source,security_id,symbol,cik,item_id,
            canonical_code,basis,period_start,period_end,fiscal_year,fiscal_period,
            value,unit_type,source_accession,filed_date,as_of_date,available_at,
            input_codes_json,input_item_ids_json,rule_id,combination_rule,
            is_latest_revision,run_id,source_loaded_at,updated_at,unit,
            revision_group_id
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        """,
        [
            "coverage-standardized-1",
            "test-coverage",
            "test-input",
            "SEC-COVERAGE-1",
            "COV",
            "0000000001",
            1001,
            "revenue",
            "annual",
            dt.date(2023, 1, 1),
            dt.date(2023, 12, 31),
            2023,
            "FY",
            100.0,
            "monetary",
            "coverage-accession",
            dt.date(2024, 2, 1),
            dt.date(2023, 12, 31),
            dt.datetime(2024, 2, 1, 12),
            '["Revenue"]',
            "[1001]",
            "coverage-rule",
            "identity",
            True,
            "coverage-run",
            dt.datetime(2024, 2, 1, 12),
            dt.datetime(2024, 2, 1, 12),
            "USD",
            "coverage-revision-group",
        ],
    )


def _close_store_for_serving(store: DuckDBStore) -> None:
    store.con.execute("CHECKPOINT")
    assert store.connection is not None
    store.connection.close()
    store.connection = None


def test_provider_coverage_migration_seeds_every_public_schema(tmp_store: DuckDBStore) -> None:
    expected = {(slo.dataset_id, slo.schema_code) for slo in DEFAULT_PROVIDER_COVERAGE_SLOS}
    actual = set(
        tmp_store.con.execute(
            "SELECT dataset_id,schema_code FROM api_schema_coverage_slo WHERE is_active"
        ).fetchall()
    )
    assert actual == expected
    assert len(actual) == 7

    snapshots = refresh_provider_coverage(
        tmp_store,
        ProviderCoverageOptions(observed_at=OBSERVED_AT, run_id="empty-coverage"),
    )
    assert len(snapshots) == 7
    assert {snapshot.condition for snapshot in snapshots} == {"pending"}
    assert tmp_store.con.execute(
        "SELECT count(*) FROM v_api_schema_coverage_current"
    ).fetchone() == (7,)


def test_provider_coverage_measures_range_breadth_and_slo_failure(tmp_store: DuckDBStore) -> None:
    _insert_standardized(tmp_store)
    snapshots = refresh_provider_coverage(
        tmp_store,
        ProviderCoverageOptions(
            dataset_ids=("ATX.US.FUNDAMENTALS",),
            observed_at=OBSERVED_AT,
            run_id="measured-coverage",
        ),
    )
    by_schema = {snapshot.schema_code: snapshot for snapshot in snapshots}
    standardized = by_schema["standardized"]

    assert standardized.condition == "degraded"
    assert standardized.record_count == 1
    assert standardized.security_count == 1
    assert standardized.item_count == 1
    assert standardized.basis_count == 1
    assert standardized.start == dt.datetime(2023, 12, 31)
    assert standardized.end == dt.datetime(2024, 1, 1)
    assert {failure["metric"] for failure in standardized.failed_slos} == {
        "history_start",
        "history_years",
        "security_count",
        "item_count",
    }
    assert by_schema["reported"].condition == "pending"

    quality = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        dataset_ids=("provider_schema_coverage",),
    )
    assert {result.check_name for result in quality} == {
        "bad_provider_schema_coverage_snapshots",
        "provider_schema_coverage_snapshot_without_slo",
    }
    assert {result.status for result in quality} == {"passed"}


def test_provider_coverage_can_reach_available_under_versioned_target(tmp_store: DuckDBStore) -> None:
    _insert_standardized(tmp_store)
    tmp_store.con.execute(
        """
        UPDATE api_schema_coverage_slo
        SET expected_history_start=DATE '2023-12-31',
            minimum_history_years=0,
            minimum_security_count=1,
            minimum_item_count=1,
            maximum_freshness_lag_days=7
        WHERE dataset_id='ATX.US.FUNDAMENTALS'
          AND schema_code='standardized'
          AND slo_version='1.0.0'
        """
    )
    snapshots = refresh_provider_coverage(
        tmp_store,
        ProviderCoverageOptions(
            dataset_ids=("ATX.US.FUNDAMENTALS",),
            observed_at=OBSERVED_AT,
            run_id="available-coverage",
        ),
    )
    standardized = next(row for row in snapshots if row.schema_code == "standardized")
    assert standardized.condition == "available"
    assert standardized.failed_slos == ()


def test_coverage_metadata_endpoints_are_entitled_and_end_exclusive(tmp_store: DuckDBStore) -> None:
    _insert_standardized(tmp_store)
    refresh_provider_coverage(
        tmp_store,
        ProviderCoverageOptions(
            dataset_ids=("ATX.US.FUNDAMENTALS",),
            observed_at=OBSERVED_AT,
            run_id="api-coverage",
        ),
    )
    path = tmp_store.path
    _close_store_for_serving(tmp_store)

    service = WarehouseReadService(path)
    measured_range = service.dataset_range("ATX.US.FUNDAMENTALS")
    schema_ranges = cast(dict[str, dict[str, Any]], measured_range["schema"])
    standardized_range = schema_ranges["standardized"]
    assert standardized_range["start"] == dt.datetime(2023, 12, 31)
    assert standardized_range["end"] == dt.datetime(2024, 1, 1)
    assert standardized_range["condition"] == "degraded"

    principal = ApiPrincipal(
        account_id="coverage-account",
        key_id="coverage-key",
        scopes=frozenset({"data:read"}),
        datasets=frozenset({"ATX.US.FUNDAMENTALS"}),
    )
    app = create_app(
        database_path=path,
        authenticator=StaticApiKeyAuthenticator({API_KEY: principal}),
    )
    client = TestClient(app)
    headers = {"Authorization": f"Bearer {API_KEY}"}

    unauthorized = client.get(
        "/v1/metadata.get_dataset_range",
        params={"dataset": "ATX.US.FUNDAMENTALS"},
    )
    assert unauthorized.status_code == 401

    dataset_range = client.get(
        "/v1/metadata.get_dataset_range",
        params={"dataset": "ATX.US.FUNDAMENTALS"},
        headers=headers,
    )
    assert dataset_range.status_code == 200
    assert dataset_range.json()["data"]["schema"]["standardized"]["end"].startswith(
        "2024-01-01"
    )

    condition = client.get(
        "/v1/metadata.get_dataset_condition",
        params={
            "dataset": "ATX.US.FUNDAMENTALS",
            "schema": "standardized",
            "start_date": "2023-01-01",
            "end_date": "2025-01-01",
        },
        headers=headers,
    )
    assert condition.status_code == 200
    condition_row = condition.json()["data"][0]
    assert condition_row["condition"] == "degraded"
    assert condition_row["start"] == "2023-01-01"
    assert condition_row["end"] == "2025-01-01"

    coverage = client.get(
        "/v1/metadata.get_schema_coverage",
        params={"dataset": "ATX.US.FUNDAMENTALS", "schema": "standardized"},
        headers=headers,
    )
    assert coverage.status_code == 200
    coverage_row = coverage.json()["data"][0]
    assert coverage_row["record_count"] == 1
    assert coverage_row["minimum_security_count"] == 2_500
    assert {failure["metric"] for failure in coverage_row["failed_slos"]} >= {
        "history_start",
        "security_count",
    }
    assert json.loads(json.dumps(coverage_row))["condition"] == "degraded"

    invalid = client.get(
        "/v1/metadata.get_dataset_condition",
        params={
            "dataset": "ATX.US.FUNDAMENTALS",
            "start_date": "2025-01-01",
            "end_date": "2024-01-01",
        },
        headers=headers,
    )
    assert invalid.status_code == 400


def test_provider_coverage_cli_emits_condition_summary(
    tmp_store: DuckDBStore,
    capsys: pytest.CaptureFixture[str],
) -> None:
    from atx_db.cli import main

    path = tmp_store.path
    _close_store_for_serving(tmp_store)
    assert main(
        [
            "refresh-provider-coverage",
            "--db-path",
            str(path),
            "--observed-at",
            OBSERVED_AT.isoformat(),
            "--run-id",
            "cli-coverage",
        ]
    ) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["run_id"] == "cli-coverage"
    assert payload["schema_count"] == 7
    assert payload["conditions"] == {
        "available": 0,
        "degraded": 0,
        "missing": 0,
        "pending": 7,
    }
