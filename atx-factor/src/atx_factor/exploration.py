"""Bounded discovery-to-validation pipeline for candidate portfolio constructions."""

from __future__ import annotations

import json
import math
from dataclasses import asdict, dataclass, replace
from typing import Literal

import numpy as np
import polars as pl

from .backtest import BacktestResult, run_weight_backtest
from .config import BacktestConfig, PortfolioConfig
from .evidence import evidence_digest, json_safe
from .mega_alpha import _blend_weights, _period_correlation, _return_panel_union
from .metrics import infer_periods_per_year
from .portfolio import build_target_weights
from .schema import validate_panel

SLEEVE_CAPACITY_MODEL = "allocation_scaled_aum_v1"
SELECTION_SUPPORT_MODEL = "positive_candidate_selection_v1"
INTEGRATED_SCORE_MODEL = "neutral_missing_cross_sectional_rank_v1"
CombinationMode = Literal["sleeve_mix", "integrated_rank"]
DEFAULT_COMBINATION_MODES: tuple[CombinationMode, ...] = (
    "sleeve_mix",
    "integrated_rank",
)


@dataclass(frozen=True)
class PortfolioVariant:
    """One economically interpretable, predeclared signal-to-weight mapping."""

    variant_id: str
    rank_signal: bool = True
    tail_fraction: float | None = None
    construction: Literal[
        "symmetric", "long_tail_vs_universe", "universe_vs_short_tail"
    ] = "symmetric"

    def __post_init__(self) -> None:
        if not self.variant_id:
            raise ValueError("variant_id cannot be empty")
        if self.tail_fraction is not None and not 0 < self.tail_fraction <= 0.5:
            raise ValueError("tail_fraction must be in (0, 0.5]")
        if self.tail_fraction is not None and not self.rank_signal:
            raise ValueError("tail portfolios require rank_signal=True")
        if self.construction != "symmetric" and self.tail_fraction is None:
            raise ValueError("asymmetric constructions require tail_fraction")


DEFAULT_VARIANTS = (
    PortfolioVariant("continuous_rank"),
    PortfolioVariant("quintile_tails", tail_fraction=0.20),
    PortfolioVariant("decile_tails", tail_fraction=0.10),
    PortfolioVariant("continuous_raw", rank_signal=False),
    PortfolioVariant(
        "top_quintile_vs_universe",
        tail_fraction=0.20,
        construction="long_tail_vs_universe",
    ),
    PortfolioVariant(
        "universe_vs_bottom_quintile",
        tail_fraction=0.20,
        construction="universe_vs_short_tail",
    ),
)


@dataclass(frozen=True)
class ExplorationConfig:
    """Frozen search space and chronological holdout policy."""

    selection_fraction: float = 0.60
    embargo_periods: int = 1
    minimum_selection_periods: int = 60
    minimum_validation_periods: int = 48
    fold_periods: int = 12
    variants: tuple[PortfolioVariant, ...] = DEFAULT_VARIANTS
    allocations: tuple[float, ...] = (0.10, 0.20, 0.30)
    combination_modes: tuple[CombinationMode, ...] = DEFAULT_COMBINATION_MODES
    prior_trial_count: int = 0

    def __post_init__(self) -> None:
        if not 0.25 <= self.selection_fraction <= 0.80:
            raise ValueError("selection_fraction must be in [0.25, 0.80]")
        if self.embargo_periods < 1:
            raise ValueError("embargo_periods must be positive")
        if self.minimum_selection_periods < 12 or self.minimum_validation_periods < 12:
            raise ValueError("selection and validation windows must each span at least 12 periods")
        if self.fold_periods < 3:
            raise ValueError("fold_periods must be at least 3")
        if not self.variants or len({item.variant_id for item in self.variants}) != len(
            self.variants
        ):
            raise ValueError("variants must be non-empty with unique ids")
        if not self.allocations or any(not 0 < value <= 1 for value in self.allocations):
            raise ValueError("allocations must be non-empty and in (0, 1]")
        if len(set(self.allocations)) != len(self.allocations):
            raise ValueError("allocations must be unique")
        if not self.combination_modes or len(set(self.combination_modes)) != len(
            self.combination_modes
        ):
            raise ValueError("combination_modes must be non-empty and unique")
        if any(mode not in DEFAULT_COMBINATION_MODES for mode in self.combination_modes):
            raise ValueError(f"unsupported combination mode: {self.combination_modes}")
        if self.prior_trial_count < 0:
            raise ValueError("prior_trial_count cannot be negative")

    @property
    def committed_trials(self) -> int:
        return (
            self.prior_trial_count
            + len(self.variants)
            * len(self.allocations)
            * len(self.combination_modes)
        )


