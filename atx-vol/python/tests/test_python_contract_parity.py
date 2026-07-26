"""Reflection/regression gates for the production Python configuration surface."""

from __future__ import annotations

import numpy as np
import pytest

import atxvol as av


def _public_members(value) -> set[str]:
    return {name for name in dir(value) if not name.startswith("_")}


def test_run_config_exposes_every_material_execution_and_reconciliation_control():
    expected = {
        "price",
        "query_pricing_tier",
        "frictions",
        "financing",
        "record_every_n",
        "unpriced",
        "snapshot_cache",
        "prefetch_snapshots",
        "query_cache_build_policy",
        "surface_provenance_policy",
        "settlement_mark_memo",
        "reconcile_nav",
        "book_entry_fill_slippage",
        "reconcile_nav_tol",
    }
    cfg = av.RunConfig()
    assert expected <= _public_members(cfg)

    cfg.reconcile_nav = True
    cfg.book_entry_fill_slippage = True
    cfg.reconcile_nav_tol = 2.5e-7
    assert cfg.reconcile_nav is True
    assert cfg.book_entry_fill_slippage is True
    assert cfg.reconcile_nav_tol == pytest.approx(2.5e-7)


def test_financing_config_exposes_exact_share_dividend_schedule():
    financing = av.FinancingConfig()
    financing.share_dividends = [
        av.ShareDividend(uid=17, ex_ts_ns=1_775_000_000_000_000_000, amount=0.42)
    ]
    assert len(financing.share_dividends) == 1
    assert financing.share_dividends[0].uid == 17
    assert financing.share_dividends[0].amount == pytest.approx(0.42)


def test_backtest_result_exposes_optional_audit_columns_losslessly():
    result = av.BacktestResult()
    result.resize(2)
    result.date = ["2026-01-02", "2026-01-05"]
    result.ts_ns = [1, 2]
    result.nav_liquidation = [1.25, 2.50]
    result.gross_vega_abs = [10.0, 20.0]
    result.validate()

    np.testing.assert_array_equal(result.nav_liquidation, [1.25, 2.50])
    np.testing.assert_array_equal(result.gross_vega_abs, [10.0, 20.0])
    columns = result.to_dict()
    np.testing.assert_array_equal(columns["nav_liquidation"], [1.25, 2.50])
    np.testing.assert_array_equal(columns["gross_vega_abs"], [10.0, 20.0])


def test_dispersion_backtest_config_matches_the_material_cpp_surface():
    expected = {
        "target_dte_days",
        "roll_dte_days",
        "gross_index_vega",
        "delta_band",
        "min_names",
        "entry_every_n",
        "project_to_calendar_expiry",
        "record_diagnostics",
        "run",
        "side",
        "multiplier",
        "hedge_kind",
        "hedge_cadence",
        "limits",
        "weighting",
        "strike",
    }
    cfg = av.DispersionBacktestConfig()
    assert expected <= _public_members(cfg)

    cfg.side = av.DispersionSide.LONG_INDEX_SHORT_NAMES
    cfg.multiplier = 50.0
    cfg.hedge_kind = av.HedgeSpec.Kind.NONE
    cfg.hedge_cadence = av.HedgeSpec.Cadence.AT_ENTRY
    cfg.weighting = av.WeightingScheme.GAMMA_NEUTRAL
    cfg.strike.rule = av.StrikeRule.FIXED_MONEYNESS
    cfg.strike.log_moneyness = 0.04
    cfg.limits.max_gross_vega = 25_000.0
    cfg.limits.action = av.RiskBreachAction.HALT

    assert cfg.side == av.DispersionSide.LONG_INDEX_SHORT_NAMES
    assert cfg.multiplier == pytest.approx(50.0)
    assert cfg.hedge_kind == av.HedgeSpec.Kind.NONE
    assert cfg.weighting == av.WeightingScheme.GAMMA_NEUTRAL
    assert cfg.strike.rule == av.StrikeRule.FIXED_MONEYNESS
    assert cfg.strike.log_moneyness == pytest.approx(0.04)
    assert cfg.limits.max_gross_vega == pytest.approx(25_000.0)
    assert cfg.limits.action == av.RiskBreachAction.HALT


def test_point_in_time_schedule_strategy_overload_is_callable():
    rows = []
    for effective_date, symbol, weight in (
        ("2026-01-02", "SPY", 1.0),
        ("2026-01-02", "AAPL", 0.6),
        ("2026-01-02", "MSFT", 0.4),
        ("2026-02-02", "SPY", 1.0),
        ("2026-02-02", "AAPL", 1.0),
    ):
        row = av.UniverseRow()
        row.effective_date = effective_date
        row.symbol = symbol
        row.raw_weight = weight
        row.source = "test"
        row.as_of = effective_date
        rows.append(row)

    strategy = av.make_dispersion_backtest_strategy(
        rows, av.DispersionBacktestConfig(), "SPY"
    )
    assert isinstance(strategy, av.DispersionStrategy)
    # The matching run overload is registered under the same public name.
    assert "schedule" in av.run_dispersion_backtest.__doc__
    assert "index_symbol" in av.run_dispersion_backtest.__doc__


def test_listed_strategy_accepts_explicit_mark_and_fill_policies():
    # An empty schedule may be rejected by the economic validator, but the
    # binding must accept and route both enum arguments (never raise TypeError).
    try:
        strategy = av.ListedDispersionStrategy.create(
            av.ListedDispersionSchedule(),
            delta_band=0.0,
            mark_policy=av.ScheduleMarkPolicy.RECORD,
            fill_policy=av.ScheduleFillPolicy.QUOTE_MID,
        )
    except av.AtxError:
        return
    assert strategy.fill_policy == av.ScheduleFillPolicy.QUOTE_MID


def test_pricer_config_exposes_full_curve_tree_not_only_curve_kind():
    cfg = av.PricerConfig()
    assert {"curve", "curve_kind", "fit_workers"} <= _public_members(cfg)

    curve = av.CurveConfig()
    curve.kind = av.VolCurveKind.SPLINE_VOL
    curve.spline.lambda_ = 0.04
    curve.spline.min_obs = 9
    curve.parametric.loss_kind = av.CalibLossKind.MID
    cfg.curve = curve

    assert cfg.curve.kind == av.VolCurveKind.SPLINE_VOL
    assert cfg.curve.spline.lambda_ == pytest.approx(0.04)
    assert cfg.curve.spline.min_obs == 9
    assert len(cfg.curve.spline.grid) >= 4
