from __future__ import annotations

import datetime as dt

import numpy as np
import polars as pl
import pytest

import atx_factor.exploration as exploration_module
from atx_factor.config import BacktestConfig, CostModel, PortfolioConfig
from atx_factor.exploration import (
    ExplorationConfig,
    ExplorationGate,
    PortfolioVariant,
    _classify_disposition,
    _integrated_rank_panel,
    explore_candidate,
)


def _monthly_dates(count: int) -> list[dt.date]:
    dates: list[dt.date] = []
    year, month = 2014, 1
    for _ in range(count):
        dates.append(dt.date(year, month, 15))
        month += 1
        if month == 13:
            year += 1
            month = 1
    return dates


def _panels(
    *,
    reverse_validation: bool = False,
    reverse_selection: bool = False,
    long_leg_only: bool = False,
) -> tuple[pl.DataFrame, pl.DataFrame]:
    rng = np.random.default_rng(117)
    candidate_rows: list[dict[str, object]] = []
    baseline_rows: list[dict[str, object]] = []
    for date_index, date in enumerate(_monthly_dates(144)):
        signal = rng.normal(size=50)
        baseline_signal = rng.normal(size=50)
        order = np.argsort(signal)
        edge = np.zeros(50)
        edge[order[:5]] = 0.8 if long_leg_only else -1.0
        edge[order[-5:]] = 1.0
        regime = -1.0 if reverse_validation and date_index >= 87 else 1.0
        if reverse_selection:
            regime = -1.0 if date_index < 87 else 1.0
        returns = regime * 0.025 * edge + rng.normal(scale=0.010, size=50)
        for asset_index in range(50):
            shared = {
                "date": date,
                "asset_id": f"S{asset_index:03d}",
                "forward_return": float(returns[asset_index]),
                "adv_usd": 5_000_000_000.0,
            }
            candidate_rows.append(
                {**shared, "signal": float(signal[asset_index])}
            )
            baseline_rows.append(
                {**shared, "signal": float(baseline_signal[asset_index])}
            )
    return pl.DataFrame(candidate_rows), pl.DataFrame(baseline_rows)


def _configs() -> tuple[BacktestConfig, ExplorationConfig, ExplorationGate]:
    return (
        BacktestConfig(
            portfolio=PortfolioConfig(minimum_names=20, name_cap=0.10),
            costs=CostModel(
                commission_bps=0.0,
                half_spread_bps=0.0,
                impact_bps=0.0,
                aum_usd=1_000_000.0,
                annual_borrow_bps=0.0,
            ),
            holding_period_days=21,
        ),
        ExplorationConfig(
            variants=(
                PortfolioVariant("continuous_rank"),
                PortfolioVariant("decile_tails", tail_fraction=0.10),
            ),
            allocations=(0.10, 0.20, 0.30),
            combination_modes=("sleeve_mix",),
            prior_trial_count=66,
        ),
        ExplorationGate(
            min_validation_candidate_sharpe=0.0,
            min_deflated_sharpe_probability=0.0,
            min_marginal_mega_alpha_sharpe=-100.0,
            min_stressed_combined_sharpe=-100.0,
            min_positive_fold_fraction=0.50,
            max_turnover=2.0,
            max_drawdown=1.0,
            max_baseline_correlation=1.0,
            min_gross_deployment=0.90,
            min_validation_median_signal_breadth=20,
            min_shadow_median_signal_breadth=10,
        ),
    )


def test_exploration_selects_early_then_validates_frozen_tail_portfolio() -> None:
    candidate, baseline = _panels()
    backtest, exploration, gate = _configs()
    decision = explore_candidate(
        candidate,
        baseline,
        candidate_id="candidate",
        baseline_id="baseline",
        backtest_config=backtest,
        exploration_config=exploration,
        gate=gate,
    )
    assert decision.accepted
    assert decision.selected_variant == "decile_tails"
    assert decision.committed_trials == 72
    assert len(decision.selection_trials) == 6
    assert decision.validation_start > decision.selection_end
    assert decision.positive_fold_fraction == 1.0
    assert decision.selection_feasible_trial_count > 0
    assert decision.validation_candidate_breadth["median_names"] == 50
    assert decision.validation_candidate_portfolio_breadth["median_effective_breadth"] > 0


def test_exploration_rejects_early_winner_that_reverses_on_untouched_holdout() -> None:
    candidate, baseline = _panels(reverse_validation=True)
    backtest, exploration, gate = _configs()
    decision = explore_candidate(
        candidate,
        baseline,
        candidate_id="candidate",
        baseline_id="baseline",
        backtest_config=backtest,
        exploration_config=exploration,
        gate=gate,
    )
    assert decision.selected_variant == "decile_tails"
    assert not decision.accepted
    assert "validation_candidate_sharpe_below_floor" in decision.rejection_reasons
    assert "validation_positive_fold_fraction_below_floor" in decision.rejection_reasons


def test_exploration_can_select_long_tail_against_universe_when_short_leg_fails() -> None:
    candidate, baseline = _panels(long_leg_only=True)
    backtest, _, gate = _configs()
    exploration = ExplorationConfig(
        variants=(
            PortfolioVariant("quintile_tails", tail_fraction=0.20),
            PortfolioVariant(
                "top_quintile_vs_universe",
                tail_fraction=0.20,
                construction="long_tail_vs_universe",
            ),
        ),
        allocations=(0.20,),
        prior_trial_count=66,
    )
    decision = explore_candidate(
        candidate,
        baseline,
        candidate_id="candidate",
        baseline_id="baseline",
        backtest_config=backtest,
        exploration_config=exploration,
        gate=gate,
    )
    assert decision.accepted
    assert decision.selected_variant == "top_quintile_vs_universe"


