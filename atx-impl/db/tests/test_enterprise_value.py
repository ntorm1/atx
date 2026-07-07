from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from db.enterprise_value import (
    EnterpriseValueDataset,
    EnterpriseValueOptions,
    compute_enterprise_value_rows,
    refresh_enterprise_value,
)
from db.warehouse import insert_frame


SECURITY_ID = "SEC-EV"


def _market_cap_row(
    *,
    security_id: str = SECURITY_ID,
    symbol: str = "EVT",
    trade_date: dt.date = dt.date(2020, 1, 2),
    available_at: dt.datetime = dt.datetime(2020, 1, 2, 22),
    market_cap: float = 1000.0,
) -> dict[str, object]:
    return {
        "market_cap_id": f"mc-{security_id}-{trade_date}",
        "source": "derived_market_cap_v1",
        "price_source": "fixture_prices",
        "share_source": "fixture_shares",
        "security_id": security_id,
        "symbol": symbol,
        "trade_date": trade_date,
        "close": 10.0,
        "share_count": market_cap / 10.0,
        "share_count_type_used": "shares_diluted_avg",
        "market_cap": market_cap,
        "is_latest_revision": True,
        "as_of_date": trade_date,
        "available_at": available_at,
        "price_available_at": available_at,
        "share_available_at": available_at,
        "price_run_id": "price-run",
        "share_run_id": "share-run",
        "share_history_id": f"share-{security_id}",
        "input_codes_json": '{"market_cap": "fixture"}',
        "input_lineage_json": '{"market_cap": "fixture"}',
        "run_id": "market-run",
    }


def _statement_row(
    metric: str,
    value: float,
    *,
    security_id: str = SECURITY_ID,
    symbol: str = "EVT",
    period_end: dt.date = dt.date(2019, 12, 31),
    available_at: dt.datetime = dt.datetime(2020, 2, 1, 10),
) -> dict[str, object]:
    return {
        "statement_point_id": f"stmt-{security_id}-{metric}",
        "fact_revision_id": f"fact-{security_id}-{metric}",
        "revision_group_id": f"rg-{security_id}-{metric}",
        "source": "fixture_statement",
        "security_id": security_id,
        "symbol": symbol,
        "cik": "0000000001",
        "statement_type": "balance_sheet",
        "statement_section": "valuation",
        "canonical_metric": metric,
        "canonical_label": metric.replace("_", " ").title(),
        "taxonomy": "us-gaap",
        "concept": metric,
        "unit": "USD",
        "unit_type": "monetary",
        "period_type": "instant",
        "normal_balance": "credit",
        "period_start": None,
        "period_end": period_end,
        "as_of_date": period_end,
        "available_at": available_at,
        "fiscal_year": period_end.year,
        "fiscal_period": "FY",
        "form": "10-K",
        "accession_number": f"acc-{security_id}-{period_end:%Y%m%d}",
        "revision_sequence": 1,
        "revision_count": 1,
        "is_latest_revision": True,
        "is_value_changed": False,
        "raw_value": value,
        "value": value,
        "previous_raw_value": None,
        "previous_value": None,
        "value_delta": None,
        "value_delta_percent": None,
        "run_id": "statement-run",
        "source_url": "fixture",
        "source_loaded_at": available_at,
    }


def _seed_enterprise_value_inputs(tmp_store, *, cash_available_at: dt.datetime | None = None) -> None:
    cash_av = cash_available_at or dt.datetime(2020, 2, 5, 10)
    insert_frame(
        tmp_store,
        pd.DataFrame([_market_cap_row()]),
        "market_cap",
        "enterprise_value_market_cap_seed",
    )
    insert_frame(
        tmp_store,
        pd.DataFrame(
            [
                _statement_row("total_debt", 200.0, available_at=dt.datetime(2020, 2, 1, 10)),
                _statement_row("pref_stock", 25.0, available_at=dt.datetime(2020, 2, 2, 10)),
                _statement_row("minority_int_bs", 10.0, available_at=dt.datetime(2020, 2, 3, 10)),
                _statement_row("cash_st_inv", 50.0, available_at=cash_av),
            ]
        ),
        "fundamental_statement_points",
        "enterprise_value_statement_seed",
    )