@dataclass(frozen=True)
class ExplorationGate:
    min_validation_candidate_sharpe: float = 0.50
    min_deflated_sharpe_probability: float = 0.95
    min_shadow_probabilistic_sharpe: float = 0.95
    min_marginal_mega_alpha_sharpe: float = 0.05
    min_stressed_combined_sharpe: float = 0.0
    min_positive_fold_fraction: float = 0.50
    max_turnover: float = 0.70
    max_drawdown: float = 0.25
    max_baseline_correlation: float = 0.70
    min_gross_deployment: float = 0.95
    min_validation_median_signal_breadth: int = 1_000
    min_shadow_median_signal_breadth: int = 100

    def __post_init__(self) -> None:
        if not 0 <= self.min_deflated_sharpe_probability <= 1:
            raise ValueError("min_deflated_sharpe_probability must be in [0, 1]")
        if not 0 <= self.min_shadow_probabilistic_sharpe <= 1:
            raise ValueError("min_shadow_probabilistic_sharpe must be in [0, 1]")
        if not 0 <= self.min_positive_fold_fraction <= 1:
            raise ValueError("min_positive_fold_fraction must be in [0, 1]")
        if self.max_turnover <= 0 or not 0 < self.max_drawdown <= 1:
            raise ValueError("turnover and drawdown limits must be positive")
        if not 0 <= self.max_baseline_correlation <= 1:
            raise ValueError("max_baseline_correlation must be in [0, 1]")
        if not 0 < self.min_gross_deployment <= 1:
            raise ValueError("min_gross_deployment must be in (0, 1]")
        if self.min_validation_median_signal_breadth < 1:
            raise ValueError("production signal breadth floor must be positive")
        if not (
            1
            <= self.min_shadow_median_signal_breadth
            <= self.min_validation_median_signal_breadth
        ):
            raise ValueError(
                "shadow signal breadth floor must be positive and no greater than production"
            )


@dataclass(frozen=True)
class ExplorationDecision:
    candidate_id: str
    baseline_id: str
    accepted: bool
    disposition: Literal["accepted", "shadow", "rejected"]
    shadow_eligible: bool
    rejection_reasons: tuple[str, ...]
    selected_variant: str
    selected_allocation: float
    selected_combination_mode: CombinationMode
    sleeve_capacity_model: str
    integrated_score_model: str
    selection_support_model: str
    selection_feasible_trial_count: int
    selection_candidate_breadth: dict[str, int | float]
    validation_candidate_breadth: dict[str, int | float]
    validation_baseline_breadth: dict[str, int | float]
    validation_candidate_portfolio_breadth: dict[str, int | float]
    validation_combined_portfolio_breadth: dict[str, int | float]
    selection_end: object
    validation_start: object
    committed_trials: int
    selection_trials: tuple[dict[str, object], ...]
    validation_candidate_metrics: dict[str, int | float | None]
    validation_baseline_metrics: dict[str, int | float | None]
    validation_combined_metrics: dict[str, int | float | None]
    validation_stressed_metrics: dict[str, int | float | None]
    validation_baseline_correlation: float
    validation_marginal_sharpe: float
    validation_folds: tuple[dict[str, object], ...]
    positive_fold_fraction: float
    exploration_config: dict[str, object]
    gate: dict[str, int | float]
    backtest_config: dict[str, object]
    evidence_sha256: str

    def to_dict(self) -> dict[str, object]:
        payload = json_safe(asdict(self))
        if not isinstance(payload, dict):
            raise TypeError("exploration decision did not serialize to an object")
        return payload

    def to_json(self, *, indent: int = 2) -> str:
        return json.dumps(self.to_dict(), sort_keys=True, indent=indent, allow_nan=False)


