from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from db.metric_engine import (
    FundamentalGrowthOptions,
    GrowthFormulaSpec,
    compute_growth_rows,
    load_growth_formula_specs,
    refresh_fundamental_growth,
)
from db.warehouse import insert_frame


def _ts(value: str) -> pd.Timestamp:
    return pd.Timestamp(value)


def _metric_row(
    metric_code: str,
    period_end: dt.date,
    value: float,
    available_at: str,
    *,
    basis: str = "ttm",
    source_metric_id: str | None = None,
) -> dict[str, object]:
    return {
        "source_metric_id": source_metric_id or f"{metric_code}-{period_end.isoformat()}",
        "input_table": "fundamental_ttm_points" if basis == "ttm" else "fundamental_statement_points",
        "upstream_source": "fixture",
        "security_id": "SEC-A",
        "symbol": "AAA",
        "cik": "0000000001",
        "metric_code": metric_code,
        "basis": basis,
        "period_start": period_end - dt.timedelta(days=364),
        "period_end": period_end,
        "fiscal_year": period_end.year,
        "fiscal_period": "FY" if period_end.month == 12 else "Q3",
        "value": value,
        "as_of_date": period_end,
        "available_at": _ts(available_at),
        "source_accession": f"acc-{metric_code}-{period_end:%Y%m%d}",
        "filed_date": period_end + dt.timedelta(days=30),
        "vintage_class": "most_recently_restated",
        "is_latest_revision": True,
    }


def _spec(
    formula_code: str,
    method: str,
    horizon_years: float,
    *,
    transform: str = "pct_change",
) -> GrowthFormulaSpec:
    return GrowthFormulaSpec(
        formula_code=formula_code,
        family="growth_cagr" if formula_code != "revenue_growth_yoy" else "growth",
        kind="growth",
        unit="ratio",
        metric_code="revenue",
        basis="ttm",
        growth_method=method,
        horizon_years=horizon_years,
        transform=transform,
        require_positive_base=True,
    )


def _history() -> pd.DataFrame:
    return pd.DataFrame(
        [
            _metric_row("revenue", dt.date(2020, 12, 31), 100.0, "2021-02-01"),
            _metric_row("revenue", dt.date(2021, 12, 31), 110.0, "2022-02-01"),
            _metric_row("revenue", dt.date(2022, 12, 31), 121.0, "2023-02-01"),
            _metric_row("revenue", dt.date(2023, 9, 30), 121.0, "2023-11-01"),
            _metric_row("revenue", dt.date(2023, 12, 31), 133.1, "2024-02-01"),
        ]
    )


def _by_code_for_period(frame: pd.DataFrame, period_end: dt.date) -> dict[str, pd.Series]:
    selected = frame[frame["period_end"] == period_end]
    return {row.formula_code: row for row in selected.itertuples(index=False)}


def test_compute_growth_rows_emits_yoy_qoq_cagr_and_stability_lineage() -> None:
    specs = (
        _spec("revenue_growth_yoy", "yoy", 1.0),
        _spec("revenue_growth_qoq", "qoq", 0.25),
        _spec("revenue_cagr_3y", "cagr", 3.0, transform="cagr"),
        _spec("revenue_growth_stability_3y", "stability", 3.0, transform="stability"),
        _spec("revenue_growth_consistency_3y", "consistency", 3.0, transform="consistency"),
    )

    out = compute_growth_rows(_history(), specs=specs, source="test-growth", run_id="run-1")
    rows = _by_code_for_period(out, dt.date(2023, 12, 31))

    assert rows["revenue_growth_yoy"].value == pytest.approx(0.10)
    assert rows["revenue_growth_qoq"].value == pytest.approx(0.10)
    assert rows["revenue_cagr_3y"].value == pytest.approx(0.10, abs=1e-4)
    assert rows["revenue_growth_stability_3y"].value == pytest.approx(1.0)
    assert rows["revenue_growth_consistency_3y"].value == pytest.approx(1.0)
    assert rows["revenue_cagr_3y"].base_period_end == dt.date(2020, 12, 31)
    assert rows["revenue_cagr_3y"].available_at == _ts("2024-02-01")

    lineage = json.loads(rows["revenue_growth_stability_3y"].input_lineage_json)
    assert len(lineage["observations"]) == 3
    assert lineage["observations"][-1]["current"]["period_end"] == "2023-12-31"
    assert lineage["observations"][-1]["base"]["period_end"] == "2022-12-31"