def test_exploration_constructs_weights_on_selection_or_validation_only(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    candidate, baseline = _panels()
    backtest, exploration, gate = _configs()
    observed_calls: list[tuple[int, float | None]] = []
    original = exploration_module.build_target_weights

    def tracked_build_target_weights(
        panel: pl.DataFrame,
        config: PortfolioConfig,
        *,
        costs: CostModel | None = None,
    ) -> pl.DataFrame:
        observed_calls.append(
            (
                panel.get_column("date").n_unique(),
                None if costs is None else costs.aum_usd,
            )
        )
        return original(panel, config, costs=costs)

    monkeypatch.setattr(
        exploration_module,
        "build_target_weights",
        tracked_build_target_weights,
    )
    decision = explore_candidate(
        candidate,
        baseline,
        candidate_id="candidate",
        baseline_id="baseline",
        backtest_config=backtest,
        exploration_config=exploration,
        gate=gate,
    )

    assert decision.accepted
    assert observed_calls == [
        (86, 1_000_000.0),
        (86, 100_000.0),
        (86, 200_000.0),
        (86, 300_000.0),
        (86, 100_000.0),
        (86, 200_000.0),
        (86, 300_000.0),
        (57, decision.selected_allocation * 1_000_000.0),
        (57, 1_000_000.0),
    ]
    assert max(date_count for date_count, _ in observed_calls) < candidate.get_column(
        "date"
    ).n_unique()
    assert decision.sleeve_capacity_model == "allocation_scaled_aum_v1"


def test_integrated_rank_panel_uses_neutral_score_for_missing_style() -> None:
    date = dt.date(2025, 1, 31)
    candidate = pl.DataFrame(
        {
            "date": [date, date],
            "asset_id": ["A", "B"],
            "signal": [-1.0, 1.0],
            "forward_return": [0.01, 0.02],
        }
    )
    baseline = pl.DataFrame(
        {
            "date": [date, date],
            "asset_id": ["B", "C"],
            "signal": [-1.0, 1.0],
            "forward_return": [0.02, 0.03],
        }
    )

    panel = _integrated_rank_panel(candidate, baseline, allocation=0.5)
    scores = dict(zip(panel["asset_id"].to_list(), panel["signal"].to_list(), strict=True))

    assert scores["A"] == pytest.approx(-1.0 / 12.0)
    assert scores["B"] == pytest.approx(0.0)
    assert scores["C"] == pytest.approx(1.0 / 12.0)
    assert panel.height == 3


def test_integrated_mode_is_explicit_and_counted_before_validation() -> None:
    candidate, baseline = _panels()
    backtest, _, gate = _configs()
    config = ExplorationConfig(
        variants=(PortfolioVariant("continuous_rank"),),
        allocations=(0.20,),
        combination_modes=("sleeve_mix", "integrated_rank"),
        prior_trial_count=66,
    )

    decision = explore_candidate(
        candidate,
        baseline,
        candidate_id="candidate",
        baseline_id="baseline",
        backtest_config=backtest,
        exploration_config=config,
        gate=gate,
    )

    assert decision.committed_trials == 68
    assert {trial["combination_mode"] for trial in decision.selection_trials} == {
        "sleeve_mix",
        "integrated_rank",
    }
    assert decision.selected_combination_mode in {"sleeve_mix", "integrated_rank"}
    assert decision.integrated_score_model == "neutral_missing_cross_sectional_rank_v1"


def test_exploration_rejects_holdout_winner_without_selection_support() -> None:
    candidate, baseline = _panels(reverse_selection=True)
    backtest, exploration, gate = _configs()
    decision = explore_candidate(
        candidate,
        baseline,
        candidate_id="candidate",
        baseline_id="baseline",
        backtest_config=backtest,
        exploration_config=exploration,
        gate=gate,
    )
    assert decision.validation_candidate_metrics["sharpe"] > 0
    assert decision.selection_feasible_trial_count == 0
    assert "selection_no_feasible_construction" in decision.rejection_reasons
    assert decision.disposition == "rejected"
    assert decision.shadow_eligible is False


def test_disposition_reserves_shadow_for_dsr_only_failures() -> None:
    assert _classify_disposition(
        [], probabilistic_sharpe=0.10, minimum_shadow_probability=0.95
    ) == ("accepted", False)
    assert _classify_disposition(
        ["validation_deflated_sharpe_below_floor"],
        probabilistic_sharpe=0.97,
        minimum_shadow_probability=0.95,
    ) == ("shadow", True)
    assert _classify_disposition(
        ["validation_candidate_breadth_below_institutional_floor"],
        probabilistic_sharpe=0.97,
        minimum_shadow_probability=0.95,
    ) == ("shadow", True)
    assert _classify_disposition(
        [
            "validation_deflated_sharpe_below_floor",
            "validation_marginal_mega_alpha_sharpe_below_floor",
        ],
        probabilistic_sharpe=0.99,
        minimum_shadow_probability=0.95,
    ) == ("rejected", False)
