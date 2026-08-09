from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

import atx_db.metric_engine as me_mod
from atx_db.metric_engine import (
    FundamentalGrowthOptions,
    GrowthFormulaSpec,
    compute_growth_rows,
    load_growth_formula_specs,
    metric_lineage_asof,
    metric_lineage_completeness,
    reconcile_metric_frames,
    refresh_fundamental_growth,
)
from atx_db.warehouse import insert_frame


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
    from atx_db.fundamental_ratios import RATIO_DEFS

    ratio_codes = {definition.code for definition in RATIO_DEFS}
    spec_codes = {spec.formula_code for spec in load_growth_formula_specs()}

    assert "revenue_growth_yoy" in ratio_codes
    assert "revenue_growth_yoy" in spec_codes
    assert "revenue_cagr_3y" in spec_codes
    assert "revenue_growth_stability_3y" in spec_codes
    assert "revenue_cagr_3y" not in ratio_codes
    assert "revenue_growth_stability_3y" not in ratio_codes


def test_reconcile_metric_frames_detects_planted_perturbation() -> None:
    expected = pd.DataFrame(
        [
            {
                "security_id": "SEC-A",
                "ratio_code": "net_profit_margin",
                "basis": "ttm",
                "period_end": dt.date(2023, 12, 31),
                "value": 0.25,
            }
        ]
    )
    actual = expected.copy()

    clean = reconcile_metric_frames(
        expected,
        actual,
        key_columns=("security_id", "ratio_code", "basis", "period_end"),
        tolerance=1e-9,
    )
    actual.loc[0, "value"] = 0.251
    perturbed = reconcile_metric_frames(
        expected,
        actual,
        key_columns=("security_id", "ratio_code", "basis", "period_end"),
        tolerance=1e-9,
    )

    assert clean.status == "passed"
    assert perturbed.status == "failed"
    assert perturbed.mismatched_count == 1
    assert perturbed.max_abs_delta == pytest.approx(0.001)


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

    early_lineage = metric_lineage_asof(
        dt.date(2024, 1, 31),
        store=tmp_store,
        metric_codes=("revenue_cagr_3y",),
        source_tables=("fundamental_growth",),
    )
    late_lineage = metric_lineage_asof(
        dt.date(2024, 2, 2),
        store=tmp_store,
        metric_codes=("revenue_cagr_3y",),
        source_tables=("fundamental_growth",),
    )

    assert early_lineage.empty
    assert len(late_lineage) == 1
    lineage = late_lineage.iloc[0]
    assert lineage["formula_expression"] == "metric:revenue|mode:cagr|horizon_years:3|basis:ttm"
    assert pd.Timestamp(lineage["formula_valid_from"]).date() == dt.date(1900, 1, 1)
    assert json.loads(lineage["input_lineage_json"])["current"]["period_end"] == "2023-12-31"