def _portfolio_config(base: PortfolioConfig, variant: PortfolioVariant) -> PortfolioConfig:
    return replace(
        base,
        rank_signal=variant.rank_signal,
        tail_fraction=variant.tail_fraction,
        construction=variant.construction,
    )


def _breadth_summary(panel: pl.DataFrame) -> dict[str, int | float]:
    """Summarize eligible signal names by formation date."""

    counts = panel.group_by("date").agg(pl.col("asset_id").n_unique().alias("names"))
    values = counts.get_column("names").cast(pl.Float64)
    return {
        "dates": counts.height,
        "minimum_names": int(values.min()),
        "p10_names": float(values.quantile(0.10, interpolation="linear")),
        "median_names": float(values.median()),
        "mean_names": float(values.mean()),
        "maximum_names": int(values.max()),
    }


def _portfolio_breadth_summary(weights: pl.DataFrame) -> dict[str, int | float]:
    """Summarize nonzero holdings and the gross-weight effective number of bets."""

    weight_column = "target_weight" if "target_weight" in weights.columns else "weight"
    daily = (
        weights.filter(pl.col(weight_column).abs() > 1e-12)
        .group_by("date")
        .agg(
            pl.len().alias("holdings"),
            (
                pl.col(weight_column).abs().sum().pow(2)
                / pl.col(weight_column).pow(2).sum()
            ).alias("effective_breadth"),
        )
    )
    holdings = daily.get_column("holdings").cast(pl.Float64)
    effective = daily.get_column("effective_breadth")
    return {
        "dates": daily.height,
        "minimum_holdings": int(holdings.min()),
        "median_holdings": float(holdings.median()),
        "mean_holdings": float(holdings.mean()),
        "median_effective_breadth": float(effective.median()),
        "mean_effective_breadth": float(effective.mean()),
    }


def _backtest_config(
    base: BacktestConfig,
    portfolio: PortfolioConfig,
    *,
    trial_count: int,
    stressed: bool = False,
    aum_fraction: float = 1.0,
) -> BacktestConfig:
    if not 0 < aum_fraction <= 1:
        raise ValueError("aum_fraction must be in (0, 1]")
    costs = base.costs.stressed() if stressed else base.costs
    costs = replace(costs, aum_usd=costs.aum_usd * aum_fraction)
    return replace(
        base,
        portfolio=portfolio,
        costs=costs,
        trial_count=max(base.trial_count, trial_count),
    )


def _align_dates(
    candidate: pl.DataFrame, baseline: pl.DataFrame
) -> tuple[pl.DataFrame, pl.DataFrame, list[object]]:
    common = candidate.select("date").unique().join(
        baseline.select("date").unique(), on="date", how="inner"
    )
    dates = common.get_column("date").sort().to_list()
    if not dates:
        raise ValueError("candidate and baseline have no common formation dates")
    return (
        candidate.filter(pl.col("date").is_in(dates)),
        baseline.filter(pl.col("date").is_in(dates)),
        dates,
    )