def test_compute_growth_rows_drops_missing_base_period() -> None:
    specs = (_spec("revenue_growth_yoy", "yoy", 1.0),)
    history = pd.DataFrame(
        [
            _metric_row("revenue", dt.date(2020, 12, 31), 100.0, "2021-02-01"),
            _metric_row("revenue", dt.date(2022, 12, 31), 121.0, "2023-02-01"),
        ]
    )

    out = compute_growth_rows(history, specs=specs)

    assert out.empty


def test_compute_growth_rows_handles_zero_and_negative_bases() -> None:
    yoy_spec = (_spec("revenue_growth_yoy", "yoy", 1.0),)
    cagr_spec = (_spec("revenue_cagr_3y", "cagr", 3.0, transform="cagr"),)
    negative_history = pd.DataFrame(
        [
            _metric_row("revenue", dt.date(2022, 12, 31), -5.0, "2023-02-01"),
            _metric_row("revenue", dt.date(2023, 12, 31), 10.0, "2024-02-01"),
        ]
    )
    zero_history = pd.DataFrame(
        [
            _metric_row("revenue", dt.date(2022, 12, 31), 0.0, "2023-02-01"),
            _metric_row("revenue", dt.date(2023, 12, 31), 10.0, "2024-02-01"),
        ]
    )
    cagr_negative_history = pd.DataFrame(
        [
            _metric_row("revenue", dt.date(2020, 12, 31), -5.0, "2021-02-01"),
            _metric_row("revenue", dt.date(2023, 12, 31), 10.0, "2024-02-01"),
        ]
    )

    negative = compute_growth_rows(negative_history, specs=yoy_spec)

    assert negative.iloc[0]["value"] == pytest.approx(3.0)
    assert bool(negative.iloc[0]["is_meaningful"]) is False
    assert compute_growth_rows(zero_history, specs=yoy_spec).empty
    assert compute_growth_rows(cagr_negative_history, specs=cagr_spec).empty


def test_registry_growth_specs_do_not_pollute_legacy_ratio_defs() -> None:
    from db.fundamental_ratios import RATIO_DEFS

    ratio_codes = {definition.code for definition in RATIO_DEFS}
    spec_codes = {spec.formula_code for spec in load_growth_formula_specs()}

    assert "revenue_growth_yoy" in ratio_codes
    assert "revenue_growth_yoy" in spec_codes
    assert "revenue_cagr_3y" in spec_codes
    assert "revenue_growth_stability_3y" in spec_codes
    assert "revenue_cagr_3y" not in ratio_codes
    assert "revenue_growth_stability_3y" not in ratio_codes


def _ttm_row(metric: str, period_end: dt.date, value: float, available_at: dt.datetime) -> dict[str, object]:
    period_start = period_end - dt.timedelta(days=364)
    return {
        "ttm_point_id": f"ttm-{metric}-{period_end:%Y%m%d}",
        "ttm_revision_group_id": f"group-{metric}-{period_end:%Y%m%d}",
        "anchor_statement_point_id": f"anchor-{metric}-{period_end:%Y%m%d}",
        "source": "fixture",
        "security_id": "SEC-A",
        "symbol": "AAA",
        "cik": "0000000001",
        "statement_type": "income",
        "statement_section": "income_statement",
        "canonical_metric": metric,
        "canonical_label": metric.replace("_", " ").title(),
        "unit": "USD",
        "unit_type": "currency",
        "ttm_start_date": period_start,
        "ttm_end_date": period_end,
        "as_of_date": period_end,
        "available_at": available_at,
        "fiscal_year": period_end.year,
        "fiscal_period": "FY" if period_end.month == 12 else "Q3",
        "form": "10-K",
        "accession_number": f"acc-{metric}-{period_end:%Y%m%d}",
        "quarter_count": 4,
        "coverage_days": 365,
        "min_input_available_at": available_at,
        "max_input_available_at": available_at,
        "input_statement_point_ids_json": "[]",
        "input_accessions_json": "[]",
        "input_period_ends_json": "[]",
        "ttm_value": value,
        "previous_ttm_value": None,
        "ttm_value_delta": None,
        "ttm_value_delta_percent": None,
        "revision_sequence": 1,
        "revision_count": 1,
        "is_latest_revision": True,
        "is_value_changed": True,
        "calculation_method": "fixture",
    }


