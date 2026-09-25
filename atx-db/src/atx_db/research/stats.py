"""Research statistics core (R3a): the single home for research inference.

Every function is pure and deterministic (numpy/pandas in, plain values out, no I/O).

Conventions
-----------
* **Formation units.** A series is tested in the units it is sampled in (one step per
  monthly formation for research). An ``h``-unit forward label sampled every formation
  overlaps its next ``h-1`` neighbours, so its per-formation statistic series is MA(h-1)
  even under the null. ``horizon_periods`` is always ``h`` in formation units:
  1/3/6/12 for monthly 21/63/126/252-session labels. Use
  :func:`horizon_in_formation_units` when only a session horizon and dates are known.
* **Missing formations.** Array inputs may carry ``NaN`` for a formation without a
  statistic. HAC sums keep calendar positions (a missing formation contributes zero,
  exact lags are preserved, gaps are never compressed); the mean is over observed values.
* **Two inference paths for a mean.**
  - ``nw_*``: Newey-West (Bartlett) with ``lags = max(h-1, floor(4 (T/100)^(2/9)))`` and a
    normal reference. This is the plan-R3a rule, reported for comparability.
  - ``robust_*``: equal-weighted-cosine (EWC) long-run variance with
    ``B = min(floor(0.4 T^(2/3)), floor(T / (2h)))`` cosine terms and Student-t(B)
    reference (fixed-b inference; Lazarus, Lewis, Stock & Watson 2018, "HAR Inference:
    Recommendations for Practice", JBES 36(4)), with the ``T/(2h)`` cap keeping the cosine
    band below the overlap-induced spectral dip at ``2*pi/h``.
  Monte Carlo on ``T=160`` monthly formations of overlapping h-month sums of iid
  returns (R3a report): nominal-5% NW rejects 5.5% / 8.8% / 13.2% / 16.4% at h=1/3/6/12 and
  ``|t|>=3`` rejects 0.5% / 1.1% / 2.4% / 4.4% (nominal 0.27%), because Bartlett weights
  with ``L=h-1`` capture only about 2/3 of an overlap's long-run variance and the variance
  estimate itself is noisy with ~T/h independent observations. EWC rejects
  4.5-6.5% / 0.13-0.33% over the same grid. **Significance decisions (HLZ, BH) should use
  the robust p-value / ``z_equivalent``.**
* **HLZ.** Harvey, Liu & Zhu (2016, RFS 29(1)) recommend ``|t| >= 3.0`` for a new factor.
  For a small-df test the threshold is applied to the normal-equivalent z of its p-value
  (two-sided ``p <= 0.0027``).
"""

from __future__ import annotations

import math
from collections.abc import Hashable, Iterable, Mapping, Sequence
from dataclasses import asdict, dataclass
from functools import lru_cache
from statistics import NormalDist
from typing import Any

import numpy as np
import pandas as pd

HLZ_T_THRESHOLD = 3.0
HLZ_P_THRESHOLD = math.erfc(HLZ_T_THRESHOLD / math.sqrt(2.0))
EULER_MASCHERONI = 0.5772156649015329
DEFAULT_BOOTSTRAP_SEED = 20260925
_Z975 = 1.959963984540054
_NORMAL = NormalDist()


# ---------------------------------------------------------------------------
# Reference distributions (scipy is not a dependency)
# ---------------------------------------------------------------------------

def normal_two_sided_p(z: float) -> float:
    """Two-sided normal p-value ``P(|Z| >= |z|)``."""
    if math.isnan(z):
        return float("nan")
    return math.erfc(abs(z) / math.sqrt(2.0))


def normal_equivalent_z(p_value: float) -> float:
    """``|z|`` whose two-sided normal p-value equals ``p_value`` (``inf`` at 0)."""
    if math.isnan(p_value):
        return float("nan")
    if not 0.0 <= p_value <= 1.0:
        raise ValueError("p_value must be within [0, 1]")
    if p_value == 0.0:
        return math.inf
    return -_NORMAL.inv_cdf(p_value / 2.0)