def _integrated_rank_panel(
    candidate: pl.DataFrame,
    baseline: pl.DataFrame,
    *,
    allocation: float,
) -> pl.DataFrame:
    """Blend same-date cross-sectional ranks before constructing one portfolio.

    A missing style receives the cross-sectional neutral score, preserving the union of
    independently eligible names without manufacturing a favorable rank. Forward returns and
    trading metadata retain the strict agreement checks of ``_return_panel_union``.
    """

    if not 0 < allocation <= 1:
        raise ValueError("allocation must be in (0, 1]")

    def ranked(frame: pl.DataFrame, output_column: str) -> pl.DataFrame:
        count = pl.len().over("date")
        score = pl.col("signal").rank(method="average").over("date") / (
            count + 1.0
        ) - 0.5
        return frame.select("date", "asset_id", score.alias(output_column))

    scores = ranked(baseline, "baseline_rank_score").join(
        ranked(candidate, "candidate_rank_score"),
        on=["date", "asset_id"],
        how="full",
        coalesce=True,
    ).with_columns(
        pl.col("baseline_rank_score").fill_null(0.0),
        pl.col("candidate_rank_score").fill_null(0.0),
    ).with_columns(
        (
            (1.0 - allocation) * pl.col("baseline_rank_score")
            + allocation * pl.col("candidate_rank_score")
        ).alias("signal")
    )
    return (
        _return_panel_union(candidate, baseline)
        .drop("signal")
        .join(scores.select("date", "asset_id", "signal"), on=["date", "asset_id"])
        .sort("date", "asset_id")
    )


def _combined_weights(
    candidate: pl.DataFrame,
    baseline: pl.DataFrame,
    candidate_weights: pl.DataFrame,
    baseline_weights: pl.DataFrame,
    *,
    allocation: float,
    combination_mode: CombinationMode,
    portfolio: PortfolioConfig,
    backtest_config: BacktestConfig,
) -> tuple[pl.DataFrame, pl.DataFrame]:
    """Return the combined return panel and weights for one declared combination mode."""

    if combination_mode == "sleeve_mix":
        return (
            _return_panel_union(candidate, baseline),
            _blend_weights(
                baseline_weights,
                candidate_weights,
                allocation=allocation,
                backtest_config=backtest_config,
            ),
        )
    if combination_mode == "integrated_rank":
        panel = _integrated_rank_panel(candidate, baseline, allocation=allocation)
        return panel, build_target_weights(
            panel,
            portfolio,
            costs=backtest_config.costs,
        )
    raise ValueError(f"unsupported combination mode: {combination_mode}")


def _evaluate_weights(
    panel: pl.DataFrame,
    weights: pl.DataFrame,
    dates: list[object],
    config: BacktestConfig,
) -> BacktestResult:
    return run_weight_backtest(
        panel.filter(pl.col("date").is_in(dates)),
        weights.filter(pl.col("date").is_in(dates)),
        config,
    )


