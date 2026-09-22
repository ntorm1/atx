"""Tier1-S4 T7: every public schema is measured, and the measure is meaningful."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.api.catalog import get_schema
from atx_db.connection import DuckDBStore
from atx_db.provider_coverage import (
    ProviderCoverageOptions,
    coverage_gate_item_count,
    refresh_provider_coverage,
)


def test_every_public_schema_has_an_active_slo(tmp_store):
    from atx_db.api.catalog import DATASETS

    missing = []
    for dataset in DATASETS:
        for schema in dataset.schemas:
            count = tmp_store.con.execute(
                "SELECT count(*) FROM api_schema_coverage_slo "
                "WHERE dataset_id = ? AND schema_code = ? AND is_active",
                [dataset.code, schema.code],
            ).fetchone()[0]
            if int(count) == 0:
                missing.append(f"{dataset.code}/{schema.code}")
    assert missing == []


def test_refresh_provider_coverage_covers_every_schema(tmp_store):
    from atx_db.api.catalog import DATASETS
    from atx_db.provider_coverage import ProviderCoverageOptions, refresh_provider_coverage

    snapshots = refresh_provider_coverage(
        tmp_store,
        ProviderCoverageOptions(observed_at=dt.datetime(2024, 6, 28, 12, 0, 0), run_id="t"),
    )
    assert len(snapshots) == sum(len(dataset.schemas) for dataset in DATASETS)


@pytest.mark.parametrize(
    "dataset_code,schema_code",
    [
        ("ATX.US.FUNDAMENTALS", "security-master"),
        ("ATX.US.EQUITIES", "universe"),
        ("ATX.US.EQUITIES", "delistings"),
    ],
)
def test_the_three_new_schemas_are_registered(dataset_code, schema_code):
    from atx_db.api.catalog import get_schema

    schema = get_schema(dataset_code, schema_code)
    assert schema.code == schema_code
    assert "security_id" in schema.field_names


def test_the_security_master_schema_never_exposes_cusip(tmp_store):
    from atx_db.api.catalog import get_schema

    schema = get_schema("ATX.US.FUNDAMENTALS", "security-master")
    assert "cusip" not in schema.field_names
    columns = {
        str(row[0])
        for row in tmp_store.con.execute(
            "SELECT column_name FROM duckdb_columns() WHERE table_name = ?",
            [schema.source_table],
        ).fetchall()
    }
    assert "cusip" not in columns
    assert {"security_id", "as_of_date", "available_at", "run_id", "source_loaded_at"} <= columns


def test_every_schema_relation_carries_the_columns_the_service_needs(tmp_store):
    from atx_db.api.catalog import DATASETS

    required = {"security_id", "available_at", "source_loaded_at", "run_id", "as_of_date"}
    for dataset in DATASETS:
        for schema in dataset.schemas:
            columns = {
                str(row[0])
                for row in tmp_store.con.execute(
                    "SELECT column_name FROM duckdb_columns() WHERE table_name = ?",
                    [schema.source_table],
                ).fetchall()
            }
            assert required <= columns, f"{dataset.code}/{schema.code} missing {required - columns}"
            assert schema.time_column in columns


def test_item_count_basis_is_declared_per_schema():
    from atx_db.provider_coverage import DEFAULT_PROVIDER_COVERAGE_SLOS

    by_key = {(slo.dataset_id, slo.schema_code): slo for slo in DEFAULT_PROVIDER_COVERAGE_SLOS}
    assert by_key[("ATX.US.FUNDAMENTALS", "reported")].item_count_basis == "distinct"
    assert by_key[("ATX.US.FUNDAMENTALS", "standardized")].item_count_basis == "coverage_gate"


def test_the_coverage_gate_drives_the_standardized_item_count(tmp_store):
    from atx_db.provider_coverage import coverage_gate_item_count

    assert coverage_gate_item_count(tmp_store) == 0
    from tests.item_coverage_fixtures import seed_gate_evidence
    seed_gate_evidence(tmp_store, items=(1101,1201))
    tmp_store.con.execute("UPDATE fundamental_item_coverage SET n_with_value=1500,coverage_pct=50 WHERE item_id=1201")
    assert coverage_gate_item_count(tmp_store) == 1


def test_a_thin_standardized_layer_reports_degraded(tmp_store):
    from atx_db.provider_coverage import ProviderCoverageOptions, refresh_provider_coverage

    tmp_store.con.execute(
        "INSERT INTO fundamental_standardized (standardized_id, source, upstream_source, "
        "security_id, cik, item_id, canonical_code, basis, period_end, value, available_at, "
        "as_of_date, input_codes_json, input_item_ids_json, rule_id, combination_rule, "
        "is_latest_revision) VALUES "
        "('s1','t','sec','SEC-1','0000000001',1101,'total_assets','instant',DATE '2024-03-31',"
        "1.0,TIMESTAMP '2024-05-01 22:00:00',DATE '2024-03-31','[]','[]','r1','identity',true)"
    )
    snapshots = {
        (s.dataset_id, s.schema_code): s
        for s in refresh_provider_coverage(
            tmp_store,
            ProviderCoverageOptions(observed_at=dt.datetime(2024, 6, 28, 12, 0, 0), run_id="t"),
        )
    }
    standardized = snapshots[("ATX.US.FUNDAMENTALS", "standardized")]
    assert standardized.condition == "degraded"
    assert any(f["metric"] == "item_count" for f in standardized.failed_slos)
    assert standardized.item_count == 0  # no coverage rows, so the gate reports zero items


def test_standardized_condition_flips_only_at_measured_target(tmp_store: DuckDBStore) -> None:
    test_a_thin_standardized_layer_reports_degraded(tmp_store)
    # Relax only unrelated SLOs: the production 110-item target remains intact.
    tmp_store.con.execute(
        "UPDATE api_schema_coverage_slo SET expected_history_start=DATE '2024-03-31', "
        "minimum_history_years=0, minimum_security_count=1 WHERE schema_code='standardized'"
    )
    from tests.item_coverage_fixtures import seed_gate_evidence
    seed_gate_evidence(tmp_store, items=tuple(range(1,111)))
    tmp_store.con.execute(
        "UPDATE fundamental_item_coverage SET n_with_value=2670,coverage_pct=89 "
        "WHERE item_id=110 AND fiscal_year=2021"
    )
    for expected_count, condition in ((109, 'degraded'), (110, 'available')):
        snapshots = refresh_provider_coverage(
            tmp_store, ProviderCoverageOptions(observed_at=dt.datetime(2024, 6, 28), run_id=condition)
        )
        snapshot = next(row for row in snapshots if row.schema_code == 'standardized')
        assert snapshot.item_count == expected_count
        assert snapshot.condition == condition
        assert {failure['metric'] for failure in snapshot.failed_slos} == (
            {'item_count'} if expected_count == 109 else set()
        )
        tmp_store.con.execute(
            "UPDATE fundamental_item_coverage SET n_with_value=2700,coverage_pct=90 "
            "WHERE item_id=110 AND fiscal_year=2021"
        )
    # Missing measurements must never revert to the raw distinct-code count (one).
    tmp_store.con.execute("DROP TABLE fundamental_item_coverage")
    assert coverage_gate_item_count(tmp_store) is None
    snapshots = refresh_provider_coverage(
        tmp_store, ProviderCoverageOptions(observed_at=dt.datetime(2024, 6, 29), run_id='absent')
    )
    snapshot = next(row for row in snapshots if row.schema_code == 'standardized')
    assert snapshot.item_count is None
    assert snapshot.condition == 'degraded'


def test_wide_market_panel_counts_populated_metric_columns(tmp_store: DuckDBStore) -> None:
    schema = get_schema('ATX.US.EQUITIES', 'market-daily-1d')
    assert len(schema.coverage_item_columns) == 30
    tmp_store.con.execute(
        "INSERT INTO market_daily_metrics (market_daily_id, source, security_id, trade_date, "
        "available_at, inputs_hash, as_of_date, market_cap) VALUES "
        "('m1','test','SEC-1',DATE '2024-06-27',TIMESTAMP '2024-06-27 22:00:00',"
        "'hash',DATE '2024-06-27',1.0)"
    )
    tmp_store.con.execute(
        "UPDATE api_schema_coverage_slo SET expected_history_start=DATE '2024-06-27', "
        "minimum_history_years=0,minimum_security_count=1 WHERE schema_code='market-daily-1d'"
    )
    for expected_count, condition in ((1, 'degraded'), (30, 'available')):
        snapshots = refresh_provider_coverage(
            tmp_store, ProviderCoverageOptions(observed_at=dt.datetime(2024, 6, 28), run_id=condition)
        )
        snapshot = next(row for row in snapshots if row.schema_code == schema.code)
        assert snapshot.item_count == expected_count
        assert snapshot.condition == condition
        updates = ','.join(f'"{column}"=1.0' for column in schema.coverage_item_columns)
        tmp_store.con.execute(f'UPDATE market_daily_metrics SET {updates}')


def test_security_master_uses_latest_open_aliases_and_input_availability(tmp_store: DuckDBStore) -> None:
    tmp_store.con.execute(
        "INSERT INTO securities (security_id, source, first_seen_date, last_seen_date, source_loaded_at) "
        "VALUES ('SEC-1','test',DATE '2020-01-01',DATE '2024-06-01',TIMESTAMP '2024-06-02')"
    )
    tmp_store.con.execute(
        "INSERT INTO security_identifier_history (security_id,id_type,id_value,valid_from,valid_to,"
        "as_of_date,available_at,source,source_loaded_at,is_latest_revision) VALUES "
        "('SEC-1','CIK','0001',DATE '2020-01-01',NULL,DATE '2024-01-01',TIMESTAMP '2024-01-02','t',TIMESTAMP '2024-01-02',true),"
        "('SEC-1','CIK','9999',DATE '2020-01-01',NULL,DATE '2024-01-01',TIMESTAMP '2024-01-02','t',TIMESTAMP '2024-01-02',false),"
        "('SEC-1','LEI','closed',DATE '2020-01-01',DATE '2020-02-01',DATE '2020-01-01',TIMESTAMP '2020-01-02','t',TIMESTAMP '2020-01-02',true)"
    )
    assert tmp_store.con.execute(
        "SELECT cik,lei,as_of_date,available_at,source_loaded_at FROM v_security_master_public"
    ).fetchone() == ('0001', None, dt.date(2024, 6, 1), dt.datetime(2024, 6, 2), dt.datetime(2024, 6, 2))
    tmp_store.con.execute(
        "UPDATE security_identifier_history SET available_at=TIMESTAMP '2024-06-03',"
        "source_loaded_at=TIMESTAMP '2024-06-04' WHERE id_value='0001'"
    )
    assert tmp_store.con.execute(
        "SELECT available_at,source_loaded_at FROM v_security_master_public"
    ).fetchone() == (dt.datetime(2024, 6, 3), dt.datetime(2024, 6, 4))


def test_slo_item_count_basis_is_not_nullable(tmp_store: DuckDBStore) -> None:
    assert tmp_store.con.execute(
        "SELECT is_nullable,column_default FROM information_schema.columns "
        "WHERE table_name='api_schema_coverage_slo' AND column_name='item_count_basis'"
    ).fetchone() == ('NO', "'distinct'")
    assert tmp_store.con.execute(
        "SELECT nullable FROM field_catalog WHERE table_name='api_schema_coverage_slo' "
        "AND field_name='item_count_basis'"
    ).fetchone() == (False,)
    assert tmp_store.con.execute(
        "SELECT count(*) FROM duckdb_indexes() WHERE index_name='idx_api_schema_coverage_slo_active'"
    ).fetchone() == (1,)
