"""Governed incumbent replacement challenges for correlated strategies."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass

import polars as pl

from .config import BacktestConfig, WalkForwardConfig
from .evidence import evidence_digest, json_safe
from .portfolio import build_target_weights
from .schema import validate_panel
from .walk_forward import walk_forward_weight_backtest


@dataclass(frozen=True)
class ReplacementGate:
    min_challenger_oos_sharpe: float = 0.50
    min_deflated_sharpe_probability: float = 0.95
    min_sharpe_improvement: float = 0.05
    min_cost_stress_sharpe: float = 0.0
    max_turnover: float = 0.70
    max_drawdown: float = 0.25
    min_gross_deployment: float = 0.95

    def __post_init__(self) -> None:
        if not 0 <= self.min_deflated_sharpe_probability <= 1:
            raise ValueError("min_deflated_sharpe_probability must be in [0, 1]")
        if self.max_turnover <= 0 or not 0 < self.max_drawdown <= 1:
            raise ValueError("turnover and drawdown limits must be positive")
        if not 0 < self.min_gross_deployment <= 1:
            raise ValueError("min_gross_deployment must be in (0, 1]")


@dataclass(frozen=True)
class ReplacementDecision:
    challenger_id: str
    incumbent_id: str
    accepted: bool
    rejection_reasons: tuple[str, ...]
    challenger_metrics: dict[str, int | float | None]
    incumbent_metrics: dict[str, int | float | None]
    stressed_challenger_metrics: dict[str, int | float | None]
    sharpe_improvement: float | None
    evaluation_status: str
    skipped_stages: tuple[str, ...]
    gate: dict[str, int | float]
    backtest_config: dict[str, object]
    walk_forward_config: dict[str, object]
    challenger_folds: tuple[dict[str, object], ...]
    incumbent_folds: tuple[dict[str, object], ...]
    evidence_sha256: str

    def to_dict(self) -> dict[str, object]:
        payload = json_safe(asdict(self))
        if not isinstance(payload, dict):
            raise TypeError("replacement decision did not serialize to an object")
        return payload

    def to_json(self, *, indent: int = 2) -> str:
        return json.dumps(self.to_dict(), sort_keys=True, indent=indent, allow_nan=False)


def evaluate_replacement(
    challenger_panel: pl.DataFrame,
    incumbent_panel: pl.DataFrame,
    *,
    challenger_id: str,
    incumbent_id: str,
    backtest_config: BacktestConfig | None = None,
    walk_forward_config: WalkForwardConfig | None = None,
    gate: ReplacementGate | None = None,
) -> ReplacementDecision:
    """Decide whether a challenger should replace the production incumbent."""

    backtest_config = backtest_config or BacktestConfig()
    walk_forward_config = walk_forward_config or WalkForwardConfig()
    gate = gate or ReplacementGate()
    challenger = validate_panel(challenger_panel)
    incumbent = validate_panel(incumbent_panel)
    common_dates = challenger.select("date").unique().join(
        incumbent.select("date").unique(), on="date", how="inner"
    )
    if common_dates.is_empty():
        raise ValueError("challenger and incumbent have no common formation dates")
    challenger = challenger.join(common_dates, on="date", how="semi")
    incumbent = incumbent.join(common_dates, on="date", how="semi")
    challenger_weights = build_target_weights(
        challenger, backtest_config.portfolio, costs=backtest_config.costs
    )
    challenger_result = walk_forward_weight_backtest(
        challenger,
        challenger_weights,
        backtest_config,
        walk_forward_config,
    )
    challenger_metrics = challenger_result.metrics
    deployment = (
        challenger_metrics.minimum_gross_exposure
        / backtest_config.portfolio.gross_leverage
    )
    reasons: list[str] = []
    if challenger_metrics.sharpe < gate.min_challenger_oos_sharpe:
        reasons.append("challenger_oos_sharpe_below_floor")
    if (
        challenger_metrics.deflated_sharpe_probability
        < gate.min_deflated_sharpe_probability
    ):
        reasons.append("challenger_deflated_sharpe_below_floor")
    if challenger_metrics.average_turnover > gate.max_turnover:
        reasons.append("challenger_turnover_above_ceiling")
    if abs(challenger_metrics.max_drawdown) > gate.max_drawdown:
        reasons.append("challenger_drawdown_above_ceiling")
    if deployment < gate.min_gross_deployment:
        reasons.append("challenger_gross_deployment_below_floor")
    if challenger_metrics.max_participation > backtest_config.costs.max_participation:
        reasons.append("challenger_participation_above_ceiling")

    incumbent_result = None
    stressed_result = None
    sharpe_improvement = None
    skipped_stages: tuple[str, ...] = ()
    evaluation_status = "complete"
    if reasons:
        evaluation_status = "primary_rejected"
        skipped_stages = ("incumbent_comparison", "cost_stress")
    else:
        incumbent_weights = build_target_weights(
            incumbent, backtest_config.portfolio, costs=backtest_config.costs
        )
        incumbent_result = walk_forward_weight_backtest(
            incumbent,
            incumbent_weights,
            backtest_config,
            walk_forward_config,
        )
        sharpe_improvement = (
            challenger_metrics.sharpe - incumbent_result.metrics.sharpe
        )
        if sharpe_improvement < gate.min_sharpe_improvement:
            reasons.append("challenger_sharpe_improvement_below_floor")
            evaluation_status = "comparison_rejected"
            skipped_stages = ("cost_stress",)
        else:
            stressed_config = BacktestConfig(
                portfolio=backtest_config.portfolio,
                costs=backtest_config.costs.stressed(),
                holding_period_days=backtest_config.holding_period_days,
                trial_count=backtest_config.trial_count,
                trial_sharpe_std=backtest_config.trial_sharpe_std,
            )
            stressed_result = walk_forward_weight_backtest(
                challenger,
                challenger_weights,
                stressed_config,
                walk_forward_config,
            )
            if stressed_result.metrics.sharpe <= gate.min_cost_stress_sharpe:
                reasons.append("challenger_cost_stress_sharpe_below_floor")
                evaluation_status = "stress_rejected"

    payload: dict[str, object] = {
        "challenger_id": challenger_id,
        "incumbent_id": incumbent_id,
        "accepted": not reasons,
        "rejection_reasons": tuple(reasons),
        "challenger_metrics": challenger_metrics.to_dict(),
        "incumbent_metrics": (
            incumbent_result.metrics.to_dict() if incumbent_result is not None else {}
        ),
        "stressed_challenger_metrics": (
            stressed_result.metrics.to_dict() if stressed_result is not None else {}
        ),
        "sharpe_improvement": sharpe_improvement,
        "evaluation_status": evaluation_status,
        "skipped_stages": skipped_stages,
        "gate": asdict(gate),
        "backtest_config": asdict(backtest_config),
        "walk_forward_config": asdict(walk_forward_config),
        "challenger_folds": tuple(challenger_result.folds.to_dicts()),
        "incumbent_folds": (
            tuple(incumbent_result.folds.to_dicts())
            if incumbent_result is not None
            else ()
        ),
    }
    return ReplacementDecision(
        **payload,
        evidence_sha256=evidence_digest(payload),
    )
