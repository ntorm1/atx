"""Chronological expanding-window out-of-sample evaluation."""

from __future__ import annotations

from dataclasses import dataclass

import polars as pl

from .backtest import BacktestResult, run_weight_backtest
from .config import BacktestConfig, WalkForwardConfig
from .metrics import PerformanceMetrics, compute_performance_metrics
from .portfolio import build_target_weights
from .schema import validate_panel


@dataclass(frozen=True)
class WalkForwardResult:
    folds: pl.DataFrame
    periods: pl.DataFrame
    metrics: PerformanceMetrics


def _fold_bounds(
    dates: list[object],
    config: WalkForwardConfig,
) -> list[tuple[int, int, int, int]]:
    if config.step_periods < config.test_periods:
        raise ValueError(
            "step_periods must be at least test_periods to avoid overlapping OOS folds"
        )
    bounds: list[tuple[int, int, int, int]] = []
    test_start = config.minimum_train_periods + config.embargo_periods
    while test_start + config.test_periods <= len(dates):
        train_end = test_start - config.embargo_periods
        bounds.append((0, train_end, test_start, test_start + config.test_periods))
        test_start += config.step_periods
    if len(bounds) < config.minimum_folds:
        raise ValueError(
            f"walk-forward produced {len(bounds)} folds, below minimum {config.minimum_folds}"
        )
    return bounds


def walk_forward_weight_backtest(
    panel: pl.DataFrame,
    weights: pl.DataFrame,
    backtest_config: BacktestConfig | None = None,
    walk_forward_config: WalkForwardConfig | None = None,
) -> WalkForwardResult:
    """Evaluate fixed construction rules on disjoint chronological OOS folds."""

    backtest_config = backtest_config or BacktestConfig()
    walk_forward_config = walk_forward_config or WalkForwardConfig()
    validated = validate_panel(panel)
    dates = validated.get_column("date").unique().sort().to_list()
    bounds = _fold_bounds(dates, walk_forward_config)
    fold_rows: list[dict[str, object]] = []
    period_parts: list[pl.DataFrame] = []
    for fold_index, (train_start, train_end, test_start, test_end) in enumerate(bounds):
        test_dates = dates[test_start:test_end]
        test_panel = validated.filter(pl.col("date").is_in(test_dates))
        test_weights = weights.filter(pl.col("date").is_in(test_dates))
        result: BacktestResult = run_weight_backtest(
            test_panel,
            test_weights,
            backtest_config,
        )
        fold_rows.append(
            {
                "fold": fold_index,
                "train_start": dates[train_start],
                "train_end": dates[train_end - 1],
                "test_start": test_dates[0],
                "test_end": test_dates[-1],
                "observations": result.metrics.observations,
                "net_return": result.metrics.total_return,
                "sharpe": result.metrics.sharpe,
                "max_drawdown": result.metrics.max_drawdown,
                "turnover": result.metrics.average_turnover,
            }
        )
        period_parts.append(result.periods.with_columns(pl.lit(fold_index).alias("fold")))
    periods = pl.concat(period_parts).sort("date")
    if periods.get_column("date").n_unique() != periods.height:
        raise ValueError("walk-forward OOS folds overlap")
    metrics = compute_performance_metrics(
        periods,
        trial_count=backtest_config.trial_count,
        trial_sharpe_std=backtest_config.trial_sharpe_std,
    )
    return WalkForwardResult(
        folds=pl.DataFrame(fold_rows).sort("fold"),
        periods=periods,
        metrics=metrics,
    )


def walk_forward_backtest(
    panel: pl.DataFrame,
    backtest_config: BacktestConfig | None = None,
    walk_forward_config: WalkForwardConfig | None = None,
) -> WalkForwardResult:
    backtest_config = backtest_config or BacktestConfig()
    validated = validate_panel(panel)
    weights = build_target_weights(
        validated,
        backtest_config.portfolio,
        costs=backtest_config.costs,
    )
    return walk_forward_weight_backtest(
        validated,
        weights,
        backtest_config,
        walk_forward_config,
    )