def _seed_revenue_ttm(tmp_store) -> None:
    rows = pd.DataFrame(
        [
            _ttm_row("revenue", dt.date(2020, 12, 31), 100.0, dt.datetime(2021, 2, 1, 10)),
            _ttm_row("revenue", dt.date(2021, 12, 31), 110.0, dt.datetime(2022, 2, 1, 10)),
            _ttm_row("revenue", dt.date(2022, 12, 31), 121.0, dt.datetime(2023, 2, 1, 10)),
            _ttm_row("revenue", dt.date(2023, 9, 30), 121.0, dt.datetime(2023, 11, 1, 10)),
            _ttm_row("revenue", dt.date(2023, 12, 31), 133.1, dt.datetime(2024, 2, 1, 10)),
        ]
    )
    insert_frame(tmp_store, rows, "fundamental_ttm_points", "fundamental_growth_ttm_seed")


def test_refresh_fundamental_growth_materializes_pit_visible_rows(tmp_store) -> None:
    _seed_revenue_ttm(tmp_store)

    loaded = refresh_fundamental_growth(
        tmp_store,
        FundamentalGrowthOptions(metrics=("revenue",), run_id="growth-run"),
    )
    early = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM fundamental_growth
        WHERE formula_code = 'revenue_cagr_3y'
          AND available_at <= TIMESTAMP '2023-12-31 23:59:59'
        """
    ).fetchone()[0]
    late = tmp_store.con.execute(
        """
        SELECT value, base_period_end, current_value, base_value
        FROM fundamental_growth
        WHERE formula_code = 'revenue_cagr_3y'
          AND available_at <= TIMESTAMP '2024-02-02 00:00:00'
        """
    ).fetchone()

    assert loaded > 0
    assert early == 0
    assert late[0] == pytest.approx(0.10, abs=1e-4)
    assert late[1:] == (dt.date(2020, 12, 31), 133.1, 100.0)


def test_fundamental_growth_migration_catalog_and_formula_seed_are_present(tmp_store) -> None:
    columns = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT column_name FROM duckdb_columns() WHERE table_name = 'fundamental_growth'"
        ).fetchall()
    }
    field_count = tmp_store.con.execute(
        "SELECT count(*) FROM field_catalog WHERE table_name = 'fundamental_growth'"
    ).fetchone()[0]
    family_count = tmp_store.con.execute(
        "SELECT count(*) FROM formula_registry WHERE family = 'growth_cagr'"
    ).fetchone()[0]
    transform = tmp_store.con.execute(
        "SELECT transform FROM formula_registry WHERE formula_code = 'revenue_cagr_3y'"
    ).fetchone()[0]

    assert {
        "growth_id",
        "formula_code",
        "growth_method",
        "horizon_years",
        "current_available_at",
        "base_available_at",
        "input_lineage_json",
    } <= columns
    assert tmp_store.con.execute(
        "SELECT count(*) FROM dataset_catalog WHERE dataset_id = 'fundamental_growth'"
    ).fetchone()[0] == 1
    assert tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'fundamental_growth'"
    ).fetchone()[0] == 1
    assert field_count == len(columns)
    assert family_count == 35
    assert transform == "cagr"
