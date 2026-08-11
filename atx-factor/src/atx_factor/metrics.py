"""Portfolio performance and multiple-testing-aware inference."""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass
from statistics import NormalDist

import numpy as np
import polars as pl


@dataclass(frozen=True)
class PerformanceMetrics:
    observations: int
    periods_per_year: float
    total_return: float
    annualized_return: float
    annualized_volatility: float
    sharpe: float
    sortino: float
    max_drawdown: float
    hit_rate: float
    average_turnover: float
    max_participation: float
    average_gross_exposure: float
    minimum_gross_exposure: float
    annualized_cost_drag: float
    fitness: float
    probabilistic_sharpe: float
    deflated_sharpe_probability: float
    deflated_sharpe_benchmark: float
    skewness: float
    excess_kurtosis: float

    def to_dict(self) -> dict[str, int | float | None]:
        return {
            key: None if isinstance(value, float) and not math.isfinite(value) else value
            for key, value in asdict(self).items()
        }


def _normal_cdf(value: float) -> float:
    return 0.5 * (1.0 + math.erf(value / math.sqrt(2.0)))


def infer_periods_per_year(dates: np.ndarray) -> float:
    if dates.size < 2:
        return 1.0
    days = dates.astype("datetime64[D]").astype(np.int64)
    gaps = np.diff(days)
    positive = gaps[gaps > 0]
    if positive.size == 0:
        return 1.0
    return float(365.25 / np.median(positive))


def expected_max_sharpe(
    *,
    n_trials: int,
    sharpe_std: float,
    sharpe_mean: float = 0.0,
) -> float:
    """Bailey/Lopez de Prado expected maximum Sharpe approximation."""

    if n_trials <= 1:
        return float(sharpe_mean)
    gamma = 0.5772156649015329
    normal = NormalDist()
    z1 = normal.inv_cdf(1.0 - 1.0 / n_trials)
    z2 = normal.inv_cdf(1.0 - 1.0 / (n_trials * math.e))
    return float(sharpe_mean + sharpe_std * ((1.0 - gamma) * z1 + gamma * z2))


def probabilistic_sharpe_ratio(
    periodic_sharpe: float,
    benchmark_periodic_sharpe: float,
    *,
    observations: int,
    skewness: float,
    kurtosis: float,
) -> float:
    if observations < 2 or not math.isfinite(periodic_sharpe):
        return float("nan")
    variance_term = (
        1.0
        - skewness * periodic_sharpe
        + ((kurtosis - 1.0) / 4.0) * periodic_sharpe**2
    )
    if variance_term <= 0:
        return float("nan")
    z_value = (
        (periodic_sharpe - benchmark_periodic_sharpe)
        * math.sqrt(observations - 1)
        / math.sqrt(variance_term)
    )
    return _normal_cdf(z_value)


def compute_performance_metrics(
    periods: pl.DataFrame,
    *,
    return_column: str = "net_return",
    trial_count: int = 1,
    trial_sharpe_std: float | None = None,
) -> PerformanceMetrics:
    required = {"date", return_column}
    missing = sorted(required.difference(periods.columns))
    if missing:
        raise ValueError(f"period frame is missing metric columns: {missing}")
    ordered = periods.sort("date")
    values = ordered.get_column(return_column).to_numpy().astype(np.float64, copy=False)
    if values.size < 2 or not np.isfinite(values).all():
        raise ValueError("at least two finite period returns are required")
    if np.any(values <= -1.0):
        raise ValueError("period returns must be greater than -100%")
    dates = ordered.get_column("date").to_numpy()
    periods_per_year = infer_periods_per_year(dates)
    observations = int(values.size)
    mean = float(values.mean())
    volatility = float(values.std(ddof=1))
    downside = values[values < 0]
    downside_volatility = float(downside.std(ddof=1)) if downside.size > 1 else float("nan")
    sharpe = mean / volatility * math.sqrt(periods_per_year) if volatility > 0 else float("nan")
    sortino = (
        mean / downside_volatility * math.sqrt(periods_per_year)
        if downside_volatility > 0
        else float("nan")
    )
    wealth = np.cumprod(1.0 + values)
    running_peak = np.maximum.accumulate(wealth)
    drawdown = wealth / running_peak - 1.0
    total_return = float(wealth[-1] - 1.0)
    annualized_return = float(wealth[-1] ** (periods_per_year / observations) - 1.0)
    annualized_volatility = float(volatility * math.sqrt(periods_per_year))
    average_turnover = (
        float(ordered.get_column("turnover").mean())
        if "turnover" in ordered.columns
        else float("nan")
    )
    max_participation = (
        float(ordered.get_column("max_participation").max())
        if "max_participation" in ordered.columns
        else float("nan")
    )
    average_gross_exposure = (
        float(ordered.get_column("gross_exposure").mean())
        if "gross_exposure" in ordered.columns
        else float("nan")
    )
    minimum_gross_exposure = (
        float(ordered.get_column("gross_exposure").min())
        if "gross_exposure" in ordered.columns
        else float("nan")
    )
    period_cost = (
        ordered.get_column("total_cost").to_numpy().astype(np.float64, copy=False)
        if "total_cost" in ordered.columns
        else np.zeros_like(values)
    )
    annualized_cost_drag = float(period_cost.mean() * periods_per_year)
    centered = values - mean
    population_std = float(values.std(ddof=0))
    if population_std > 0:
        skewness = float(np.mean((centered / population_std) ** 3))
        kurtosis = float(np.mean((centered / population_std) ** 4))
    else:
        skewness = float("nan")
        kurtosis = float("nan")
    periodic_sharpe = mean / volatility if volatility > 0 else float("nan")
    psr = probabilistic_sharpe_ratio(
        periodic_sharpe,
        0.0,
        observations=observations,
        skewness=skewness,
        kurtosis=kurtosis,
    )
    default_trial_std = math.sqrt(periods_per_year / observations)
    benchmark_annual = expected_max_sharpe(
        n_trials=trial_count,
        sharpe_std=trial_sharpe_std or default_trial_std,
    )
    dsr = probabilistic_sharpe_ratio(
        periodic_sharpe,
        benchmark_annual / math.sqrt(periods_per_year),
        observations=observations,
        skewness=skewness,
        kurtosis=kurtosis,
    )
    fitness = (
        sharpe
        * math.sqrt(abs(annualized_return) / max(average_turnover, 0.125))
        if math.isfinite(sharpe) and math.isfinite(average_turnover)
        else float("nan")
    )
    return PerformanceMetrics(
        observations=observations,
        periods_per_year=periods_per_year,
        total_return=total_return,
        annualized_return=annualized_return,
        annualized_volatility=annualized_volatility,
        sharpe=float(sharpe),
        sortino=float(sortino),
        max_drawdown=float(drawdown.min()),
        hit_rate=float(np.mean(values > 0)),
        average_turnover=average_turnover,
        max_participation=max_participation,
        average_gross_exposure=average_gross_exposure,
        minimum_gross_exposure=minimum_gross_exposure,
        annualized_cost_drag=annualized_cost_drag,
        fitness=float(fitness),
        probabilistic_sharpe=float(psr),
        deflated_sharpe_probability=float(dsr),
        deflated_sharpe_benchmark=benchmark_annual,
        skewness=skewness,
        excess_kurtosis=float(kurtosis - 3.0),
    )
