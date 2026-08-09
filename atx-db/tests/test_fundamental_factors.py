from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.factors.fundamental_families import (
    compute_fundamental_factor_rows,
    compute_signal_native_factor_rows,
    factor_seed_definitions,
    factor_seed_frame,
    fundamental_factor_family_coverage,
    fundamental_factor_lineage_completeness,
    load_factor_seed_specs,
)


def _metric(
    security_id: str,
    metric_code: str,
    value: float,
    available_at: str,
    *,
    symbol: str,
    as_of_date: dt.date = dt.date(2023, 12, 31),
) -> dict[str, object]:
    return {
        "security_id": security_id,
        "symbol": symbol,
        "metric_code": metric_code,
        "as_of_date": as_of_date,
        "value": value,
        "available_at": pd.Timestamp(available_at),
        "metric_row_id": f"{security_id}-{metric_code}",
        "formula_code": metric_code,
        "standardization_rule_id": f"std-{metric_code}",
        "vintage_class": "as_first_reported",
        "source_table": "fixture_metrics",
    }


def _fixture_metrics() -> pd.DataFrame:
    rows = []
    values = {
        "SEC-A": {
            "symbol": "AAA",
            "gross_profit": 400.0,
            "assets": 1000.0,
            "net_income": 120.0,
            "stockholders_equity": 500.0,
            "operating_cash_flow": 180.0,
            "book_to_market": 0.80,
            "earnings_yield": 0.10,
            "free_cash_flow_yield": 0.08,
        },
        "SEC-B": {
            "symbol": "BBB",
            "gross_profit": 240.0,
            "assets": 1200.0,
            "net_income": 90.0,
            "stockholders_equity": 450.0,
            "operating_cash_flow": 120.0,
            "book_to_market": 0.50,
            "earnings_yield": 0.06,
            "free_cash_flow_yield": 0.03,
        },
    }
    for security_id, metrics in values.items():
        symbol = str(metrics["symbol"])
        for metric_code, value in metrics.items():
            if metric_code == "symbol":
                continue
            rows.append(
                _metric(
                    security_id,
                    metric_code,
                    float(value),
                    "2024-02-01 10:00:00" if metric_code != "assets" else "2024-02-02 10:00:00",
                    symbol=symbol,
                )
            )
    return pd.DataFrame(rows)


def test_factor_seed_rows_load_as_s7_factor_definitions() -> None:
    specs = load_factor_seed_specs()
    definitions = factor_seed_definitions()
    frame = factor_seed_frame()

    assert {
        "fundamental_value",
        "fundamental_quality",
        "fundamental_profitability",
    } <= {spec.family for spec in specs}
    assert "profitability_gross_profitability" in set(frame["factor_id"])
    assert set(frame["stage"]) >= {"s8_0", "s8_1"}
    assert all(row.declared_in == "db/seeds/factor_definitions.csv" for row in definitions)
    assert frame.loc[frame["factor_id"] == "profitability_gross_profitability", "valid_from"].iloc[0] == dt.date(1900, 1, 1)


def test_core_academic_factors_emit_pit_rows_with_lineage_and_standardization() -> None:
    rows = compute_fundamental_factor_rows(_fixture_metrics(), run_id="s8-run")
    gross = rows[
        (rows["factor_id"] == "profitability_gross_profitability")
        & (rows["security_id"] == "SEC-A")
    ].iloc[0]
    quality = rows[(rows["factor_id"] == "quality_cash_earnings") & (rows["security_id"] == "SEC-A")].iloc[0]
    families = set(rows["family"])

    assert {"fundamental_value", "fundamental_quality", "fundamental_profitability"} <= families
    assert gross["raw_value"] == pytest.approx(0.4)
    assert gross["value"] == pytest.approx(0.70710678, abs=1e-6)
    assert gross["available_at"] == pd.Timestamp("2024-02-02 10:00:00")
    assert quality["raw_value"] == pytest.approx((180.0 - 120.0) / 1000.0)
    lineage = json.loads(gross["input_lineage_json"])
    assert {leg["metric_code"] for leg in lineage} == {"gross_profit", "assets"}
    assert all(leg["formula_code"] for leg in lineage)
    assert all(leg["standardization_rule_id"] for leg in lineage)
    assert all(leg["vintage_class"] == "as_first_reported" for leg in lineage)


