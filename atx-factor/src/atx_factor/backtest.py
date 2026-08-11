"""Vectorized long/short portfolio simulation over Polars frames."""

from __future__ import annotations

from dataclasses import dataclass

import polars as pl

from .config import BacktestConfig, CostModel
from .metrics import PerformanceMetrics, compute_performance_metrics
from .portfolio import build_target_weights
from .schema import validate_panel


@dataclass(frozen=True)
class BacktestResult:
    positions: pl.DataFrame
    trades: pl.DataFrame
    periods: pl.DataFrame
    metrics: PerformanceMetrics


def _previous_weights(weights: pl.DataFrame) -> pl.DataFrame:
    dates = weights.get_column("date").unique().sort()
    if len(dates) < 2:
        return pl.DataFrame(
            schema={
                "date": pl.Date,
                "asset_id": pl.String,
                "previous_weight": pl.Float64,
                "previous_adv_usd": pl.Float64,
            }
        )
    mapping = pl.DataFrame(
        {"previous_date": dates[:-1], "date": dates[1:]},
        schema={"previous_date": pl.Date, "date": pl.Date},
    )
    expressions = [
        pl.col("date").alias("previous_date"),
        pl.col("asset_id"),
        pl.col("target_weight").alias("previous_weight"),
    ]
    if "adv_usd" in weights.columns:
        expressions.append(pl.col("adv_usd").alias("previous_adv_usd"))
    else:
        expressions.append(pl.lit(None, dtype=pl.Float64).alias("previous_adv_usd"))
    return (
        weights.select(expressions)
        .join(mapping, on="previous_date", how="inner")
        .drop("previous_date")
    )


def _build_trades(weights: pl.DataFrame, costs: CostModel) -> pl.DataFrame:
    previous = _previous_weights(weights)
    current_columns = ["date", "asset_id", "target_weight"]
    if "adv_usd" in weights.columns:
        current_columns.append("adv_usd")
    current = weights.select(current_columns)
    trades = current.join(
        previous,
        on=["date", "asset_id"],
        how="full",
        coalesce=True,
    ).with_columns(
        pl.col("target_weight").fill_null(0.0),
        pl.col("previous_weight").fill_null(0.0),
    )
    if "adv_usd" not in trades.columns:
        trades = trades.with_columns(pl.lit(None, dtype=pl.Float64).alias("adv_usd"))
    trades = trades.with_columns(
        pl.coalesce("adv_usd", "previous_adv_usd", pl.lit(costs.default_adv_usd)).alias(
            "effective_adv_usd"
        ),
        (pl.col("target_weight") - pl.col("previous_weight")).alias("weight_change"),
    ).with_columns(
        (pl.col("weight_change").abs() * costs.aum_usd / pl.col("effective_adv_usd")).alias(
            "participation"
        )
    )
    linear_bps = costs.commission_bps + costs.half_spread_bps
    return trades.with_columns(
        (pl.col("weight_change").abs() * linear_bps / 10_000.0).alias("linear_cost"),
        (
            pl.col("weight_change").abs()
            * costs.impact_bps
            / 10_000.0
            * pl.col("participation").sqrt()
        ).alias("impact_cost"),
    ).sort("date", "asset_id")


def run_weight_backtest(
    panel: pl.DataFrame,
    weights: pl.DataFrame,
    config: BacktestConfig | None = None,
) -> BacktestResult:
    """Simulate already-constructed target weights against canonical forward returns."""

    config = config or BacktestConfig()
    panel = validate_panel(panel)
    required = {"date", "asset_id", "target_weight"}
    missing = sorted(required.difference(weights.columns))
    if missing:
        raise ValueError(f"weights are missing columns: {missing}")
    positions = panel.join(
        weights.select("date", "asset_id", "target_weight"),
        on=["date", "asset_id"],
        how="inner",
        validate="1:1",
    )
    if positions.is_empty():
        raise ValueError("weights do not overlap the return panel")
    trades = _build_trades(positions, config.costs)
    if "forward_end_date" in positions.columns:
        holding_days = (pl.col("forward_end_date") - pl.col("date")).dt.total_days()
    else:
        holding_days = pl.lit(config.holding_period_days)
    default_borrow_rate = config.costs.annual_borrow_bps / 10_000.0
    borrow_rate = (
        pl.col("borrow_rate").fill_null(default_borrow_rate)
        if "borrow_rate" in positions.columns
        else pl.lit(default_borrow_rate)
    )
    position_periods = positions.with_columns(
        (pl.col("target_weight") * pl.col("forward_return")).alias("return_contribution"),
        (
            pl.col("target_weight").clip(upper_bound=0.0).abs()
            * borrow_rate
            * holding_days
            / 365.25
        ).alias("borrow_cost"),
    ).group_by("date").agg(
        pl.col("return_contribution").sum().alias("gross_return"),
        pl.col("borrow_cost").sum(),
        pl.col("target_weight").abs().sum().alias("gross_exposure"),
        pl.col("target_weight").sum().alias("net_exposure"),
        pl.len().alias("n_names"),
    )
    trade_periods = trades.group_by("date").agg(
        (pl.col("weight_change").abs().sum() / 2.0).alias("turnover"),
        pl.col("linear_cost").sum(),
        pl.col("impact_cost").sum(),
        pl.col("participation").max().alias("max_participation"),
    )
    periods = (
        position_periods.join(trade_periods, on="date", how="left")
        .with_columns(
            pl.col("turnover").fill_null(0.0),
            pl.col("linear_cost").fill_null(0.0),
            pl.col("impact_cost").fill_null(0.0),
        )
        .with_columns(
            (pl.col("linear_cost") + pl.col("impact_cost") + pl.col("borrow_cost")).alias(
                "total_cost"
            )
        )
        .with_columns((pl.col("gross_return") - pl.col("total_cost")).alias("net_return"))
        .sort("date")
        .with_columns((pl.col("net_return") + 1.0).cum_prod().alias("equity"))
    )
    metrics = compute_performance_metrics(
        periods,
        trial_count=config.trial_count,
        trial_sharpe_std=config.trial_sharpe_std,
    )
    return BacktestResult(
        positions=positions.sort("date", "asset_id"),
        trades=trades,
        periods=periods,
        metrics=metrics,
    )


def run_backtest(
    panel: pl.DataFrame,
    config: BacktestConfig | None = None,
) -> BacktestResult:
    """Validate a signal panel, construct positions, and run the costed simulation."""

    config = config or BacktestConfig()
    validated = validate_panel(panel)
    weights = build_target_weights(validated, config.portfolio, costs=config.costs)
    return run_weight_backtest(validated, weights, config)
