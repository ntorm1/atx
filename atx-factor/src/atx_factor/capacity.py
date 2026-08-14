"""Costed strategy-capacity frontiers over fixed AUM grids."""

from __future__ import annotations

import json
from collections.abc import Iterable
from dataclasses import asdict, dataclass, replace

import polars as pl

from .backtest import run_backtest
from .config import BacktestConfig, CostModel, PortfolioConfig
from .schema import validate_panel


@dataclass(frozen=True)
class CapacityPoint:
    aum_usd: float
    feasible: bool
    maximum_participation: float
    minimum_gross_deployment: float
    average_gross_deployment: float
    average_turnover: float
    annualized_cost_drag: float
    annualized_return: float
    sharpe: float
    total_return: float


@dataclass(frozen=True)
class CapacityFrontier:
    points: tuple[CapacityPoint, ...]
    maximum_feasible_aum_usd: float | None
    maximum_participation: float
    minimum_gross_deployment: float

    def to_dict(self) -> dict[str, object]:
        return {
            "maximum_feasible_aum_usd": self.maximum_feasible_aum_usd,
            "maximum_participation": self.maximum_participation,
            "minimum_gross_deployment": self.minimum_gross_deployment,
            "points": [asdict(point) for point in self.points],
        }

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), indent=2, sort_keys=True, allow_nan=False)


def evaluate_capacity_frontier(
    panel: pl.DataFrame,
    aum_grid_usd: Iterable[float],
    *,
    portfolio: PortfolioConfig | None = None,
    costs: CostModel | None = None,
    holding_period_days: int = 21,
    maximum_participation: float = 0.10,
    minimum_gross_deployment: float = 0.95,
) -> CapacityFrontier:
    """Evaluate execution feasibility at every predeclared AUM point."""

    aum_values = tuple(sorted(float(value) for value in aum_grid_usd))
    if not aum_values or any(value <= 0 for value in aum_values):
        raise ValueError("aum_grid_usd must contain positive values")
    if len(set(aum_values)) != len(aum_values):
        raise ValueError("aum_grid_usd must not contain duplicates")
    if not 0 < maximum_participation <= 1:
        raise ValueError("maximum_participation must be in (0, 1]")
    if not 0 < minimum_gross_deployment <= 1:
        raise ValueError("minimum_gross_deployment must be in (0, 1]")

    validated = validate_panel(panel)
    portfolio = portfolio or PortfolioConfig()
    costs = costs or CostModel()
    points: list[CapacityPoint] = []
    for aum_usd in aum_values:
        result = run_backtest(
            validated,
            BacktestConfig(
                portfolio=portfolio,
                costs=replace(costs, aum_usd=aum_usd),
                holding_period_days=holding_period_days,
            ),
        )
        metrics = result.metrics
        minimum_deployment = metrics.minimum_gross_exposure / portfolio.gross_leverage
        average_deployment = metrics.average_gross_exposure / portfolio.gross_leverage
        feasible = (
            metrics.max_participation <= maximum_participation
            and minimum_deployment >= minimum_gross_deployment
        )
        points.append(
            CapacityPoint(
                aum_usd=aum_usd,
                feasible=feasible,
                maximum_participation=metrics.max_participation,
                minimum_gross_deployment=minimum_deployment,
                average_gross_deployment=average_deployment,
                average_turnover=metrics.average_turnover,
                annualized_cost_drag=metrics.annualized_cost_drag,
                annualized_return=metrics.annualized_return,
                sharpe=metrics.sharpe,
                total_return=metrics.total_return,
            )
        )
    feasible_aum = [point.aum_usd for point in points if point.feasible]
    return CapacityFrontier(
        points=tuple(points),
        maximum_feasible_aum_usd=max(feasible_aum, default=None),
        maximum_participation=maximum_participation,
        minimum_gross_deployment=minimum_gross_deployment,
    )
