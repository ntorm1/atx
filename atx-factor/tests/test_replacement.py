from __future__ import annotations

import datetime as dt
import json

import numpy as np
import polars as pl

from atx_factor.config import BacktestConfig, CostModel, PortfolioConfig, WalkForwardConfig
from atx_factor.replacement import (
    ReplacementDecision,
    ReplacementGate,
    evaluate_replacement,
)


def _panels(*, reverse_challenger: bool = False) -> tuple[pl.DataFrame, pl.DataFrame]:
    rng = np.random.default_rng(29)
    challenger_rows: list[dict[str, object]] = []
    incumbent_rows: list[dict[str, object]] = []
    year, month = 2014, 1
    for _ in range(96):
        date = dt.date(year, month, 15)
        month += 1
        if month == 13:
            year += 1
            month = 1
        challenger_signal = rng.normal(size=40)
        incumbent_signal = rng.normal(size=40)
        returns = 0.020 * challenger_signal + rng.normal(scale=0.012, size=40)
        published = -challenger_signal if reverse_challenger else challenger_signal
        for index in range(40):
            shared = {
                "date": date,
                "asset_id": f"S{index:03d}",
                "forward_return": float(returns[index]),
                "adv_usd": 2_000_000_000.0,
            }
            challenger_rows.append({**shared, "signal": float(published[index])})
            incumbent_rows.append(
                {**shared, "signal": float(incumbent_signal[index])}
            )
    return pl.DataFrame(challenger_rows), pl.DataFrame(incumbent_rows)


def _configs() -> tuple[BacktestConfig, WalkForwardConfig, ReplacementGate]:
    return (
        BacktestConfig(
            portfolio=PortfolioConfig(minimum_names=20, name_cap=0.05),
            costs=CostModel(
                commission_bps=0.1,
                half_spread_bps=0.5,
                impact_bps=2.0,
                annual_borrow_bps=20.0,
            ),
            trial_count=8,
        ),
        WalkForwardConfig(
            minimum_train_periods=36,
            test_periods=12,
            step_periods=12,
            embargo_periods=1,
            minimum_folds=4,
        ),
        ReplacementGate(
            min_challenger_oos_sharpe=0.1,
            min_deflated_sharpe_probability=0.8,
            min_sharpe_improvement=0.05,
            max_turnover=1.2,
            max_drawdown=0.5,
        ),
    )


def test_default_gate_requires_95_percent_deflated_sharpe_probability() -> None:
    assert ReplacementGate().min_deflated_sharpe_probability == 0.95


def test_better_challenger_is_accepted() -> None:
    challenger, incumbent = _panels()
    backtest, walk_forward, gate = _configs()
    decision = evaluate_replacement(
        challenger,
        incumbent,
        challenger_id="challenger",
        incumbent_id="incumbent",
        backtest_config=backtest,
        walk_forward_config=walk_forward,
        gate=gate,
    )
    assert decision.accepted
    assert decision.sharpe_improvement > 0.05
    assert len(decision.evidence_sha256) == 64


def test_reversed_challenger_is_rejected() -> None:
    challenger, incumbent = _panels(reverse_challenger=True)
    backtest, walk_forward, gate = _configs()
    decision = evaluate_replacement(
        challenger,
        incumbent,
        challenger_id="reversed",
        incumbent_id="incumbent",
        backtest_config=backtest,
        walk_forward_config=walk_forward,
        gate=gate,
    )
    assert not decision.accepted
    assert "challenger_oos_sharpe_below_floor" in decision.rejection_reasons
    assert decision.evaluation_status == "primary_rejected"
    assert decision.incumbent_metrics == {}
    assert decision.skipped_stages == ("incumbent_comparison", "cost_stress")


def test_decision_serializes_nonfinite_diagnostics_as_null() -> None:
    decision = ReplacementDecision(
        challenger_id="challenger",
        incumbent_id="incumbent",
        accepted=False,
        rejection_reasons=("challenger_oos_sharpe_below_floor",),
        challenger_metrics={"sharpe": float("nan")},
        incumbent_metrics={},
        stressed_challenger_metrics={},
        sharpe_improvement=float("-inf"),
        evaluation_status="primary_rejected",
        skipped_stages=("incumbent_comparison", "cost_stress"),
        gate={},
        backtest_config={},
        walk_forward_config={},
        challenger_folds=({"test_start": dt.date(2025, 1, 1)},),
        incumbent_folds=(),
        evidence_sha256="a" * 64,
    )
    payload = json.loads(decision.to_json())
    assert payload["challenger_metrics"]["sharpe"] is None
    assert payload["sharpe_improvement"] is None
    assert payload["challenger_folds"][0]["test_start"] == "2025-01-01"
