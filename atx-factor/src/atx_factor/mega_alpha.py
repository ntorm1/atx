"""Marginal mega-alpha admission with cost and multiple-testing gates."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
from dataclasses import asdict, dataclass

import numpy as np
import polars as pl

from .config import BacktestConfig, WalkForwardConfig
from .portfolio import build_target_weights, normalize_weight_scores
from .schema import validate_panel
from .walk_forward import WalkForwardResult, walk_forward_weight_backtest


def _json_safe(value: object) -> object:
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    return value


@dataclass(frozen=True)
class AcceptanceGate:
    min_candidate_oos_sharpe: float = 0.50
    min_deflated_sharpe_probability: float = 0.90
    min_marginal_mega_alpha_sharpe: float = 0.05
    min_cost_stress_sharpe: float = 0.0
    max_turnover: float = 0.70
    max_drawdown: float = 0.25
    max_baseline_correlation: float = 0.70
    min_gross_deployment: float = 0.95
    candidate_allocation: float = 0.20

    def __post_init__(self) -> None:
        if not 0 < self.candidate_allocation <= 1:
            raise ValueError("candidate_allocation must be in (0, 1]")
        if not 0 <= self.min_deflated_sharpe_probability <= 1:
            raise ValueError("min_deflated_sharpe_probability must be in [0, 1]")
        if self.max_turnover <= 0 or not 0 < self.max_drawdown <= 1:
            raise ValueError("turnover and drawdown limits must be positive")
        if not 0 <= self.max_baseline_correlation <= 1:
            raise ValueError("max_baseline_correlation must be in [0, 1]")
        if not 0 < self.min_gross_deployment <= 1:
            raise ValueError("min_gross_deployment must be in (0, 1]")


@dataclass(frozen=True)
class CandidateDecision:
    candidate_id: str
    baseline_id: str
    accepted: bool
    rejection_reasons: tuple[str, ...]
    candidate_metrics: dict[str, int | float | None]
    baseline_metrics: dict[str, int | float | None]
    combined_metrics: dict[str, int | float | None]
    stressed_combined_metrics: dict[str, int | float | None]
    baseline_correlation: float
    marginal_sharpe: float
    gate: dict[str, int | float]
    backtest_config: dict[str, object]
    walk_forward_config: dict[str, object]
    candidate_folds: tuple[dict[str, object], ...]
    baseline_folds: tuple[dict[str, object], ...]
    combined_folds: tuple[dict[str, object], ...]
    evidence_sha256: str

    def to_dict(self) -> dict[str, object]:
        payload = _json_safe(asdict(self))
        if not isinstance(payload, dict):
            raise TypeError("candidate decision did not serialize to an object")
        return payload

    def to_json(self, *, indent: int = 2) -> str:
        return json.dumps(self.to_dict(), sort_keys=True, indent=indent, allow_nan=False)


def _return_panel_union(candidate: pl.DataFrame, baseline: pl.DataFrame) -> pl.DataFrame:
    metadata_columns = [
        column
        for column in ("forward_end_date", "adv_usd", "borrow_rate", "group")
        if column in candidate.columns or column in baseline.columns
    ]
    select_columns = ["date", "asset_id", "forward_return", *metadata_columns]

    def select_available(frame: pl.DataFrame) -> pl.DataFrame:
        expressions: list[pl.Expr] = [
            pl.col("date"),
            pl.col("asset_id"),
            pl.col("forward_return"),
        ]
        for column in metadata_columns:
            if column in frame.columns:
                expressions.append(pl.col(column))
            else:
                dtype = pl.Date if column == "forward_end_date" else pl.Float64
                if column == "group":
                    dtype = pl.String
                expressions.append(pl.lit(None, dtype=dtype).alias(column))
        return frame.select(expressions)

    left = select_available(candidate)
    right = select_available(baseline)
    overlap = left.select(
        "date", "asset_id", pl.col("forward_return").alias("candidate_return")
    ).join(
        right.select("date", "asset_id", pl.col("forward_return").alias("baseline_return")),
        on=["date", "asset_id"],
        how="inner",
    )
    if not overlap.is_empty() and overlap.select(
        ((pl.col("candidate_return") - pl.col("baseline_return")).abs() > 1e-12).any()
    ).item():
        raise ValueError("candidate and baseline disagree on forward returns for common keys")
    combined = pl.concat([left, right], how="diagonal_relaxed").group_by(
        "date", "asset_id"
    ).agg(
        pl.col("forward_return").first(),
        *[pl.col(column).drop_nulls().first().alias(column) for column in metadata_columns],
    )
    return combined.select(select_columns).with_columns(pl.lit(0.0).alias("signal"))


def _blend_weights(
    baseline_weights: pl.DataFrame,
    candidate_weights: pl.DataFrame,
    *,
    allocation: float,
    backtest_config: BacktestConfig,
) -> pl.DataFrame:
    baseline_expressions: list[pl.Expr | str] = [
        "date",
        "asset_id",
        pl.col("target_weight").alias("baseline_weight"),
    ]
    candidate_expressions: list[pl.Expr | str] = [
        "date",
        "asset_id",
        pl.col("target_weight").alias("candidate_weight"),
    ]
    baseline_expressions.append(
        pl.col("adv_usd").alias("baseline_adv_usd")
        if "adv_usd" in baseline_weights.columns
        else pl.lit(None, dtype=pl.Float64).alias("baseline_adv_usd")
    )
    candidate_expressions.append(
        pl.col("adv_usd").alias("candidate_adv_usd")
        if "adv_usd" in candidate_weights.columns
        else pl.lit(None, dtype=pl.Float64).alias("candidate_adv_usd")
    )
    baseline = baseline_weights.select(baseline_expressions)
    candidate = candidate_weights.select(candidate_expressions)
    score = baseline.join(
        candidate,
        on=["date", "asset_id"],
        how="full",
        coalesce=True,
    ).with_columns(
        pl.col("baseline_weight").fill_null(0.0),
        pl.col("candidate_weight").fill_null(0.0),
        pl.coalesce("baseline_adv_usd", "candidate_adv_usd").alias("adv_usd"),
    ).with_columns(
        (
            (1.0 - allocation) * pl.col("baseline_weight")
            + allocation * pl.col("candidate_weight")
        ).alias("weight_score")
    )
    return normalize_weight_scores(
        score,
        backtest_config.portfolio,
        score_column="weight_score",
        costs=backtest_config.costs,
    )


def _period_correlation(left: pl.DataFrame, right: pl.DataFrame) -> float:
    joined = left.select("date", pl.col("net_return").alias("left_return")).join(
        right.select("date", pl.col("net_return").alias("right_return")),
        on="date",
        how="inner",
    )
    if joined.height < 3:
        return float("nan")
    correlation = np.corrcoef(
        joined.get_column("left_return").to_numpy(),
        joined.get_column("right_return").to_numpy(),
    )[0, 1]
    return float(correlation)


def _evidence_digest(payload: dict[str, object]) -> str:
    serialized = json.dumps(
        _json_safe(payload),
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    )
    return hashlib.sha256(serialized.encode()).hexdigest()


def evaluate_candidate(
    candidate_panel: pl.DataFrame,
    baseline_panel: pl.DataFrame,
    *,
    candidate_id: str,
    baseline_id: str,
    backtest_config: BacktestConfig | None = None,
    walk_forward_config: WalkForwardConfig | None = None,
    gate: AcceptanceGate | None = None,
) -> CandidateDecision:
    """Decide whether a candidate improves the current mega-alpha out of sample."""

    backtest_config = backtest_config or BacktestConfig()
    walk_forward_config = walk_forward_config or WalkForwardConfig()
    gate = gate or AcceptanceGate()
    candidate = validate_panel(candidate_panel)
    baseline = validate_panel(baseline_panel)
    common_dates = candidate.select("date").unique().join(
        baseline.select("date").unique(), on="date", how="inner"
    )
    candidate = candidate.join(common_dates, on="date", how="semi")
    baseline = baseline.join(common_dates, on="date", how="semi")
    if common_dates.is_empty():
        raise ValueError("candidate and baseline have no common formation dates")
    candidate_weights = build_target_weights(
        candidate,
        backtest_config.portfolio,
        costs=backtest_config.costs,
    )
    baseline_weights = build_target_weights(
        baseline,
        backtest_config.portfolio,
        costs=backtest_config.costs,
    )
    return_panel = _return_panel_union(candidate, baseline)
    combined_weights = _blend_weights(
        baseline_weights,
        candidate_weights,
        allocation=gate.candidate_allocation,
        backtest_config=backtest_config,
    )
    candidate_result: WalkForwardResult = walk_forward_weight_backtest(
        candidate,
        candidate_weights,
        backtest_config,
        walk_forward_config,
    )
    baseline_result = walk_forward_weight_backtest(
        baseline,
        baseline_weights,
        backtest_config,
        walk_forward_config,
    )
    combined_result = walk_forward_weight_backtest(
        return_panel,
        combined_weights,
        backtest_config,
        walk_forward_config,
    )
    stressed_config = BacktestConfig(
        portfolio=backtest_config.portfolio,
        costs=backtest_config.costs.stressed(),
        holding_period_days=backtest_config.holding_period_days,
        trial_count=backtest_config.trial_count,
        trial_sharpe_std=backtest_config.trial_sharpe_std,
    )
    stressed_result = walk_forward_weight_backtest(
        return_panel,
        combined_weights,
        stressed_config,
        walk_forward_config,
    )
    correlation = _period_correlation(candidate_result.periods, baseline_result.periods)
    marginal_sharpe = combined_result.metrics.sharpe - baseline_result.metrics.sharpe
    reasons: list[str] = []
    if candidate_result.metrics.sharpe < gate.min_candidate_oos_sharpe:
        reasons.append("candidate_oos_sharpe_below_floor")
    if (
        candidate_result.metrics.deflated_sharpe_probability
        < gate.min_deflated_sharpe_probability
    ):
        reasons.append("candidate_deflated_sharpe_below_floor")
    if marginal_sharpe < gate.min_marginal_mega_alpha_sharpe:
        reasons.append("marginal_mega_alpha_sharpe_below_floor")
    if stressed_result.metrics.sharpe < gate.min_cost_stress_sharpe:
        reasons.append("cost_stress_sharpe_below_floor")
    if combined_result.metrics.average_turnover > gate.max_turnover:
        reasons.append("combined_turnover_above_ceiling")
    if combined_result.metrics.max_participation > backtest_config.costs.max_participation:
        reasons.append("combined_participation_above_ceiling")
    candidate_deployment = (
        candidate_result.metrics.minimum_gross_exposure
        / backtest_config.portfolio.gross_leverage
    )
    combined_deployment = (
        combined_result.metrics.minimum_gross_exposure
        / backtest_config.portfolio.gross_leverage
    )
    if candidate_deployment < gate.min_gross_deployment:
        reasons.append("candidate_gross_deployment_below_floor")
    if combined_deployment < gate.min_gross_deployment:
        reasons.append("combined_gross_deployment_below_floor")
    if abs(combined_result.metrics.max_drawdown) > gate.max_drawdown:
        reasons.append("combined_drawdown_above_ceiling")
    if not math.isfinite(correlation) or abs(correlation) > gate.max_baseline_correlation:
        reasons.append("candidate_baseline_correlation_above_ceiling")
    payload: dict[str, object] = {
        "candidate_id": candidate_id,
        "baseline_id": baseline_id,
        "accepted": not reasons,
        "rejection_reasons": reasons,
        "candidate_metrics": candidate_result.metrics.to_dict(),
        "baseline_metrics": baseline_result.metrics.to_dict(),
        "combined_metrics": combined_result.metrics.to_dict(),
        "stressed_combined_metrics": stressed_result.metrics.to_dict(),
        "baseline_correlation": correlation,
        "marginal_sharpe": marginal_sharpe,
        "gate": asdict(gate),
        "backtest_config": asdict(backtest_config),
        "walk_forward_config": asdict(walk_forward_config),
        "candidate_folds": tuple(candidate_result.folds.to_dicts()),
        "baseline_folds": tuple(baseline_result.folds.to_dicts()),
        "combined_folds": tuple(combined_result.folds.to_dicts()),
    }
    return CandidateDecision(
        candidate_id=candidate_id,
        baseline_id=baseline_id,
        accepted=not reasons,
        rejection_reasons=tuple(reasons),
        candidate_metrics=candidate_result.metrics.to_dict(),
        baseline_metrics=baseline_result.metrics.to_dict(),
        combined_metrics=combined_result.metrics.to_dict(),
        stressed_combined_metrics=stressed_result.metrics.to_dict(),
        baseline_correlation=correlation,
        marginal_sharpe=marginal_sharpe,
        gate=asdict(gate),
        backtest_config=asdict(backtest_config),
        walk_forward_config=asdict(walk_forward_config),
        candidate_folds=tuple(candidate_result.folds.to_dicts()),
        baseline_folds=tuple(baseline_result.folds.to_dicts()),
        combined_folds=tuple(combined_result.folds.to_dicts()),
        evidence_sha256=_evidence_digest(payload),
    )