def test_core_factor_requires_all_pit_inputs() -> None:
    missing_assets = _fixture_metrics()
    missing_assets = missing_assets[
        ~((missing_assets["security_id"] == "SEC-A") & (missing_assets["metric_code"] == "assets"))
    ]
    rows = compute_fundamental_factor_rows(missing_assets, families=("fundamental_profitability",))

    assert not (
        (rows["factor_id"] == "profitability_gross_profitability")
        & (rows["security_id"] == "SEC-A")
    ).any()


def test_core_factor_seed_rows_round_trip_into_catalog(tmp_store) -> None:
    seeded = tmp_store.con.execute(
        """
        SELECT family, standardization_spec_json, valid_from, valid_to
        FROM factor_definition
        WHERE factor_id = 'profitability_gross_profitability'
        """
    ).fetchone()
    metric_edges = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT dependency_name
            FROM factor_dependency_edges
            WHERE factor_id = 'profitability_gross_profitability'
              AND dependency_type = 'metric'
            """
        ).fetchall()
    }
    dataset_row = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM dataset_catalog
        WHERE dataset_id = 'fundamental_factor_families'
        """
    ).fetchone()[0]

    assert seeded[0] == "fundamental_profitability"
    assert json.loads(seeded[1]) == {"method": "zscore_cs"}
    assert seeded[2] == dt.date(1900, 1, 1)
    assert seeded[3] is None
    assert metric_edges == {"gross_profit", "assets"}
    assert dataset_row == 1


def _composite_metrics() -> pd.DataFrame:
    rows = []
    values = {
        "SEC-A": {
            "symbol": "AAA",
            "revenue_growth_yoy": 0.15,
            "revenue_cagr_3y": 0.12,
            "assets_growth_yoy": 0.05,
            "capital_expenditures": 80.0,
            "assets": 1000.0,
            "total_debt": 300.0,
            "stockholders_equity": 500.0,
            "net_income": 100.0,
            "operating_cash_flow": 150.0,
            "roa_yoy_change": 0.01,
            "debt_to_assets_change": -0.05,
            "current_ratio_change": 0.10,
            "shares_outstanding_growth": 0.0,
            "gross_margin_change": 0.02,
            "asset_turnover_change": 0.03,
            "working_capital": 200.0,
            "retained_earnings": 300.0,
            "ebit": 120.0,
            "market_cap": 600.0,
            "revenue": 1000.0,
            "delta_current_assets": 50.0,
            "delta_cash_and_equivalents": 10.0,
            "delta_current_liabilities": 20.0,
            "delta_short_term_debt": 5.0,
            "depreciation_expense": 15.0,
            "average_assets": 1000.0,
        },
        "SEC-B": {
            "symbol": "BBB",
            "revenue_growth_yoy": 0.02,
            "revenue_cagr_3y": 0.04,
            "assets_growth_yoy": 0.15,
            "capital_expenditures": 120.0,
            "assets": 1200.0,
            "total_debt": 600.0,
            "stockholders_equity": 400.0,
            "net_income": -20.0,
            "operating_cash_flow": -10.0,
            "roa_yoy_change": -0.02,
            "debt_to_assets_change": 0.04,
            "current_ratio_change": -0.10,
            "shares_outstanding_growth": 0.03,
            "gross_margin_change": -0.02,
            "asset_turnover_change": -0.01,
            "working_capital": 50.0,
            "retained_earnings": 80.0,
            "ebit": 40.0,
            "market_cap": 300.0,
            "revenue": 700.0,
            "delta_current_assets": 80.0,
            "delta_cash_and_equivalents": 5.0,
            "delta_current_liabilities": 20.0,
            "delta_short_term_debt": 20.0,
            "depreciation_expense": 10.0,
            "average_assets": 1100.0,
        },
    }
    for security_id, metrics in values.items():
        symbol = str(metrics["symbol"])
        for metric_code, value in metrics.items():
            if metric_code == "symbol":
                continue
            rows.append(_metric(security_id, metric_code, float(value), "2024-02-03 10:00:00", symbol=symbol))
    return pd.DataFrame(rows)