def test_metric_lineage_completeness_flags_planted_orphan(tmp_store) -> None:
    _seed_revenue_ttm(tmp_store)
    refresh_fundamental_growth(
        tmp_store,
        FundamentalGrowthOptions(metrics=("revenue",), run_id="growth-run"),
    )

    clean = metric_lineage_completeness(tmp_store, source_tables=("fundamental_growth",))
    orphan = tmp_store.con.execute("SELECT * FROM fundamental_growth LIMIT 1").df()
    orphan.loc[0, "growth_id"] = "orphan-growth-row"
    orphan.loc[0, "formula_code"] = "orphan_metric"
    insert_frame(tmp_store, orphan, "fundamental_growth", "fundamental_growth_orphan")
    failed = metric_lineage_completeness(tmp_store, source_tables=("fundamental_growth",))

    assert clean["status"] == "passed"
    assert clean["incomplete_count"] == 0
    assert failed["status"] == "failed"
    assert failed["incomplete_count"] == 1
    assert failed["examples"][0]["metric_code"] == "orphan_metric"


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
    lineage_field_count = tmp_store.con.execute(
        "SELECT count(*) FROM field_catalog WHERE table_name = 'v_metric_lineage'"
    ).fetchone()[0]
    metric_catalog_row = tmp_store.con.execute(
        """
        SELECT family, unit, sign_convention, definition
        FROM v_metric_catalog
        WHERE metric_code = 'revenue_cagr_3y'
        """
    ).fetchone()
    gate_checks = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT check_name
            FROM quality_check_registry
            WHERE source = 'pf3_s6'
            """
        ).fetchall()
    }

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
    assert tmp_store.con.execute(
        "SELECT count(*) FROM duckdb_views() WHERE view_name = 'v_metric_lineage'"
    ).fetchone()[0] == 1
    assert tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'v_metric_lineage'"
    ).fetchone()[0] == 1
    assert lineage_field_count > 0
    assert tmp_store.con.execute(
        "SELECT count(*) FROM duckdb_views() WHERE view_name = 'v_metric_catalog'"
    ).fetchone()[0] == 1
    assert tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'v_metric_catalog'"
    ).fetchone()[0] == 1
    assert metric_catalog_row[0:3] == ("growth_cagr", "ratio", "signed")
    assert "Three-year compound annual growth rate" in metric_catalog_row[3]
    assert {
        "metric_lineage_completeness",
        "fundamental_ratio_reconciliation",
    } <= gate_checks


def _reference_compute_growth_rows(
    metric_history: pd.DataFrame,
    *,
    specs: tuple[GrowthFormulaSpec, ...] | None = None,
    source: str = me_mod.DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """PF4-S3 S3-6: frozen pre-vectorization reference.

    Verbatim copy of the nested ``for key, group ... for _, current ... for spec:
    _base_row(group, current, spec)`` body that ``compute_growth_rows`` used before
    S3-6. Kept independent of the production function (whose base-pairing is now
    O(M) instead of O(M^2)) so the new implementation can be checked for
    byte-for-byte equivalence against the original per-current-row rescan algorithm,
    not just against itself. Reuses every other helper (`_base_row`, `_pair_record`,
    `_stability_record`, ...) unchanged from the module.
    """
    history = me_mod._normalize_metric_history(metric_history)
    if history.empty:
        return pd.DataFrame(columns=me_mod.FUNDAMENTAL_GROWTH_COLUMNS)
    resolved_specs = specs or me_mod.load_growth_formula_specs()
    specs_by_key: dict[tuple[str, str], list[GrowthFormulaSpec]] = {}
    for spec in resolved_specs:
        specs_by_key.setdefault((spec.metric_code, spec.basis), []).append(spec)

    records: list[dict[str, object]] = []
    for key, group in history.groupby(["metric_code", "basis", "security_id"], sort=False):
        metric_code, basis, _security_id = key
        group_specs = specs_by_key.get((str(metric_code), str(basis)), [])
        if not group_specs:
            continue
        group = group.sort_values(["period_end_ts", "available_at", "source_metric_id"], kind="mergesort")
        for _, current in group.iterrows():
            current_value = me_mod._safe_float(current.get("value"))
            if current_value is None:
                continue
            for spec in group_specs:
                if spec.growth_method in {"stability", "consistency"}:
                    record = me_mod._stability_record(spec, group, current, source=source, run_id=run_id)
                    if record is not None:
                        records.append(record)
                    continue
                base = me_mod._base_row(group, current, spec)
                if base is None:
                    continue
                base_value = me_mod._safe_float(base.get("value"))
                if base_value is None:
                    continue
                years = me_mod._elapsed_years(current, base)
                if spec.growth_method in {"yoy", "qoq"}:
                    value = me_mod._pct_change_value(current_value, base_value)
                    if value is None:
                        continue
                    is_meaningful = (not spec.require_positive_base) or base_value > 0
                elif spec.growth_method == "cagr":
                    value = me_mod._cagr_value(current_value, base_value, years)
                    if value is None:
                        continue
                    is_meaningful = True
                else:
                    raise ValueError(f"Unsupported growth method {spec.growth_method!r}")
                records.append(
                    me_mod._pair_record(
                        spec,
                        current,
                        base,
                        source=source,
                        run_id=run_id,
                        value=value,
                        is_meaningful=is_meaningful,
                        elapsed_years=years,
                    )
                )

    if not records:
        return pd.DataFrame(columns=me_mod.FUNDAMENTAL_GROWTH_COLUMNS)
    return pd.DataFrame(records, columns=me_mod.FUNDAMENTAL_GROWTH_COLUMNS)


def _regularly_spaced_growth_history(n_periods: int, *, start: dt.date = dt.date(1990, 1, 1)) -> pd.DataFrame:
    # 91-day (quarterly) spacing lines up cleanly with the qoq (91d/25d), yoy
    # (365.25d/35d) and 3y-cagr (1095.75d/54d) gap/tolerance windows used below, so
    # each `current` row has a small, bounded set of in-tolerance base candidates
    # regardless of how large `n_periods` gets.
    rows = []
    for i in range(n_periods):
        period_end = start + dt.timedelta(days=91 * i)
        available_at = dt.datetime.combine(period_end + dt.timedelta(days=20), dt.time(10, 0))
        rows.append(_metric_row("revenue", period_end, 100.0 + i, available_at.isoformat(sep=" ")))
    return pd.DataFrame(rows)


def _growth_history_with_revisions(n_periods: int, *, start: dt.date = dt.date(1995, 1, 1)) -> pd.DataFrame:
    """Same cadence as `_regularly_spaced_growth_history`, but every 7th period also
    carries a stale revision (earlier available_at, distinct source_metric_id) ahead
    of the latest one -- exercises the available_at/source_metric_id tie-break that
    `_base_row` resolves via its multi-key sort.
    """
    rows = []
    for i in range(n_periods):
        period_end = start + dt.timedelta(days=91 * i)
        base_available_at = dt.datetime.combine(period_end + dt.timedelta(days=20), dt.time(10, 0))
        if i > 0 and i % 7 == 0:
            rows.append(
                _metric_row(
                    "revenue",
                    period_end,
                    100.0 + i - 0.5,
                    base_available_at.isoformat(sep=" "),
                    source_metric_id=f"revenue-{period_end.isoformat()}-rev1",
                )
            )
            rows.append(
                _metric_row(
                    "revenue",
                    period_end,
                    100.0 + i,
                    (base_available_at + dt.timedelta(days=5)).isoformat(sep=" "),
                    source_metric_id=f"revenue-{period_end.isoformat()}-rev2",
                )
            )
        else:
            rows.append(_metric_row("revenue", period_end, 100.0 + i, base_available_at.isoformat(sep=" ")))
    return pd.DataFrame(rows)


_SCALE_GROWTH_SPECS = (
    GrowthFormulaSpec(
        formula_code="revenue_growth_yoy",
        family="growth",
        kind="growth",
        unit="ratio",
        metric_code="revenue",
        basis="ttm",
        growth_method="yoy",
        horizon_years=1.0,
        transform="pct_change",
        require_positive_base=True,
    ),
    GrowthFormulaSpec(
        formula_code="revenue_growth_qoq",
        family="growth",
        kind="growth",
        unit="ratio",
        metric_code="revenue",
        basis="ttm",
        growth_method="qoq",
        horizon_years=0.25,
        transform="pct_change",
        require_positive_base=True,
    ),
    GrowthFormulaSpec(
        formula_code="revenue_cagr_3y",
        family="growth_cagr",
        kind="growth",
        unit="ratio",
        metric_code="revenue",
        basis="ttm",
        growth_method="cagr",
        horizon_years=3.0,
        transform="cagr",
        require_positive_base=True,
    ),
)


def test_growth_base_pairing_is_linear(monkeypatch) -> None:
    # Equivalence, small fixture (existing 5-row history/spec mix incl. stability).
    small_specs = (
        _spec("revenue_growth_yoy", "yoy", 1.0),
        _spec("revenue_growth_qoq", "qoq", 0.25),
        _spec("revenue_cagr_3y", "cagr", 3.0, transform="cagr"),
        _spec("revenue_growth_stability_3y", "stability", 3.0, transform="stability"),
        _spec("revenue_growth_consistency_3y", "consistency", 3.0, transform="consistency"),
    )
    small_history = _history()
    reference_small = _reference_compute_growth_rows(small_history, specs=small_specs, source="test-growth", run_id="run-1")

    # Equivalence, medium fixture with same-period revisions (tie-break coverage).
    medium_history = _growth_history_with_revisions(28)
    reference_medium = _reference_compute_growth_rows(
        medium_history, specs=_SCALE_GROWTH_SPECS, source="test-growth-medium", run_id="run-medium"
    )

    # Complexity, scale fixture: M=600 periods, one security, no stability/consistency
    # specs (those still use unmodified `_base_row` via `_stability_observations` and
    # are out of scope for this fix) -> isolates the flagged main-loop pairing.
    M = 600
    scale_history = _regularly_spaced_growth_history(M)

    base_row_scan_totals = {"group_rows_scanned": 0, "calls": 0}
    original_base_row = me_mod._base_row

    def counting_base_row(group, current, spec):
        base_row_scan_totals["group_rows_scanned"] += len(group)
        base_row_scan_totals["calls"] += 1
        return original_base_row(group, current, spec)

    monkeypatch.setattr(me_mod, "_base_row", counting_base_row)

    vectorized_scale = compute_growth_rows(scale_history, specs=_SCALE_GROWTH_SPECS, source="test-growth-scale", run_id="run-scale")

    assert len(vectorized_scale) > 0
    # Pre-fix: `_base_row` is called ~M times per spec and rescans the full M-row
    # group each time -> ~M^2 * len(specs) group-row scans (~1M+ for M=600, 3 specs).
    # Post-fix: the main pairing loop no longer calls `_base_row` at all.
    assert base_row_scan_totals["group_rows_scanned"] <= 5 * M, (
        "compute_growth_rows's base-pairing loop must not rescan the full group via "
        f"_base_row per (current, spec) pair; saw {base_row_scan_totals['group_rows_scanned']} "
        f"group-row scans ({base_row_scan_totals['calls']} calls) for M={M} periods"
    )

    monkeypatch.undo()

    vectorized_small = compute_growth_rows(small_history, specs=small_specs, source="test-growth", run_id="run-1")
    pd.testing.assert_frame_equal(
        vectorized_small.reset_index(drop=True),
        reference_small.reset_index(drop=True),
    )

    vectorized_medium = compute_growth_rows(
        medium_history, specs=_SCALE_GROWTH_SPECS, source="test-growth-medium", run_id="run-medium"
    )
    pd.testing.assert_frame_equal(
        vectorized_medium.reset_index(drop=True),
        reference_medium.reset_index(drop=True),
    )
