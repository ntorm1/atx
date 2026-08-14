"""Immutable configuration contracts for portfolio research."""

from __future__ import annotations

from dataclasses import dataclass, field, replace
from typing import Literal


@dataclass(frozen=True)
class PortfolioConfig:
    """Cross-sectional signal-to-position policy."""

    gross_leverage: float = 1.0
    name_cap: float = 0.05
    minimum_names: int = 20
    rank_signal: bool = True
    dollar_neutral: bool = True
    neutralize_column: str | None = None
    tail_fraction: float | None = None
    construction: Literal[
        "symmetric", "long_tail_vs_universe", "universe_vs_short_tail"
    ] = "symmetric"

    def __post_init__(self) -> None:
        if self.gross_leverage <= 0:
            raise ValueError("gross_leverage must be positive")
        if not 0 < self.name_cap <= self.gross_leverage:
            raise ValueError("name_cap must be in (0, gross_leverage]")
        if self.minimum_names < 2:
            raise ValueError("minimum_names must be at least 2")
        if self.tail_fraction is not None and not 0 < self.tail_fraction <= 0.5:
            raise ValueError("tail_fraction must be in (0, 0.5] when supplied")
        if self.construction != "symmetric" and self.tail_fraction is None:
            raise ValueError("asymmetric constructions require tail_fraction")


@dataclass(frozen=True)
class CostModel:
    """One-way trading, participation impact, and short-borrow assumptions."""

    commission_bps: float = 0.25
    half_spread_bps: float = 2.0
    impact_bps: float = 10.0
    aum_usd: float = 100_000_000.0
    default_adv_usd: float = 100_000_000.0
    annual_borrow_bps: float = 50.0
    max_participation: float = 0.10
    position_participation_fraction: float = 0.25

    def __post_init__(self) -> None:
        nonnegative = (
            self.commission_bps,
            self.half_spread_bps,
            self.impact_bps,
            self.annual_borrow_bps,
        )
        if any(value < 0 for value in nonnegative):
            raise ValueError("cost rates must be non-negative")
        if self.aum_usd <= 0 or self.default_adv_usd <= 0:
            raise ValueError("aum_usd and default_adv_usd must be positive")
        if not 0 < self.max_participation <= 1:
            raise ValueError("max_participation must be in (0, 1]")
        if not 0 < self.position_participation_fraction <= 0.5:
            raise ValueError("position_participation_fraction must be in (0, 0.5]")

    def stressed(self, multiplier: float = 2.0) -> CostModel:
        if multiplier < 1:
            raise ValueError("cost stress multiplier must be at least 1")
        return replace(
            self,
            commission_bps=self.commission_bps * multiplier,
            half_spread_bps=self.half_spread_bps * multiplier,
            impact_bps=self.impact_bps * multiplier,
            annual_borrow_bps=self.annual_borrow_bps * multiplier,
        )


@dataclass(frozen=True)
class BacktestConfig:
    portfolio: PortfolioConfig = field(default_factory=PortfolioConfig)
    costs: CostModel = field(default_factory=CostModel)
    holding_period_days: int = 21
    trial_count: int = 1
    trial_sharpe_std: float | None = None

    def __post_init__(self) -> None:
        if self.holding_period_days < 1:
            raise ValueError("holding_period_days must be positive")
        if self.trial_count < 1:
            raise ValueError("trial_count must be positive")
        if self.trial_sharpe_std is not None and self.trial_sharpe_std <= 0:
            raise ValueError("trial_sharpe_std must be positive when supplied")


@dataclass(frozen=True)
class WalkForwardConfig:
    minimum_train_periods: int = 60
    test_periods: int = 12
    step_periods: int = 12
    embargo_periods: int = 1
    minimum_folds: int = 3

    def __post_init__(self) -> None:
        if self.minimum_train_periods < 1 or self.test_periods < 1 or self.step_periods < 1:
            raise ValueError("walk-forward window sizes must be positive")
        if self.embargo_periods < 0:
            raise ValueError("embargo_periods must be non-negative")
        if self.minimum_folds < 1:
            raise ValueError("minimum_folds must be positive")