def _beta_continued_fraction(a: float, b: float, x: float) -> float:
    """Lentz evaluation of the incomplete-beta continued fraction."""
    tiny = 1e-300
    qab, qap, qam = a + b, a + 1.0, a - 1.0
    c = 1.0
    d = 1.0 - qab * x / qap
    d = 1.0 / (d if abs(d) >= tiny else tiny)
    result = d
    for m in range(1, 20_000):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) >= tiny else tiny)
        c = 1.0 + aa / c
        c = c if abs(c) >= tiny else tiny
        result *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) >= tiny else tiny)
        c = 1.0 + aa / c
        c = c if abs(c) >= tiny else tiny
        delta = d * c
        result *= delta
        if abs(delta - 1.0) < 1e-16:
            return result
    raise ArithmeticError("incomplete beta continued fraction did not converge")


def _regularized_incomplete_beta(a: float, b: float, x: float) -> float:
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    log_front = (math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
                 + a * math.log(x) + b * math.log1p(-x))
    if x < (a + 1.0) / (a + b + 2.0):
        return math.exp(log_front) * _beta_continued_fraction(a, b, x) / a
    return 1.0 - math.exp(log_front) * _beta_continued_fraction(b, a, 1.0 - x) / b


def student_t_two_sided_p(t: float, df: float) -> float:
    """Two-sided Student-t p-value ``P(|T_df| >= |t|) = I_{df/(df+t^2)}(df/2, 1/2)``."""
    if not df > 0:
        raise ValueError("df must be positive")
    if math.isnan(t):
        return float("nan")
    if math.isinf(t):
        return 0.0
    return _regularized_incomplete_beta(df / 2.0, 0.5, df / (df + t * t))


@lru_cache(maxsize=512)
def student_t_critical(p_two_sided: float, df: float) -> float:
    """``c > 0`` with ``P(|T_df| >= c) = p_two_sided`` (bisection to machine precision)."""
    if not 0.0 < p_two_sided < 1.0:
        raise ValueError("p_two_sided must be within (0, 1)")
    low, high = 0.0, 1.0
    while student_t_two_sided_p(high, df) > p_two_sided:
        low, high = high, high * 2.0
    for _ in range(200):
        middle = 0.5 * (low + high)
        if middle in (low, high):
            break
        if student_t_two_sided_p(middle, df) > p_two_sided:
            low = middle
        else:
            high = middle
    return 0.5 * (low + high)


# ---------------------------------------------------------------------------
# Lag / bandwidth rules
# ---------------------------------------------------------------------------

def newey_west_lags(horizon_periods: int, n_obs: int) -> int:
    """``max(h-1, floor(4 (T/100)^(2/9)))`` in formation units, capped at ``T-1``.

    ``h-1`` covers the overlap of ``h``-unit labels sampled every unit; the second term
    is the Newey-West (1994) plug-in rate for the remaining serial correlation.
    """
    if int(horizon_periods) != horizon_periods or horizon_periods < 1:
        raise ValueError("horizon_periods must be a positive integer")
    if n_obs < 2:
        return 0
    rule = math.floor(4.0 * (n_obs / 100.0) ** (2.0 / 9.0))
    return int(min(max(int(horizon_periods) - 1, rule), n_obs - 1))