def _share_history_row(share_count_type: str, share_count: float) -> dict[str, object]:
    return {
        "share_history_id": f"share-{share_count_type}",
        "source": "fixture_shares",
        "security_id": SECURITY_ID,
        "symbol": "EVT",
        "cik": "0000000001",
        "share_count_type": share_count_type,
        "share_class": None,
        "share_count_category": "float_treasury"
        if share_count_type in {"float", "treasury"}
        else "consolidated",
        "taxonomy": "us-gaap",
        "concept": share_count_type,
        "unit": "shares",
        "period_type": "duration" if share_count_type == "shares_diluted_avg" else "instant",
        "period_start": dt.date(2019, 1, 1)
        if share_count_type == "shares_diluted_avg"
        else None,
        "period_end": dt.date(2019, 12, 31),
        "effective_date": dt.date(2019, 12, 31),
        "as_of_date": dt.date(2020, 2, 1),
        "available_at": dt.datetime(2020, 2, 1, 10),
        "fiscal_year": 2019,
        "fiscal_period": "FY",
        "form": "10-K",
        "accession_number": "acc-coverage",
        "revision_sequence": 1,
        "revision_count": 1,
        "is_latest_revision": True,
        "share_count": share_count,
        "source_url": "fixture",
        "run_id": "coverage-shares",
    }


def _seed_valuation_coverage_inputs(tmp_store) -> None:
    _seed_enterprise_value_inputs(tmp_store)
    assert refresh_enterprise_value(tmp_store, EnterpriseValueOptions(run_id="coverage-ev")) == 1
    insert_frame(
        tmp_store,
        pd.DataFrame(
            [
                _share_history_row("shares_diluted_avg", 100.0),
                _share_history_row("float", 80.0),
                _share_history_row("treasury", 5.0),
            ]
        ),
        "shares_outstanding_history",
        "valuation_input_share_seed",
    )
    tmp_store.con.execute(
        """
        INSERT INTO universe_membership (
            universe_id,
            security_id,
            symbol,
            valid_from,
            valid_to,
            as_of_date,
            is_member,
            reason,
            rules_json,
            decision_count,
            available_at,
            source,
            run_id,
            is_latest_revision
        )
        VALUES (
            'us_common_equity_liquid_v1',
            ?,
            'EVT',
            DATE '2020-01-02',
            NULL,
            DATE '2020-01-02',
            true,
            'member',
            '{}',
            1,
            TIMESTAMP '2020-03-01 00:00:00',
            'fixture_universe',
            'coverage-universe',
            true
        )
        """,
        [SECURITY_ID],
    )


def _wide_row(**overrides) -> dict[str, object]:
    row = {
        "market_cap_source": "derived_market_cap_v1",
        "market_cap_id": "mc-ev",
        "security_id": SECURITY_ID,
        "symbol": "EVT",
        "trade_date": dt.date(2020, 1, 2),
        "period_end": dt.date(2019, 12, 31),
        "fiscal_year": 2019,
        "fiscal_period": "FY",
        "price": 10.0,
        "share_count": 100.0,
        "share_count_type_used": "shares_diluted_avg",
        "market_cap": 1000.0,
        "market_cap_available_at": pd.Timestamp("2020-01-02 22:00:00"),
        "price_available_at": pd.Timestamp("2020-01-02 22:00:00"),
        "share_available_at": pd.Timestamp("2020-01-02 22:00:00"),
        "market_cap_input_lineage_json": '{"market_cap": "fixture"}',
        "total_debt": 200.0,
        "total_debt_available_at": pd.Timestamp("2020-02-01 10:00:00"),
        "total_debt_id": "stmt-debt",
        "total_debt_source": "fixture_statement",
        "preferred_equity": 25.0,
        "preferred_equity_available_at": pd.Timestamp("2020-02-02 10:00:00"),
        "preferred_equity_id": "stmt-pref",
        "preferred_equity_source": "fixture_statement",
        "minority_interest": 10.0,
        "minority_interest_available_at": pd.Timestamp("2020-02-03 10:00:00"),
        "minority_interest_id": "stmt-minority",
        "minority_interest_source": "fixture_statement",
        "cash_and_equivalents": 50.0,
        "cash_and_equivalents_available_at": pd.Timestamp("2020-02-05 10:00:00"),
        "cash_and_equivalents_id": "stmt-cash",
        "cash_and_equivalents_source": "fixture_statement",
    }
    row.update(overrides)
    return row


