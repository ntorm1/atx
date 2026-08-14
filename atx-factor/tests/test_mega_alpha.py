from __future__ import annotations

import datetime as dt
import json

import numpy as np
import polars as pl

from atx_factor.config import BacktestConfig, CostModel, PortfolioConfig, WalkForwardConfig
from atx_factor.mega_alpha import AcceptanceGate, CandidateDecision, evaluate_candidate


def test_default_gate_requires_95_percent_deflated_sharpe_probability() -> None:
    assert AcceptanceGate().min_deflated_sharpe_probability == 0.95


def _panels(*, reverse_candidate: bool = False) -> tuple[pl.DataFrame, pl.DataFrame]:
    rng = np.random.default_rng(19)
    candidate_rows: list[dict[str, object]] = []
    baseline_rows: list[dict[str, object]] = []
    year, month = 2014, 1
    for _ in range(96):
        date = dt.date(year, month, 15)
        month += 1
        if month == 13:
            year += 1
            month = 1
        candidate_signal = rng.normal(size=40)
        baseline_signal = rng.normal(size=40)
        returns = (
            0.020 * candidate_signal
            + 0.004 * baseline_signal
            + rng.normal(scale=0.012, size=40)
        )
        published_candidate = -candidate_signal if reverse_candidate else candidate_signal
        for index in range(40):
            shared = {
                "date": date,
                "asset_id": f"S{index:03d}",
                "forward_return": float(returns[index]),
                "adv_usd": 2_000_000_000.0,
            }
            candidate_rows.append(
                {**shared, "signal": float(published_candidate[index])}
            )
            baseline_rows.append({**shared, "signal": float(baseline_signal[index])})
    return pl.DataFrame(candidate_rows), pl.DataFrame(baseline_rows)


def _configs() -> tuple[BacktestConfig, WalkForwardConfig, AcceptanceGate]:
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
        AcceptanceGate(
            min_candidate_oos_sharpe=0.1,
            min_deflated_sharpe_probability=0.8,
            min_marginal_mega_alpha_sharpe=0.01,
            max_turnover=1.2,
            max_drawdown=0.5,
            max_baseline_correlation=0.95,
        ),
    )


def test_candidate_is_accepted_only_when_it_improves_mega_alpha() -> None:
    candidate, baseline = _panels()
    backtest, walk_forward, gate = _configs()
    decision = evaluate_candidate(
        candidate,
        baseline,
        candidate_id="candidate",
        baseline_id="baseline",
        backtest_config=backtest,
        walk_forward_config=walk_forward,
        gate=gate,
    )
    assert decision.accepted
    assert decision.marginal_sharpe > 0.01
    assert len(decision.evidence_sha256) == 64


def test_reversed_candidate_is_rejected() -> None:
    candidate, baseline = _panels(reverse_candidate=True)
    backtest, walk_forward, gate = _configs()
    decision = evaluate_candidate(
        candidate,
        baseline,
        candidate_id="reversed",
        baseline_id="baseline",
        backtest_config=backtest,
        walk_forward_config=walk_forward,
        gate=gate,
    )
    assert not decision.accepted
    assert "candidate_oos_sharpe_below_floor" in decision.rejection_reasons


def test_decision_serializes_nonfinite_diagnostics_as_null() -> None:
    decision = CandidateDecision(
        candidate_id="candidate",
        baseline_id="constant",
        accepted=False,
        rejection_reasons=("candidate_baseline_correlation_above_ceiling",),
        candidate_metrics={"sharpe": float("nan")},
        baseline_metrics={},
        combined_metrics={},
        stressed_combined_metrics={},
        baseline_correlation=float("nan"),
        marginal_sharpe=float("-inf"),
        gate={},
        backtest_config={},
        walk_forward_config={},
        candidate_folds=({"test_start": dt.date(2025, 1, 1)},),
        baseline_folds=(),
        combined_folds=(),
        evidence_sha256="a" * 64,
    )
    payload = json.loads(decision.to_json())
    assert payload["candidate_metrics"]["sharpe"] is None
    assert payload["baseline_correlation"] is None
    assert payload["marginal_sharpe"] is None
    assert payload["candidate_folds"][0]["test_start"] == "2025-01-01"