def test_growth_investment_leverage_and_named_composites_reconcile_to_references() -> None:
    rows = compute_fundamental_factor_rows(_composite_metrics(), run_id="s8-composite-run")
    lookup = {
        (row.factor_id, row.security_id): row
        for row in rows.itertuples(index=False)
    }

    assert lookup[("growth_revenue_yoy", "SEC-A")].raw_value == pytest.approx(0.15)
    assert lookup[("investment_capex_to_assets", "SEC-A")].raw_value == pytest.approx(0.08)
    assert lookup[("leverage_debt_to_assets", "SEC-A")].raw_value == pytest.approx(0.30)
    assert lookup[("quality_piotroski_f_score", "SEC-A")].raw_value == pytest.approx(9.0)
    assert lookup[("quality_piotroski_f_score", "SEC-B")].raw_value == pytest.approx(1.0)
    assert lookup[("distress_altman_z_score", "SEC-A")].raw_value == pytest.approx(3.256)
    assert lookup[("accruals_sloan_working_capital", "SEC-A")].raw_value == pytest.approx(0.01)
    assert json.loads(lookup[("quality_piotroski_f_score", "SEC-A")].input_lineage_json)[0]["metric_code"]


def test_composite_with_missing_leg_is_withheld() -> None:
    metrics = _composite_metrics()
    metrics = metrics[
        ~((metrics["security_id"] == "SEC-A") & (metrics["metric_code"] == "retained_earnings"))
    ]
    rows = compute_fundamental_factor_rows(metrics)

    assert not (
        (rows["factor_id"] == "distress_altman_z_score")
        & (rows["security_id"] == "SEC-A")
    ).any()


def test_composite_seed_rows_round_trip_into_catalog(tmp_store) -> None:
    composite = tmp_store.con.execute(
        """
        SELECT family, direction, standardization_spec_json
        FROM factor_definition
        WHERE factor_id = 'quality_piotroski_f_score'
        """
    ).fetchone()
    edges = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT dependency_name
            FROM factor_dependency_edges
            WHERE factor_id = 'quality_piotroski_f_score'
              AND dependency_type = 'metric'
            """
        ).fetchall()
    }
    dataset_row = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM dataset_catalog
        WHERE dataset_id = 'fundamental_composite_factors'
        """
    ).fetchone()[0]

    assert composite[0:2] == ("fundamental_quality", 1)
    standardization = json.loads(composite[2])
    assert standardization["method"] == "zscore_cs"
    assert standardization["complete_case"] is True
    assert {
        "net_income",
        "operating_cash_flow",
        "total_assets",
        "gross_profit",
        "revenue",
        "lt_debt",
    } <= edges
    assert dataset_row == 1