def test_compute_enterprise_value_rows_returns_hand_computed_ev_and_lineage() -> None:
    rows = compute_enterprise_value_rows(pd.DataFrame([_wide_row()]), run_id="ev-run")

    assert len(rows) == 1
    row = rows.iloc[0]
    assert row["enterprise_value"] == pytest.approx(1185.0)
    assert row["available_at"] == pd.Timestamp("2020-02-05 10:00:00")
    assert row["run_id"] == "ev-run"
    lineage = json.loads(row["input_lineage_json"])
    assert lineage["components"]["cash_and_equivalents"]["sign"] == "subtract"
    assert lineage["components"]["total_debt"]["sign"] == "add"


def test_compute_enterprise_value_rejects_negative_cash_component() -> None:
    with pytest.raises(ValueError, match="cash_and_equivalents"):
        compute_enterprise_value_rows(pd.DataFrame([_wide_row(cash_and_equivalents=-50.0)]))


def test_refresh_enterprise_value_is_idempotent_and_asof_visible(tmp_store) -> None:
    from db.asof import enterprise_value_asof

    _seed_enterprise_value_inputs(tmp_store)

    assert refresh_enterprise_value(tmp_store, EnterpriseValueOptions(run_id="run-1")) == 1
    assert refresh_enterprise_value(tmp_store, EnterpriseValueOptions(run_id="run-2")) == 1

    stored = tmp_store.con.execute(
        """
        SELECT symbol, enterprise_value, available_at, run_id
        FROM enterprise_value
        """
    ).fetchall()
    assert stored == [("EVT", 1185.0, dt.datetime(2020, 2, 5, 10), "run-2")]

    early = enterprise_value_asof(
        dt.date(2020, 1, 2),
        as_of_ts=dt.datetime(2020, 2, 4, 23),
        store=tmp_store,
        symbols=("EVT",),
    )
    late = enterprise_value_asof(
        dt.date(2020, 1, 2),
        as_of_ts=dt.datetime(2020, 2, 5, 10),
        store=tmp_store,
        symbols=("EVT",),
    )
    assert early.empty
    assert late[["symbol", "enterprise_value"]].to_dict("records") == [
        {"symbol": "EVT", "enterprise_value": 1185.0}
    ]