def ewc_degrees_of_freedom(n_obs: int, horizon_periods: int) -> int:
    """EWC cosine terms ``B = min(floor(0.4 T^(2/3)), floor(T/(2h)))`` (0 = untestable)."""
    if int(horizon_periods) != horizon_periods or horizon_periods < 1:
        raise ValueError("horizon_periods must be a positive integer")
    if n_obs < 2:
        return 0
    return int(max(0, min(math.floor(0.4 * n_obs ** (2.0 / 3.0)), n_obs // (2 * int(horizon_periods)))))


def horizon_in_formation_units(formation_dates: Iterable[Any], horizon_sessions: int) -> int:
    """Convert a session horizon into formation units from the observed formation dates.

    Returns ``1 +`` the largest number of later formations that start inside one label's
    window ``(d_i, d_i + h sessions)``; daily sessions give ``h``, month-end formations give
    about ``ceil(h/21)``. Sessions are approximated by weekdays (``numpy.busday_count``):
    an exchange holiday inside a window can undercount that window's overlap by one.
    """
    if int(horizon_sessions) != horizon_sessions or horizon_sessions < 1:
        raise ValueError("horizon_sessions must be a positive integer")
    days = np.unique(pd.to_datetime(pd.Series(list(formation_dates))).dropna()
                     .dt.normalize().to_numpy().astype("datetime64[D]"))
    if len(days) < 2:
        return 1
    offsets = np.busday_count(days[0], days, weekmask="1111100")
    starts_before_end = np.searchsorted(offsets, offsets + int(horizon_sessions), side="left")
    overlaps = starts_before_end - np.arange(1, len(days) + 1)
    return int(max(int(overlaps.max()), 0)) + 1


# ---------------------------------------------------------------------------
# Long-run variance of a mean
# ---------------------------------------------------------------------------

def _as_positioned(values: Iterable[float] | np.ndarray | pd.Series) -> np.ndarray:
    """Float array in formation order; non-finite = missing; outer missing trimmed."""
    array = pd.to_numeric(pd.Series(values if not isinstance(values, np.ndarray) else values.ravel()),
                          errors="coerce").to_numpy(dtype=float)
    array = np.where(np.isfinite(array), array, np.nan)
    observed = np.flatnonzero(~np.isnan(array))
    if not len(observed):
        return array[:0]
    return array[observed[0]:observed[-1] + 1]


def _bartlett_long_run_sum(centered: np.ndarray, lags: int) -> float:
    """``sum_t sum_s w(|t-s|) u_t u_s`` with Bartlett weights; zeros mark missing."""
    total = float(np.dot(centered, centered))
    for lag in range(1, lags + 1):
        total += 2.0 * (1.0 - lag / (lags + 1.0)) * float(np.dot(centered[lag:], centered[:-lag]))
    return total


def _ewc_basis(span: int, df: int) -> np.ndarray:
    positions = np.arange(1, span + 1, dtype=float) - 0.5
    return math.sqrt(2.0 / span) * np.cos(np.pi * np.outer(positions, np.arange(1, df + 1)) / span)


def newey_west_mean(values: Iterable[float] | pd.Series, *, lags: int) -> tuple[int, float, float]:
    """Bartlett HAC standard error and t of the mean of a regularly spaced series.

    Missing values are dropped (the series is taken as already regular). Returns
    ``(used_lags, standard_error, t)``; ``(0, nan, nan)`` below two observations and a NaN
    t for a zero standard error. This is the former ``signal_eval`` estimator verbatim;
    only its callers' lag rule changed.
    """
    array = pd.to_numeric(pd.Series(values), errors="coerce").dropna().to_numpy(dtype=float)
    n_obs = len(array)
    if n_obs < 2:
        return 0, float("nan"), float("nan")
    used_lags = min(max(int(lags), 0), n_obs - 1)
    demeaned = array - float(array.mean())
    long_run_variance = float(np.dot(demeaned, demeaned) / n_obs)
    for lag in range(1, used_lags + 1):
        weight = 1.0 - lag / (used_lags + 1.0)
        autocovariance = float(np.dot(demeaned[lag:], demeaned[:-lag]) / n_obs)
        long_run_variance += 2.0 * weight * autocovariance
    standard_error = math.sqrt(max(long_run_variance, 0.0) / n_obs)
    tstat = float("nan") if standard_error == 0.0 else float(array.mean()) / standard_error
    return used_lags, standard_error, tstat


@dataclass(frozen=True)
class MeanInference:
    """Inference on the mean of a per-formation statistic (IC, spread, FM slope)."""

    n_obs: int
    span: int
    mean: float
    horizon_periods: int
    nw_lags: int
    nw_standard_error: float
    nw_t: float
    nw_p_value: float
    robust_df: int
    robust_standard_error: float
    robust_t: float
    robust_p_value: float
    robust_ci95_low: float
    robust_ci95_high: float
    z_equivalent: float
    hlz_pass: bool

    def as_dict(self) -> dict[str, Any]:
        return asdict(self)


def mean_inference(
    values: Iterable[float] | np.ndarray | pd.Series,
    *,
    horizon_periods: int,
    lags: int | None = None,
    robust_df: int | None = None,
) -> MeanInference:
    """Test ``E[x] = 0`` for a per-formation series of ``horizon_periods``-unit labels.

    ``values`` is in formation order; ``NaN`` marks a missing formation (kept in place).
    ``lags``/``robust_df`` override the default rules (see module docstring). ``hlz_pass``
    applies the HLZ ``|t| >= 3`` bar to the robust test's normal-equivalent z.
    """
    horizon = int(horizon_periods)
    if horizon != horizon_periods or horizon < 1:
        raise ValueError("horizon_periods must be a positive integer")
    array = _as_positioned(values)
    observed = ~np.isnan(array)
    n_obs = int(observed.sum())
    span = len(array)
    nan = float("nan")
    mean = float(array[observed].mean()) if n_obs else nan
    nw_lags = newey_west_lags(horizon, n_obs) if lags is None else min(max(int(lags), 0), max(span - 1, 0))
    df = ewc_degrees_of_freedom(n_obs, horizon) if robust_df is None else int(robust_df)
    nw_se = nw_t = nw_p = robust_se = robust_t = robust_p = low = high = z_equivalent = nan
    if n_obs >= 2:
        centered = np.where(observed, array - mean, 0.0)
        long_run = _bartlett_long_run_sum(centered, nw_lags)
        if long_run > 0.0 and math.isfinite(long_run):
            nw_se = math.sqrt(long_run) / n_obs
            nw_t = mean / nw_se
            nw_p = normal_two_sided_p(nw_t)
        if df >= 1:
            loadings = centered @ _ewc_basis(span, df)
            omega = float(np.mean(loadings * loadings))
            if omega > 0.0 and math.isfinite(omega):
                robust_se = math.sqrt(span * omega) / n_obs
                robust_t = mean / robust_se
                robust_p = student_t_two_sided_p(robust_t, df)
                critical = student_t_critical(0.05, df)
                low, high = mean - critical * robust_se, mean + critical * robust_se
                z_equivalent = math.copysign(normal_equivalent_z(robust_p), robust_t)
    return MeanInference(
        n_obs=n_obs, span=span, mean=mean, horizon_periods=horizon,
        nw_lags=nw_lags, nw_standard_error=nw_se, nw_t=nw_t, nw_p_value=nw_p,
        robust_df=df, robust_standard_error=robust_se, robust_t=robust_t,
        robust_p_value=robust_p, robust_ci95_low=low, robust_ci95_high=high,
        z_equivalent=z_equivalent,
        hlz_pass=bool(not math.isnan(z_equivalent) and abs(z_equivalent) >= HLZ_T_THRESHOLD),
    )


def calendar_hac_statistics(values: list[tuple[int, float]], horizon: int) -> dict[str, Any]:
    """Bartlett HAC for a mean, with actual session differences and no gap compression.

    Sealed CF1/FQ2 contract (moved verbatim from ``custom_features``): daily decision
    sessions, lag ``horizon-1`` sessions, asymptotic normal p. Missing observations have
    zero estimating-equation contribution. Autocovariance sums use observed pairs at
    each exact calendar lag and denominator n**2 for the variance of the observed mean.
    No significance on fewer than max(30,2*horizon) dates, a zero variance, or a
    degenerate constant series. Research inference uses :func:`mean_inference`.
    """
    data = {int(session): float(value) for session, value in values if math.isfinite(value)}
    n = len(data)
    lags = max(horizon-1, 0)
    result: dict[str, Any] = {"spread_dates": n, "gross_mean": None, "hac_lags": lags,
                              "hac_standard_error": None, "z_statistic": None,
                              "p_value": None, "ci95_low": None, "ci95_high": None}
    if not n:
        return result
    mean = math.fsum(data.values()) / n
    result["gross_mean"] = mean
    if n < max(30, 2*horizon):
        return result
    centered = {session: value-mean for session, value in data.items()}
    variance_sum = math.fsum(value*value for value in centered.values())
    if variance_sum <= 1e-24:
        return result
    for lag in range(1, lags+1):
        cross = math.fsum(value*centered.get(session-lag, 0.0) for session, value in centered.items())
        variance_sum += 2*(1-lag/(lags+1))*cross
    if variance_sum <= 0:
        return result
    standard_error = math.sqrt(variance_sum)/n
    z_statistic = mean/standard_error
    result.update(hac_standard_error=standard_error, z_statistic=z_statistic,
                  p_value=math.erfc(abs(z_statistic)/math.sqrt(2)),
                  ci95_low=mean-_Z975*standard_error,
                  ci95_high=mean+_Z975*standard_error)
    return result


# ---------------------------------------------------------------------------
# Multiple testing
# ---------------------------------------------------------------------------

def _checked(p: float | None) -> float | None:
    if p is not None and (not math.isfinite(p) or not 0 <= p <= 1):
        raise ValueError("p-values must be finite and within [0,1]")
    return p


def holm[K: Hashable](p_values: Mapping[K, float | None]) -> dict[K, float | None]:
    """Holm step-down adjusted p-values over the whole frozen family.

    ``None`` = untestable hypothesis: it stays in the family (as p=1, so it enlarges the
    multiplier of every tested hypothesis) and its adjusted value stays ``None``. Keys
    are returned in ascending (p, key) order.
    """
    ordered = sorted(p_values, key=lambda key: (1 if p_values[key] is None else p_values[key], key))
    result: dict[K, float | None] = {}
    running = 0.0
    for index, key in enumerate(ordered):
        p = _checked(p_values[key])
        running = max(running, (len(ordered) - index) * (1.0 if p is None else p))
        result[key] = None if p is None else min(1.0, running)
    return result


def benjamini_hochberg[K: Hashable](
    p_values: Mapping[K, float | None], *, arbitrary_dependence: bool = False,
) -> dict[K, float | None]:
    """Benjamini-Hochberg (1995) q-values (BH-adjusted p-values) over the whole family.

    ``q_(i) = min_{j>=i} min(1, m c(m) p_(j) / j)`` with ``c(m)=1`` (independent or
    positively dependent tests) or ``c(m)=sum 1/k`` (Benjamini-Yekutieli 2001, arbitrary
    dependence). Rejecting ``q <= alpha`` is the BH step-up rule at FDR ``alpha``.
    ``None`` = untestable: counted in ``m`` as p=1, returned as ``None``.
    """
    keys = list(p_values)
    m = len(keys)
    if not m:
        return {}
    checked = [_checked(p_values[key]) for key in keys]
    order = sorted(range(m), key=lambda index: (1.0 if checked[index] is None else checked[index], index))
    factor = math.fsum(1.0 / k for k in range(1, m + 1)) if arbitrary_dependence else 1.0
    adjusted = [0.0] * m
    running = 1.0
    for rank in range(m, 0, -1):
        index = order[rank - 1]
        value = checked[index]
        p = 1.0 if value is None else value
        running = min(running, m * factor * p / rank)
        adjusted[index] = min(1.0, running)
    return {key: None if checked[index] is None else adjusted[index] for index, key in enumerate(keys)}


def hlz_pass(statistic: float | None, threshold: float = HLZ_T_THRESHOLD) -> bool:
    """Harvey-Liu-Zhu hurdle: ``|t| >= 3.0`` (use a normal-equivalent z for small-df tests)."""
    return statistic is not None and not math.isnan(statistic) and abs(statistic) >= threshold


# ---------------------------------------------------------------------------
# Fama-MacBeth
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FamaMacBethResult:
    """``per_date``: one row per formation (status, n_obs, r_squared, one column per
    coefficient incl. ``intercept``). ``summary``: one row per coefficient with the
    :class:`MeanInference` fields of its slope series."""

    per_date: pd.DataFrame
    summary: pd.DataFrame


def fama_macbeth(
    frame: pd.DataFrame,
    *,
    date: str,
    y: str,
    x: Sequence[str],
    horizon_periods: int,
    weights: str | None = None,
    min_obs: int | None = None,
    formation_dates: Sequence[Any] | None = None,
) -> FamaMacBethResult:
    """Per-formation cross-sectional (W)LS of ``y`` on ``[1, x...]``; slopes averaged.

    Rows with a non-finite ``y``/``x``/weight or a non-positive weight are dropped per
    date. A date needs ``max(min_obs, k+2)`` rows (``k`` = coefficients incl. intercept)
    and a full-rank design, else its status is ``insufficient_obs``/``rank_deficient``
    and its slopes are NaN. ``formation_dates`` (default: sorted dates present in
    ``frame``, same representation as ``frame[date]``) fixes the formation calendar so
    missing formations stay in place for the HAC sums; a row dated outside it raises.
    Slope means are tested with :func:`mean_inference` (``horizon_periods`` in formation
    units, so overlapping h-month labels get ``nw_lags >= h-1``).
    """
    regressors = list(x)
    if not regressors:
        raise ValueError("fama_macbeth needs at least one regressor")
    coefficients = ["intercept", *regressors]
    k = len(coefficients)
    needed = max(int(min_obs or 0), k + 2)
    columns = [date, y, *regressors, *([weights] if weights else [])]
    data = frame.loc[:, columns]
    calendar = pd.Index(sorted(data[date].dropna().unique()) if formation_dates is None else list(formation_dates))
    if not calendar.is_unique:
        raise ValueError("formation_dates must be unique")
    position = calendar.get_indexer(data[date])
    if (position < 0).any():
        raise ValueError("frame has rows dated outside formation_dates")
    numeric = data[[y, *regressors, *([weights] if weights else [])]].apply(pd.to_numeric, errors="coerce")
    finite = np.isfinite(numeric.to_numpy(dtype=float)).all(axis=1)
    if weights:
        finite &= numeric[weights].to_numpy(dtype=float) > 0
    usable = numeric.loc[finite].assign(_formation=position[finite])
    groups = {int(key): group for key, group in usable.groupby("_formation", sort=False)}
    rows: list[dict[str, Any]] = []
    for slot, day in enumerate(calendar):
        group = groups.get(slot)
        record: dict[str, Any] = {"date": day, "n_obs": 0 if group is None else len(group),
                                  "r_squared": float("nan"), "status": "estimated"}
        record.update({name: float("nan") for name in coefficients})
        if group is None or len(group) < needed:
            record["status"] = "insufficient_obs"
            rows.append(record)
            continue
        design = np.column_stack([np.ones(len(group)), group[regressors].to_numpy(dtype=float)])
        target = group[y].to_numpy(dtype=float)
        root_weight = np.sqrt(group[weights].to_numpy(dtype=float)) if weights else np.ones(len(group))
        scaled_design = design * root_weight[:, None]
        scaled_target = target * root_weight
        beta, _, rank, _ = np.linalg.lstsq(scaled_design, scaled_target, rcond=None)
        if rank < k:
            record["status"] = "rank_deficient"
            rows.append(record)
            continue
        residual = scaled_target - scaled_design @ beta
        weight_sq = root_weight * root_weight
        center = float(np.dot(weight_sq, target) / weight_sq.sum())
        total = float(np.dot(weight_sq, (target - center) ** 2))
        record["r_squared"] = 1.0 - float(np.dot(residual, residual)) / total if total > 0 else float("nan")
        record.update(dict(zip(coefficients, (float(value) for value in beta), strict=True)))
        rows.append(record)
    per_date = pd.DataFrame(rows, columns=["date", "n_obs", "r_squared", "status", *coefficients])
    summary_rows = []
    for name in coefficients:
        inference = mean_inference(per_date[name].to_numpy(dtype=float), horizon_periods=horizon_periods)
        summary_rows.append({"term": name, **inference.as_dict()})
    return FamaMacBethResult(per_date=per_date, summary=pd.DataFrame(summary_rows))


# ---------------------------------------------------------------------------
# Deflated Sharpe ratio (Bailey & Lopez de Prado 2014, JPM 40(5))
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class DeflatedSharpe:
    sharpe: float
    benchmark_sharpe: float
    z: float
    deflated_sharpe_ratio: float
    n_obs: int
    n_trials: int


def sharpe_moments(returns: Iterable[float]) -> tuple[float, float, float, int]:
    """Per-period Sharpe (mean / sample sd, ddof=1), skewness and NON-excess kurtosis
    (population moments), and the observation count of the finite returns."""
    array = pd.to_numeric(pd.Series(list(returns)), errors="coerce").to_numpy(dtype=float)
    array = array[np.isfinite(array)]
    n = len(array)
    if n < 3:
        raise ValueError("at least three returns are required")
    mean = float(array.mean())
    centered = array - mean
    second = float(np.mean(centered ** 2))
    if second <= 0.0:
        raise ValueError("returns have zero variance")
    sharpe = mean / float(array.std(ddof=1))
    skewness = float(np.mean(centered ** 3)) / second ** 1.5
    kurtosis = float(np.mean(centered ** 4)) / second ** 2
    return sharpe, skewness, kurtosis, n


def probabilistic_sharpe_ratio(
    sharpe: float, benchmark_sharpe: float, *, n_obs: int, skewness: float, kurtosis: float,
) -> float:
    """PSR = Phi[(SR - SR*) sqrt(T-1) / sqrt(1 - g3 SR + (g4-1)/4 SR^2)], per-period SR,
    non-excess kurtosis ``g4`` (3 for normal returns)."""
    return _normal_cdf(_psr_z(sharpe, benchmark_sharpe, n_obs, skewness, kurtosis))


def _psr_z(sharpe: float, benchmark: float, n_obs: int, skewness: float, kurtosis: float) -> float:
    if n_obs < 2:
        raise ValueError("n_obs must be at least 2")
    denominator = 1.0 - skewness * sharpe + (kurtosis - 1.0) / 4.0 * sharpe * sharpe
    if not denominator > 0.0:
        raise ValueError("non-positive Sharpe-ratio variance term")
    return (sharpe - benchmark) * math.sqrt(n_obs - 1.0) / math.sqrt(denominator)


def _normal_cdf(z: float) -> float:
    return 0.5 * math.erfc(-z / math.sqrt(2.0))


def expected_maximum_sharpe(n_trials: int, sharpe_variance: float) -> float:
    """E[max SR] of ``n_trials`` independent null trials:
    ``sqrt(V) ((1-gamma) Phi^-1(1 - 1/N) + gamma Phi^-1(1 - 1/(N e)))``; 0 for one trial."""
    if int(n_trials) != n_trials or n_trials < 1:
        raise ValueError("n_trials must be a positive integer")
    if not sharpe_variance >= 0.0:
        raise ValueError("sharpe_variance must be non-negative")
    if n_trials == 1:
        return 0.0
    return math.sqrt(sharpe_variance) * (
        (1.0 - EULER_MASCHERONI) * _NORMAL.inv_cdf(1.0 - 1.0 / n_trials)
        + EULER_MASCHERONI * _NORMAL.inv_cdf(1.0 - 1.0 / (n_trials * math.e))
    )


def deflated_sharpe_ratio(
    sharpe: float, *, n_obs: int, skewness: float, kurtosis: float,
    n_trials: int, sharpe_variance: float,
) -> DeflatedSharpe:
    """DSR = PSR against the expected maximum Sharpe of ``n_trials`` null trials whose
    estimated Sharpe ratios have cross-trial variance ``sharpe_variance`` (same period
    units as ``sharpe``)."""
    benchmark = expected_maximum_sharpe(n_trials, sharpe_variance)
    z = _psr_z(sharpe, benchmark, n_obs, skewness, kurtosis)
    return DeflatedSharpe(sharpe=sharpe, benchmark_sharpe=benchmark, z=z,
                          deflated_sharpe_ratio=_normal_cdf(z), n_obs=int(n_obs), n_trials=int(n_trials))


# ---------------------------------------------------------------------------
# Circular block bootstrap
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class BootstrapInterval:
    estimate: float
    low: float
    high: float
    level: float
    method: str
    block_length: int
    n_resamples: int
    seed: int
    percentile_low: float
    percentile_high: float
    robust_df: int

    def as_dict(self) -> dict[str, Any]:
        return asdict(self)


def circular_block_bootstrap_ci(
    values: Iterable[float] | np.ndarray | pd.Series,
    *,
    horizon_periods: int,
    level: float = 0.95,
    n_resamples: int = 1999,
    seed: int = DEFAULT_BOOTSTRAP_SEED,
    block_length: int | None = None,
    method: str = "studentized",
) -> BootstrapInterval:
    """Circular block bootstrap CI for the mean (Politis & Romano 1992), fixed seed.

    Blocks default to ``max(h, round(T^(1/3)))`` units (always ``>= h``). ``studentized``
    (default) is the bootstrap-t: resample t* = (mean* - mean)/se*_EWC and invert around
    the original EWC standard error; it holds ~95% coverage for overlapping labels,
    where the plain ``percentile`` interval under-covers (T=160 simulation: 90% at h=3,
    84% at h=12). Both intervals are returned. Missing values are dropped (compressed).
    """
    if method not in {"studentized", "percentile"}:
        raise ValueError("method must be studentized or percentile")
    if not 0.0 < level < 1.0:
        raise ValueError("level must be within (0, 1)")
    horizon = int(horizon_periods)
    if horizon != horizon_periods or horizon < 1:
        raise ValueError("horizon_periods must be a positive integer")
    array = pd.to_numeric(pd.Series(values if not isinstance(values, np.ndarray) else values.ravel()),
                          errors="coerce").to_numpy(dtype=float)
    array = array[np.isfinite(array)]
    n = len(array)
    block = max(horizon, round(n ** (1.0 / 3.0))) if block_length is None else int(block_length)
    if block < horizon:
        raise ValueError("block_length must be at least horizon_periods")
    df = ewc_degrees_of_freedom(n, horizon)
    nan = float("nan")
    if n < 2 or n_resamples < 1:
        return BootstrapInterval(float(array.mean()) if n else nan, nan, nan, level, method,
                                 block, n_resamples, seed, nan, nan, df)
    block = min(block, n)
    estimate = float(array.mean())
    rng = np.random.default_rng(seed)
    blocks_per_resample = -(-n // block)
    offsets = np.arange(block)
    basis = _ewc_basis(n, df) if df >= 1 else None
    means: list[np.ndarray] = []
    studentized: list[np.ndarray] = []
    for start in range(0, n_resamples, 256):
        size = min(256, n_resamples - start)
        starts = rng.integers(0, n, size=(size, blocks_per_resample))
        index = ((starts[:, :, None] + offsets[None, None, :]).reshape(size, -1)[:, :n]) % n
        sample = array[index]
        sample_means = sample.mean(axis=1)
        means.append(sample_means)
        if basis is not None:
            loadings = (sample - sample_means[:, None]) @ basis
            resample_se = np.sqrt(np.mean(loadings * loadings, axis=1) / n)
            with np.errstate(divide="ignore", invalid="ignore"):
                studentized.append((sample_means - estimate) / resample_se)
    tail = (1.0 - level) / 2.0
    all_means = np.concatenate(means)
    percentile_low, percentile_high = (float(v) for v in np.quantile(all_means, [tail, 1.0 - tail]))
    low, high = percentile_low, percentile_high
    if method == "studentized":
        low = high = nan
        if basis is not None:
            loadings = (array - estimate) @ basis
            original_se = math.sqrt(float(np.mean(loadings * loadings)) / n)
            t_star = np.concatenate(studentized)
            t_star = t_star[np.isfinite(t_star)]
            if original_se > 0.0 and len(t_star):
                q_low, q_high = (float(v) for v in np.quantile(t_star, [tail, 1.0 - tail]))
                low, high = estimate - q_high * original_se, estimate - q_low * original_se
    return BootstrapInterval(estimate, low, high, level, method, block, n_resamples, seed,
                             percentile_low, percentile_high, df)