def test_signal_native_factors_emit_and_revisions_ignore_future_vintages() -> None:
    as_of = dt.date(2024, 3, 1)
    revisions = pd.DataFrame(
        [
            {"security_id": "SEC-A", "symbol": "AAA", "metric_code": "net_income", "as_of_date": as_of, "value": 100.0, "available_at": pd.Timestamp("2024-01-15"), "vintage_class": "as_first_reported"},
            {"security_id": "SEC-A", "symbol": "AAA", "metric_code": "net_income", "as_of_date": as_of, "value": 110.0, "available_at": pd.Timestamp("2024-02-15"), "vintage_class": "amended"},
            {"security_id": "SEC-A", "symbol": "AAA", "metric_code": "net_income", "as_of_date": as_of, "value": 200.0, "available_at": pd.Timestamp("2024-04-01"), "vintage_class": "future_restatement"},
            {"security_id": "SEC-B", "symbol": "BBB", "metric_code": "net_income", "as_of_date": as_of, "value": 100.0, "available_at": pd.Timestamp("2024-01-15"), "vintage_class": "as_first_reported"},
            {"security_id": "SEC-B", "symbol": "BBB", "metric_code": "net_income", "as_of_date": as_of, "value": 90.0, "available_at": pd.Timestamp("2024-02-15"), "vintage_class": "amended"},
        ]
    )
    standardization = pd.DataFrame(
        [
            {"security_id": "SEC-A", "symbol": "AAA", "as_of_date": as_of, "raw_value": 100.0, "standardized_value": 90.0, "available_at": pd.Timestamp("2024-02-01"), "standardization_rule_id": "std-a"},
            {"security_id": "SEC-B", "symbol": "BBB", "as_of_date": as_of, "raw_value": 100.0, "standardized_value": 110.0, "available_at": pd.Timestamp("2024-02-01"), "standardization_rule_id": "std-b"},
        ]
    )
    segments = pd.DataFrame(
        [
            {"security_id": "SEC-A", "symbol": "AAA", "as_of_date": as_of, "segment": "cloud", "segment_revenue": 70.0, "available_at": pd.Timestamp("2024-02-01")},
            {"security_id": "SEC-A", "symbol": "AAA", "as_of_date": as_of, "segment": "devices", "segment_revenue": 30.0, "available_at": pd.Timestamp("2024-02-01")},
            {"security_id": "SEC-B", "symbol": "BBB", "as_of_date": as_of, "segment": "cloud", "segment_revenue": 50.0, "available_at": pd.Timestamp("2024-02-01")},
            {"security_id": "SEC-B", "symbol": "BBB", "as_of_date": as_of, "segment": "devices", "segment_revenue": 50.0, "available_at": pd.Timestamp("2024-02-01")},
        ]
    )
    footnotes = pd.DataFrame(
        [
            {"security_id": "SEC-A", "symbol": "AAA", "as_of_date": as_of, "footnote_count": 12, "prior_footnote_count": 10, "available_at": pd.Timestamp("2024-02-01")},
            {"security_id": "SEC-B", "symbol": "BBB", "as_of_date": as_of, "footnote_count": 9, "prior_footnote_count": 10, "available_at": pd.Timestamp("2024-02-01")},
        ]
    )

    rows = compute_signal_native_factor_rows(
        revisions=revisions,
        standardization_deltas=standardization,
        segments=segments,
        footnotes=footnotes,
        run_id="s8-native-run",
    )
    lookup = {(row.factor_id, row.security_id): row for row in rows.itertuples(index=False)}

    assert set(rows["factor_id"]) == {
        "signal_revision_momentum",
        "signal_standardization_delta",
        "signal_segment_revenue_concentration",
        "signal_footnote_disclosure_change",
    }
    assert lookup[("signal_revision_momentum", "SEC-A")].raw_value == pytest.approx(0.10)
    assert "future_restatement" not in json.dumps(json.loads(lookup[("signal_revision_momentum", "SEC-A")].input_lineage_json))
    assert lookup[("signal_standardization_delta", "SEC-A")].raw_value == pytest.approx(-0.10)
    assert lookup[("signal_segment_revenue_concentration", "SEC-A")].raw_value == pytest.approx(0.58)
    assert lookup[("signal_footnote_disclosure_change", "SEC-A")].raw_value == pytest.approx(0.20)