def test_ev_falls_back_to_latest_available_period_across_filing_boundary(tmp_store) -> None:
    from db.asof import enterprise_value_asof

    security_id = "SEC-EV-FILING"
    symbol = "EVF"
    period_1_end = dt.date(2019, 12, 31)
    period_1_available = dt.datetime(2020, 2, 5, 10)
    period_2_end = dt.date(2020, 3, 31)
    period_2_available = dt.datetime(2020, 5, 10, 10)
    trade_date_early = dt.date(2020, 5, 2)
    trade_date_late = dt.date(2020, 5, 15)

    insert_frame(
        tmp_store,
        pd.DataFrame(
            [
                _market_cap_row(
                    security_id=security_id,
                    symbol=symbol,
                    trade_date=trade_date_early,
                    available_at=dt.datetime(2020, 5, 2, 22),
                ),
                _market_cap_row(
                    security_id=security_id,
                    symbol=symbol,
                    trade_date=trade_date_late,
                    available_at=dt.datetime(2020, 5, 15, 22),
                ),
            ]
        ),
        "market_cap",
        "enterprise_value_filing_boundary_market_cap_seed",
    )

    statement_rows: list[dict[str, object]] = []
    for metric, base_value in (
        ("total_debt", 200.0),
        ("pref_stock", 25.0),
        ("minority_int_bs", 10.0),
        ("cash_st_inv", 50.0),
    ):
        p1_row = _statement_row(
            metric,
            base_value,
            security_id=security_id,
            symbol=symbol,
            period_end=period_1_end,
            available_at=period_1_available,
        )
        p1_row["statement_point_id"] = f"{p1_row['statement_point_id']}-p1"
        p1_row["fact_revision_id"] = f"{p1_row['fact_revision_id']}-p1"
        p1_row["revision_group_id"] = f"{p1_row['revision_group_id']}-p1"
        statement_rows.append(p1_row)

        p2_row = _statement_row(
            metric,
            base_value + 5.0,
            security_id=security_id,
            symbol=symbol,
            period_end=period_2_end,
            available_at=period_2_available,
        )
        p2_row["statement_point_id"] = f"{p2_row['statement_point_id']}-p2"
        p2_row["fact_revision_id"] = f"{p2_row['fact_revision_id']}-p2"
        p2_row["revision_group_id"] = f"{p2_row['revision_group_id']}-p2"
        statement_rows.append(p2_row)

    insert_frame(
        tmp_store,
        pd.DataFrame(statement_rows),
        "fundamental_statement_points",
        "enterprise_value_filing_boundary_statement_seed",
    )

    assert refresh_enterprise_value(tmp_store, EnterpriseValueOptions(run_id="filing-boundary-run")) == 2

    early = enterprise_value_asof(trade_date_early, store=tmp_store, symbols=(symbol,))
    early_at_trade_date = early[early["trade_date"] == pd.Timestamp(trade_date_early)]
    assert not early_at_trade_date.empty, "EV must not have a coverage hole across the filing boundary"
    early_row = early_at_trade_date.iloc[0]
    assert pd.Timestamp(early_row["period_end"]) == pd.Timestamp(period_1_end)
    assert early_row["available_at"].date() <= trade_date_early

    late = enterprise_value_asof(trade_date_late, store=tmp_store, symbols=(symbol,))
    late_at_trade_date = late[late["trade_date"] == pd.Timestamp(trade_date_late)]
    assert not late_at_trade_date.empty
    late_row = late_at_trade_date.iloc[0]
    assert pd.Timestamp(late_row["period_end"]) == pd.Timestamp(period_2_end)


def test_enterprise_value_dataset_records_quality(tmp_store) -> None:
    _seed_enterprise_value_inputs(tmp_store)

    result = EnterpriseValueDataset().load(tmp_store, EnterpriseValueOptions(run_id="dataset-run"))

    assert result.rows_loaded == 1
    assert tmp_store.con.execute(
        """
        SELECT status, observed_value
        FROM data_quality_checks
        WHERE dataset_id = 'enterprise_value'
          AND check_name = 'rows_materialized'
        """
    ).fetchall() == [("passed", 1.0)]