def _fold_diagnostics(periods: pl.DataFrame, fold_periods: int) -> tuple[dict[str, object], ...]:
    ordered = periods.sort("date").with_row_index("_row").with_columns(
        (pl.col("_row") // fold_periods).alias("_fold")
    )
    rows: list[dict[str, object]] = []
    for fold, frame in ordered.partition_by("_fold", as_dict=True).items():
        returns = frame.get_column("net_return").to_numpy()
        volatility = float(np.std(returns, ddof=1)) if len(returns) > 1 else float("nan")
        periods_per_year = infer_periods_per_year(frame.get_column("date").to_numpy())
        sharpe = (
            float(np.mean(returns) / volatility * math.sqrt(periods_per_year))
            if volatility > 0
            else float("nan")
        )
        rows.append(
            {
                "fold": int(fold[0] if isinstance(fold, tuple) else fold),
                "start": frame.get_column("date").min(),
                "end": frame.get_column("date").max(),
                "observations": frame.height,
                "net_return": float(np.prod(1.0 + returns) - 1.0),
                "sharpe": sharpe,
                "positive": bool(float(np.mean(returns)) > 0),
            }
        )
    return tuple(rows)


def _classify_disposition(
    reasons: list[str],
    *,
    probabilistic_sharpe: float,
    minimum_shadow_probability: float,
) -> tuple[Literal["accepted", "shadow", "rejected"], bool]:
    shadow_reasons = {
        "validation_deflated_sharpe_below_floor",
        "validation_candidate_breadth_below_institutional_floor",
    }
    shadow_eligible = bool(reasons) and set(reasons).issubset(shadow_reasons) and (
        probabilistic_sharpe >= minimum_shadow_probability
    )
    disposition: Literal["accepted", "shadow", "rejected"] = (
        "accepted" if not reasons else "shadow" if shadow_eligible else "rejected"
    )
    return disposition, shadow_eligible


def explore_candidate(
    candidate_panel: pl.DataFrame,
    baseline_panel: pl.DataFrame,
    *,
    candidate_id: str,
    baseline_id: str,
    backtest_config: BacktestConfig | None = None,
    exploration_config: ExplorationConfig | None = None,
    gate: ExplorationGate | None = None,
) -> ExplorationDecision:
    """Select a bounded construction early, then make one untouched holdout decision."""

    base_backtest = backtest_config or BacktestConfig()
    exploration = exploration_config or ExplorationConfig()
    gate = gate or ExplorationGate()
    if base_backtest.holding_period_days != 21:
        raise ValueError(
            "portfolio exploration currently requires non-overlapping monthly 21-day returns"
        )
    candidate, baseline, dates = _align_dates(
        validate_panel(candidate_panel), validate_panel(baseline_panel)
    )
    split = int(len(dates) * exploration.selection_fraction)
    validation_start_index = split + exploration.embargo_periods
    selection_dates = dates[:split]
    validation_dates = dates[validation_start_index:]
    if len(selection_dates) < exploration.minimum_selection_periods:
        raise ValueError("insufficient selection history")
    if len(validation_dates) < exploration.minimum_validation_periods:
        raise ValueError("insufficient untouched validation history")

    committed_trials = exploration.committed_trials
    selection_candidate = candidate.filter(pl.col("date").is_in(selection_dates))
    selection_baseline = baseline.filter(pl.col("date").is_in(selection_dates))
    baseline_portfolio = replace(base_backtest.portfolio, tail_fraction=None)
    baseline_selection_weights = build_target_weights(
        selection_baseline, baseline_portfolio, costs=base_backtest.costs
    )
    selection_config = _backtest_config(
        base_backtest, baseline_portfolio, trial_count=committed_trials
    )
    baseline_selection = _evaluate_weights(
        selection_baseline,
        baseline_selection_weights,
        selection_dates,
        selection_config,
    )

    trials: list[dict[str, object]] = []
    candidate_portfolios: dict[str, PortfolioConfig] = {}
    for variant in exploration.variants:
        portfolio = _portfolio_config(base_backtest.portfolio, variant)
        candidate_portfolios[variant.variant_id] = portfolio
        for allocation in exploration.allocations:
            sleeve_config = _backtest_config(
                base_backtest,
                portfolio,
                trial_count=committed_trials,
                aum_fraction=allocation,
            )
            candidate_selection_weights = build_target_weights(
                selection_candidate, portfolio, costs=sleeve_config.costs
            )
            candidate_selection = _evaluate_weights(
                selection_candidate,
                candidate_selection_weights,
                selection_dates,
                sleeve_config,
            )
            for combination_mode in exploration.combination_modes:
                combined_panel, combined_weights = _combined_weights(
                    selection_candidate,
                    selection_baseline,
                    candidate_selection_weights,
                    baseline_selection_weights,
                    allocation=allocation,
                    combination_mode=combination_mode,
                    portfolio=portfolio,
                    backtest_config=selection_config,
                )
                combined_selection = _evaluate_weights(
                    combined_panel,
                    combined_weights,
                    selection_dates,
                    selection_config,
                )
                marginal = (
                    combined_selection.metrics.sharpe
                    - baseline_selection.metrics.sharpe
                )
                feasible = (
                    candidate_selection.metrics.sharpe > 0
                    and combined_selection.metrics.average_turnover <= gate.max_turnover
                    and combined_selection.metrics.max_participation
                    <= base_backtest.costs.max_participation
                )
                trials.append(
                    {
                        "variant_id": variant.variant_id,
                        "allocation": allocation,
                        "combination_mode": combination_mode,
                        "candidate_sharpe": candidate_selection.metrics.sharpe,
                        "combined_sharpe": combined_selection.metrics.sharpe,
                        "baseline_sharpe": baseline_selection.metrics.sharpe,
                        "marginal_sharpe": marginal,
                        "turnover": combined_selection.metrics.average_turnover,
                        "max_participation": combined_selection.metrics.max_participation,
                        "feasible": feasible,
                    }
                )
    ranked = sorted(
        trials,
        key=lambda row: (
            bool(row["feasible"]),
            float(row["marginal_sharpe"]),
            float(row["candidate_sharpe"]),
            -float(row["turnover"]),
            str(row["variant_id"]),
            str(row["combination_mode"]),
            -float(row["allocation"]),
        ),
        reverse=True,
    )
    feasible_trial_count = sum(bool(row["feasible"]) for row in trials)
    winner = ranked[0]
    selected_variant = str(winner["variant_id"])
    selected_allocation = float(winner["allocation"])
    selected_combination_mode = str(winner["combination_mode"])
    if selected_combination_mode not in DEFAULT_COMBINATION_MODES:
        raise ValueError(f"unsupported selected combination mode: {selected_combination_mode}")
    portfolio = candidate_portfolios[selected_variant]
    validation_config = _backtest_config(
        base_backtest, portfolio, trial_count=committed_trials
    )
    candidate_validation_config = _backtest_config(
        base_backtest,
        portfolio,
        trial_count=committed_trials,
        aum_fraction=selected_allocation,
    )
    validation_candidate = candidate.filter(pl.col("date").is_in(validation_dates))
    validation_baseline = baseline.filter(pl.col("date").is_in(validation_dates))
    candidate_validation_weights = build_target_weights(
        validation_candidate, portfolio, costs=candidate_validation_config.costs
    )
    baseline_validation_weights = build_target_weights(
        validation_baseline, baseline_portfolio, costs=base_backtest.costs
    )
    validation_return_panel, combined_weights = _combined_weights(
        validation_candidate,
        validation_baseline,
        candidate_validation_weights,
        baseline_validation_weights,
        allocation=selected_allocation,
        combination_mode=selected_combination_mode,
        portfolio=portfolio,
        backtest_config=validation_config,
    )
    candidate_validation = _evaluate_weights(
        validation_candidate,
        candidate_validation_weights,
        validation_dates,
        candidate_validation_config,
    )
    baseline_validation = _evaluate_weights(
        validation_baseline,
        baseline_validation_weights,
        validation_dates,
        validation_config,
    )
    combined_validation = _evaluate_weights(
        validation_return_panel,
        combined_weights,
        validation_dates,
        validation_config,
    )
    stressed_config = _backtest_config(
        base_backtest,
        portfolio,
        trial_count=committed_trials,
        stressed=True,
    )
    stressed_validation = _evaluate_weights(
        validation_return_panel,
        combined_weights,
        validation_dates,
        stressed_config,
    )
    correlation = _period_correlation(
        candidate_validation.periods, baseline_validation.periods
    )
    marginal_sharpe = (
        combined_validation.metrics.sharpe - baseline_validation.metrics.sharpe
    )
    folds = _fold_diagnostics(candidate_validation.periods, exploration.fold_periods)
    positive_fold_fraction = sum(bool(row["positive"]) for row in folds) / len(folds)
    candidate_deployment = (
        candidate_validation.metrics.minimum_gross_exposure / portfolio.gross_leverage
    )
    combined_deployment = (
        combined_validation.metrics.minimum_gross_exposure / portfolio.gross_leverage
    )
    reasons: list[str] = []
    if feasible_trial_count == 0:
        reasons.append("selection_no_feasible_construction")
    selection_breadth = _breadth_summary(
        candidate.filter(pl.col("date").is_in(selection_dates))
    )
    validation_candidate_breadth = _breadth_summary(validation_candidate)
    validation_baseline_breadth = _breadth_summary(validation_baseline)
    validation_candidate_portfolio_breadth = _portfolio_breadth_summary(
        candidate_validation_weights
    )
    validation_combined_portfolio_breadth = _portfolio_breadth_summary(combined_weights)
    median_signal_breadth = float(validation_candidate_breadth["median_names"])
    if median_signal_breadth < gate.min_shadow_median_signal_breadth:
        reasons.append("validation_candidate_breadth_below_shadow_floor")
    elif median_signal_breadth < gate.min_validation_median_signal_breadth:
        reasons.append("validation_candidate_breadth_below_institutional_floor")
    if candidate_validation.metrics.sharpe < gate.min_validation_candidate_sharpe:
        reasons.append("validation_candidate_sharpe_below_floor")
    if (
        candidate_validation.metrics.deflated_sharpe_probability
        < gate.min_deflated_sharpe_probability
    ):
        reasons.append("validation_deflated_sharpe_below_floor")
    if marginal_sharpe < gate.min_marginal_mega_alpha_sharpe:
        reasons.append("validation_marginal_mega_alpha_sharpe_below_floor")
    if stressed_validation.metrics.sharpe < gate.min_stressed_combined_sharpe:
        reasons.append("validation_cost_stress_sharpe_below_floor")
    if positive_fold_fraction < gate.min_positive_fold_fraction:
        reasons.append("validation_positive_fold_fraction_below_floor")
    if combined_validation.metrics.average_turnover > gate.max_turnover:
        reasons.append("validation_combined_turnover_above_ceiling")
    if combined_validation.metrics.max_participation > base_backtest.costs.max_participation:
        reasons.append("validation_combined_participation_above_ceiling")
    if candidate_deployment < gate.min_gross_deployment:
        reasons.append("validation_candidate_gross_deployment_below_floor")
    if combined_deployment < gate.min_gross_deployment:
        reasons.append("validation_combined_gross_deployment_below_floor")
    if abs(combined_validation.metrics.max_drawdown) > gate.max_drawdown:
        reasons.append("validation_combined_drawdown_above_ceiling")
    if not math.isfinite(correlation) or abs(correlation) > gate.max_baseline_correlation:
        reasons.append("validation_candidate_baseline_correlation_above_ceiling")

    disposition, shadow_eligible = _classify_disposition(
        reasons,
        probabilistic_sharpe=candidate_validation.metrics.probabilistic_sharpe,
        minimum_shadow_probability=gate.min_shadow_probabilistic_sharpe,
    )

    payload: dict[str, object] = {
        "candidate_id": candidate_id,
        "baseline_id": baseline_id,
        "accepted": not reasons,
        "disposition": disposition,
        "shadow_eligible": shadow_eligible,
        "rejection_reasons": reasons,
        "selected_variant": selected_variant,
        "selected_allocation": selected_allocation,
        "selected_combination_mode": selected_combination_mode,
        "sleeve_capacity_model": SLEEVE_CAPACITY_MODEL,
        "integrated_score_model": INTEGRATED_SCORE_MODEL,
        "selection_support_model": SELECTION_SUPPORT_MODEL,
        "selection_feasible_trial_count": feasible_trial_count,
        "selection_candidate_breadth": selection_breadth,
        "validation_candidate_breadth": validation_candidate_breadth,
        "validation_baseline_breadth": validation_baseline_breadth,
        "validation_candidate_portfolio_breadth": (
            validation_candidate_portfolio_breadth
        ),
        "validation_combined_portfolio_breadth": (
            validation_combined_portfolio_breadth
        ),
        "selection_end": selection_dates[-1],
        "validation_start": validation_dates[0],
        "committed_trials": committed_trials,
        "selection_trials": trials,
        "validation_candidate_metrics": candidate_validation.metrics.to_dict(),
        "validation_baseline_metrics": baseline_validation.metrics.to_dict(),
        "validation_combined_metrics": combined_validation.metrics.to_dict(),
        "validation_stressed_metrics": stressed_validation.metrics.to_dict(),
        "validation_baseline_correlation": correlation,
        "validation_marginal_sharpe": marginal_sharpe,
        "validation_folds": folds,
        "positive_fold_fraction": positive_fold_fraction,
        "exploration_config": asdict(exploration),
        "gate": asdict(gate),
        "backtest_config": asdict(base_backtest),
    }
    return ExplorationDecision(
        **payload,
        evidence_sha256=evidence_digest(payload),
    )