def test_signal_native_seed_rows_round_trip_into_catalog(tmp_store) -> None:
    signal_rows = {
        row[0]: row[1]
        for row in tmp_store.con.execute(
            """
            SELECT factor_id, family
            FROM factor_definition
            WHERE factor_id LIKE 'signal_%'
            """
        ).fetchall()
    }
    dataset_row = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM dataset_catalog
        WHERE dataset_id = 'signal_native_factors'
        """
    ).fetchone()[0]

    assert signal_rows == {
        "signal_revision_momentum": "signal_native",
        "signal_standardization_delta": "signal_native",
        "signal_segment_revenue_concentration": "signal_native",
        "signal_footnote_disclosure_change": "signal_native",
    }
    assert dataset_row == 1


def test_factor_lineage_and_coverage_gates_flag_planted_failures() -> None:
    academic = compute_fundamental_factor_rows(_fixture_metrics(), run_id="gate-run")
    native = compute_signal_native_factor_rows(
        revisions=pd.DataFrame(
            [
                {"security_id": "SEC-A", "symbol": "AAA", "metric_code": "net_income", "as_of_date": dt.date(2024, 3, 1), "value": 100.0, "available_at": pd.Timestamp("2024-01-15")},
                {"security_id": "SEC-A", "symbol": "AAA", "metric_code": "net_income", "as_of_date": dt.date(2024, 3, 1), "value": 110.0, "available_at": pd.Timestamp("2024-02-15")},
                {"security_id": "SEC-B", "symbol": "BBB", "metric_code": "net_income", "as_of_date": dt.date(2024, 3, 1), "value": 100.0, "available_at": pd.Timestamp("2024-01-15")},
                {"security_id": "SEC-B", "symbol": "BBB", "metric_code": "net_income", "as_of_date": dt.date(2024, 3, 1), "value": 95.0, "available_at": pd.Timestamp("2024-02-15")},
            ]
        ),
        run_id="gate-run",
    )
    panel = pd.concat([academic, native], ignore_index=True)

    clean_lineage = fundamental_factor_lineage_completeness(panel)
    broken = panel.copy()
    broken.loc[0, "input_lineage_json"] = ""
    failed_lineage = fundamental_factor_lineage_completeness(broken)
    clean_coverage = fundamental_factor_family_coverage(
        panel,
        expected_families=(
            "fundamental_value",
            "fundamental_quality",
            "fundamental_profitability",
            "signal_native",
        ),
    )
    failed_coverage = fundamental_factor_family_coverage(
        panel[panel["family"] != "signal_native"],
        expected_families=(
            "fundamental_value",
            "fundamental_quality",
            "fundamental_profitability",
            "signal_native",
        ),
    )

    assert clean_lineage["status"] == "passed"
    assert failed_lineage["status"] == "failed"
    assert failed_lineage["incomplete_count"] == 1
    assert clean_coverage["status"] == "passed"
    assert failed_coverage["status"] == "failed"
    assert failed_coverage["missing_families"] == ["signal_native"]


def test_factor_family_panel_catalog_and_gate_registry_are_present(tmp_store) -> None:
    tables = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM table_catalog
            WHERE table_name IN ('fundamental_factor_values', 'v_fundamental_factor_family_catalog')
            """
        ).fetchall()
    }
    registry = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT check_name
            FROM quality_check_registry
            WHERE source = 'pf3_s8'
            """
        ).fetchall()
    }
    catalog_row = tmp_store.con.execute(
        """
        SELECT family, dependency_count
        FROM v_fundamental_factor_family_catalog
        WHERE factor_id = 'profitability_gross_profitability'
        """
    ).fetchone()

    assert tables == {"fundamental_factor_values", "v_fundamental_factor_family_catalog"}
    assert {
        "fundamental_factor_lineage_completeness",
        "fundamental_factor_family_coverage",
    } <= registry
    assert catalog_row == ("fundamental_profitability", 2)