def test_enterprise_value_migration_catalog_and_view_are_present(tmp_store) -> None:
    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM duckdb_columns()
            WHERE table_name = 'enterprise_value'
            """
        ).fetchall()
    }
    assert {
        "enterprise_value_id",
        "market_cap",
        "total_debt",
        "preferred_equity",
        "minority_interest",
        "cash_and_equivalents",
        "enterprise_value",
        "input_lineage_json",
    }.issubset(columns)
    assert (
        tmp_store.con.execute(
            "SELECT description FROM schema_migrations WHERE version = 145"
        ).fetchone()[0]
        == "pf3_s5_enterprise_value_schema_catalog"
    )
    assert (
        tmp_store.con.execute(
            """
            SELECT count(*)
            FROM information_schema.tables
            WHERE table_name = 'v_enterprise_value_latest'
              AND table_type = 'VIEW'
            """
        ).fetchone()[0]
        == 1
    )
    assert (
        tmp_store.con.execute(
            """
            SELECT count(*)
            FROM field_catalog
            WHERE table_name = 'enterprise_value'
              AND field_name IN (
                'market_cap',
                'total_debt',
                'preferred_equity',
                'minority_interest',
                'cash_and_equivalents',
                'enterprise_value',
                'input_lineage_json'
              )
            """
        ).fetchone()[0]
        == 7
    )


def test_valuation_input_completeness_gate_passes_populated_slice(tmp_store) -> None:
    from db.quality import run_warehouse_quality_checks

    _seed_valuation_coverage_inputs(tmp_store)

    results = {
        result.check_name: result
        for result in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=(
                "valuation_input_core_completeness",
                "valuation_core_item_stub_detector",
            ),
        )
    }

    assert results["valuation_input_core_completeness"].status == "passed"
    assert results["valuation_input_core_completeness"].observed_value == 0.0
    assert results["valuation_core_item_stub_detector"].status == "passed"
    assert results["valuation_core_item_stub_detector"].observed_value == 0.0


def test_valuation_input_completeness_gate_fails_when_component_stripped(tmp_store) -> None:
    from db.quality import run_warehouse_quality_checks

    _seed_valuation_coverage_inputs(tmp_store)
    tmp_store.con.execute("DELETE FROM enterprise_value")

    results = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        check_names=("valuation_input_core_completeness",),
    )

    assert len(results) == 1
    result = results[0]
    assert result.status == "failed"
    assert result.severity == "critical"
    assert result.observed_value == 1.0
    assert result.details["rows"] == [
        {
            "security_id": SECURITY_ID,
            "symbol": "EVT",
            "as_of_date": dt.date(2020, 1, 2),
            "decision_available_at": dt.datetime(2020, 3, 1),
            "canonical_input": "enterprise_value",
            "display_name": "Enterprise Value",
            "source_table": "enterprise_value",
        }
    ]


def test_valuation_core_item_stub_detector_fires_on_empty_surface(tmp_store) -> None:
    from db.quality import run_warehouse_quality_checks

    results = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        check_names=("valuation_core_item_stub_detector",),
    )

    assert len(results) == 1
    result = results[0]
    assert result.status == "failed"
    assert result.severity == "critical"
    assert result.observed_value == 9.0
    assert {row["canonical_input"] for row in result.details["rows"]} == {
        "cash_and_equivalents",
        "enterprise_value",
        "float",
        "market_cap",
        "minority_interest",
        "preferred_equity",
        "shares_diluted_avg",
        "total_debt",
        "treasury",
    }


def test_valuation_input_catalog_migration_registry_and_indexes_are_present(tmp_store) -> None:
    assert (
        tmp_store.con.execute(
            "SELECT description FROM schema_migrations WHERE version = 147"
        ).fetchone()[0]
        == "pf3_s5_valuation_input_coverage_catalog"
    )
    catalog_columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM duckdb_columns()
            WHERE table_name = 'valuation_input_catalog'
            """
        ).fetchall()
    }
    assert {
        "canonical_input",
        "display_name",
        "source_table",
        "source_field",
        "source_filter",
        "input_family",
        "unit",
        "sign_role",
        "is_core_input",
    }.issubset(catalog_columns)
    assert (
        tmp_store.con.execute(
            """
            SELECT count(*)
            FROM valuation_input_catalog
            WHERE is_core_input
            """
        ).fetchone()[0]
        == 9
    )
    assert (
        tmp_store.con.execute(
            """
            SELECT count(*)
            FROM information_schema.tables
            WHERE table_name = 'v_valuation_input_coverage'
              AND table_type = 'VIEW'
            """
        ).fetchone()[0]
        == 1
    )
    assert {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT index_name
            FROM duckdb_indexes()
            WHERE index_name IN (
                'idx_enterprise_value_key',
                'idx_enterprise_value_security_date',
                'idx_enterprise_value_asof',
                'idx_shares_outstanding_history_class',
                'idx_valuation_input_catalog_core'
            )
            """
        ).fetchall()
    } == {
        "idx_enterprise_value_key",
        "idx_enterprise_value_security_date",
        "idx_enterprise_value_asof",
        "idx_shares_outstanding_history_class",
        "idx_valuation_input_catalog_core",
    }
    registry_rows = tmp_store.con.execute(
        """
        SELECT check_name, dataset_id, table_name, severity, threshold_value, comparator, enabled
        FROM quality_check_registry
        WHERE check_name IN (
            'valuation_input_core_completeness',
            'valuation_core_item_stub_detector'
        )
        ORDER BY check_name
        """
    ).fetchall()
    assert registry_rows == [
        (
            "valuation_core_item_stub_detector",
            "valuation_input_coverage",
            "v_valuation_input_coverage",
            "critical",
            0.0,
            "eq",
            True,
        ),
        (
            "valuation_input_core_completeness",
            "valuation_input_coverage",
            "v_valuation_input_coverage",
            "critical",
            0.0,
            "eq",
            True,
        ),
    ]
