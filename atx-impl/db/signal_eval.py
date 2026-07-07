"""PF4-S1 signal-evaluation surface: per-factor rank-IC + IC-decay over the exported panel.

Scores every factor in the PIT-safe exported panel (``v_factor_panel``, owned by pf3-S10)
for SIGNAL: rank information coefficient (rank-IC), IC information ratio, t-stat, and
sign-consistency over the horizon ladder, plus the IC-decay profile across that ladder.

Forward returns are used ONLY as a scoring *target* (t+1..t+h). They are never fed back
into any factor value, and this module never rewrites the panel it reads.

This is task PF4-S1-0 (migration 0176): the IC surface only. Later tasks in this sprint
(quantile/decile spread, turnover, factor-to-factor correlation/crowding, breadth, and the
gated leakage/coverage DQC checks) extend ``evaluate_panel`` without reworking this module.
"""

from __future__ import annotations

import hashlib
import math
from dataclasses import dataclass
from typing import Any, Iterable

import numpy as np
import pandas as pd

from .connection import DEFAULT_DB_PATH, connect  # noqa: F401  (re-exported per interfaces contract)
from .quality import QualityResult, _registry_allows_check  # noqa: F401  (gated factor DQC, PF4-S1-3)
from .warehouse import insert_frame, json_dumps, quality_check  # noqa: F401  (quality_check reused by later tasks)


IC_HORIZONS: tuple[int, ...] = (1, 5, 10, 21, 63)
DEFAULT_N_QUANTILES: int = 10
SOURCE_NAME: str = "atx-impl signal evaluation engine"
DEFAULT_UNIVERSE_ID: str = "us_common_equity_liquid_v1"

# clause-G gated DQC check names (registered in quality_check_registry by migration 0179)
LEAKAGE_DQC_CHECK_NAME: str = "factor_leakage_tplus0"
COVERAGE_DQC_CHECK_NAME: str = "factor_coverage_asof_universe"
DEFAULT_LEAKAGE_ABS_CORR_THRESHOLD: float = 0.10  # |corr(factor, t+0 return)| above this -> leaky
DEFAULT_COVERAGE_MIN_FRACTION: float = 0.50  # factor must cover >= 50% of the as-of universe


@dataclass(frozen=True)
class IcResult:
    ic: pd.DataFrame  # factor_id, horizon, mean_rank_ic, ic_std, ic_information_ratio,
    #   ic_tstat, sign_consistency, n_dates, mean_names
    ic_decay: pd.DataFrame  # factor_id, horizon, ladder_position, mean_rank_ic, decay_ratio
    per_date: pd.DataFrame  # factor_id, as_of_date, horizon, rank_ic, n_names   (intermediate; not persisted)


_PER_DATE_COLUMNS = ["factor_id", "as_of_date", "horizon", "rank_ic", "n_names"]
_IC_COLUMNS = [
    "factor_id",
    "horizon",
    "mean_rank_ic",
    "ic_std",
    "ic_information_ratio",
    "ic_tstat",
    "sign_consistency",
    "n_dates",
    "mean_names",
]
_IC_DECAY_COLUMNS = ["factor_id", "horizon", "ladder_position", "mean_rank_ic", "decay_ratio"]

_QUANTILE_SPREAD_COLUMNS = [
    "factor_id",
    "horizon",
    "n_quantiles",
    "quantile",
    "mean_forward_return",
    "mean_factor_value",
    "n_obs",
    "long_short_spread",
    "long_short_hit_rate",
    "decile_monotonicity",
]
_TURNOVER_COLUMNS = [
    "factor_id",
    "n_quantiles",
    "top_decile_turnover",
    "bottom_decile_turnover",
    "mean_rank_autocorrelation",
    "n_rebalances",
]

_CORRELATION_COLUMNS = [
    "factor_id_a",
    "factor_id_b",
    "mean_correlation",
    "mean_abs_correlation",
    "n_dates",
]
_CROWDING_COLUMNS = [
    "factor_id",
    "max_abs_correlation",
    "avg_abs_correlation",
    "most_correlated_factor_id",
    "n_peers",
]
_BREADTH_COLUMNS = [
    "factor_id",
    "as_of_date",
    "n_names",
    "n_non_null",
    "universe_size",
    "coverage_fraction",
    "effective_breadth",
    "available_at",
]

_LEAKAGE_COLUMNS = ["factor_id", "abs_corr", "threshold", "is_leaky"]
_COVERAGE_COLUMNS = ["factor_id", "coverage_fraction", "min_fraction", "is_undercovered"]
_FACTOR_DQC_RESULT_COLUMNS = [
    "check_name",
    "factor_id",
    "status",
    "observed_value",
    "threshold_value",
    "severity",
    "details_json",
    "run_id",
]

_FACTOR_EVAL_MANIFEST_COLUMNS = [
    "eval_id",
    "factor_id",
    "eval_kind",
    "universe_id",
    "start_date",
    "end_date",
    "horizon_days",
    "n_quantiles",
    "evaluation_days",
    "factor_row_count",
    "params_json",
    "source",
    "run_id",
]
_FACTOR_IC_TABLE_COLUMNS = [
    "eval_id",
    "factor_id",
    "horizon",
    "mean_rank_ic",
    "ic_std",
    "ic_information_ratio",
    "ic_tstat",
    "sign_consistency",
    "n_dates",
    "mean_names",
    "universe_id",
    "start_date",
    "end_date",
    "source",
    "run_id",
]
_FACTOR_IC_DECAY_TABLE_COLUMNS = [
    "eval_id",
    "factor_id",
    "horizon",
    "ladder_position",
    "mean_rank_ic",
    "decay_ratio",
    "universe_id",
    "source",
    "run_id",
]
_FACTOR_QUANTILE_SPREAD_TABLE_COLUMNS = [
    "eval_id",
    "factor_id",
    "horizon",
    "n_quantiles",
    "quantile",
    "mean_forward_return",
    "mean_factor_value",
    "n_obs",
    "long_short_spread",
    "long_short_hit_rate",
    "decile_monotonicity",
    "universe_id",
    "source",
    "run_id",
]
_FACTOR_TURNOVER_TABLE_COLUMNS = [
    "eval_id",
    "factor_id",
    "n_quantiles",
    "top_decile_turnover",
    "bottom_decile_turnover",
    "mean_rank_autocorrelation",
    "n_rebalances",
    "universe_id",
    "source",
    "run_id",
]
_FACTOR_CORRELATION_TABLE_COLUMNS = [
    "eval_id",
    "factor_id_a",
    "factor_id_b",
    "mean_correlation",
    "mean_abs_correlation",
    "n_dates",
    "universe_id",
    "source",
    "run_id",
]
_FACTOR_CROWDING_TABLE_COLUMNS = [
    "eval_id",
    "factor_id",
    "max_abs_correlation",
    "avg_abs_correlation",
    "most_correlated_factor_id",
    "n_peers",
    "universe_id",
    "source",
    "run_id",
]
# NOTE: source_loaded_at / is_latest_revision are deliberately absent from this frame subset:
# db.warehouse._insert_projection auto-populates them (now() / true) on any target table that
# HAS those columns but whose insert frame omits them. available_at IS included here because
# _insert_projection does NOT auto-fill it -- it is populated PIT-correctly in compute_breadth.
_FACTOR_BREADTH_TABLE_COLUMNS = [
    "eval_id",
    "factor_id",
    "as_of_date",
    "n_names",
    "n_non_null",
    "universe_size",
    "coverage_fraction",
    "effective_breadth",
    "available_at",
    "universe_id",
    "source",
    "run_id",
]


# ---------------------------------------------------------------------------
# pure transforms (pandas in -> long DataFrame out; no DuckDB)
# ---------------------------------------------------------------------------


def compute_forward_returns(prices: pd.DataFrame, horizons: Iterable[int] = IC_HORIZONS) -> pd.DataFrame:
    """Derive long forward returns from a price panel.

    ``prices``: security_id, as_of_date, close  ->  long: security_id, as_of_date, horizon, forward_return.
    ``forward_return`` at ``as_of_date`` is ``close[t+h] / close[t] - 1``; rows with no t+h close
    (end of the series) are dropped rather than emitted as NaN.
    """

    horizons = tuple(horizons)
    columns = ["security_id", "as_of_date", "horizon", "forward_return"]
    if prices.empty:
        return pd.DataFrame(columns=columns)

    frame = prices.loc[:, ["security_id", "as_of_date", "close"]].copy()
    frame["as_of_date"] = pd.to_datetime(frame["as_of_date"])
    frame = frame.sort_values(["security_id", "as_of_date"], kind="stable").reset_index(drop=True)
    grouped_close = frame.groupby("security_id", sort=True)["close"]

    parts: list[pd.DataFrame] = []
    for horizon in horizons:
        future_close = grouped_close.shift(-horizon)
        forward_return = future_close / frame["close"] - 1.0
        parts.append(
            pd.DataFrame(
                {
                    "security_id": frame["security_id"],
                    "as_of_date": frame["as_of_date"].dt.date,
                    "horizon": int(horizon),
                    "forward_return": forward_return,
                }
            )
        )
    result = pd.concat(parts, ignore_index=True) if parts else pd.DataFrame(columns=columns)
    result = result.dropna(subset=["forward_return"])
    result = result.sort_values(["security_id", "as_of_date", "horizon"], kind="stable").reset_index(drop=True)
    return result.loc[:, columns]


def _normalize_asof(frame: pd.DataFrame) -> pd.DataFrame:
    """Coerce ``as_of_date`` to one canonical dtype (midnight-normalized datetime64).

    A panel read from DuckDB via ``.df()`` carries ``as_of_date`` as ``datetime64``, while
    ``compute_forward_returns`` emits it as ``datetime.date`` objects (object dtype). Merging
    the two on ``as_of_date`` otherwise raises ``ValueError: You are trying to merge on
    datetime64[...] and object columns``. Normalizing BOTH sides to the same representation
    before every panel x forward_returns merge closes that gap. Deterministic and row-order
    preserving (no sort side effects); returns a copy, never mutating the caller's frame.
    """

    if "as_of_date" not in frame.columns:
        return frame
    out = frame.copy()
    out["as_of_date"] = pd.to_datetime(out["as_of_date"]).dt.normalize()
    return out


def _rank_corr(values: pd.Series, targets: pd.Series) -> tuple[float, int]:
    """Spearman rank correlation of ``values``/``targets`` via Pearson-of-ranks (scipy-free)."""

    valid = values.notna() & targets.notna()
    n_names = int(valid.sum())
    if n_names < 3:
        return float("nan"), n_names
    ranked_values = values[valid].rank()
    ranked_targets = targets[valid].rank()
    if ranked_values.nunique() < 2 or ranked_targets.nunique() < 2:
        return float("nan"), n_names
    return float(ranked_values.corr(ranked_targets)), n_names


def _empty_ic_result() -> IcResult:
    return IcResult(
        ic=pd.DataFrame(columns=_IC_COLUMNS),
        ic_decay=pd.DataFrame(columns=_IC_DECAY_COLUMNS),
        per_date=pd.DataFrame(columns=_PER_DATE_COLUMNS),
    )


def _aggregate_ic(per_date: pd.DataFrame) -> pd.DataFrame:
    if per_date.empty:
        return pd.DataFrame(columns=_IC_COLUMNS)

    rows: list[dict[str, Any]] = []
    for (factor_id, horizon), group in per_date.groupby(["factor_id", "horizon"], sort=True):
        valid = group.dropna(subset=["rank_ic"])
        n_dates = int(len(valid))
        if n_dates == 0:
            rows.append(
                {
                    "factor_id": factor_id,
                    "horizon": int(horizon),
                    "mean_rank_ic": float("nan"),
                    "ic_std": float("nan"),
                    "ic_information_ratio": float("nan"),
                    "ic_tstat": float("nan"),
                    "sign_consistency": float("nan"),
                    "n_dates": 0,
                    "mean_names": float("nan"),
                }
            )
            continue
        mean_rank_ic = float(valid["rank_ic"].mean())
        ic_std = float(valid["rank_ic"].std(ddof=1)) if n_dates > 1 else float("nan")
        if math.isnan(ic_std) or ic_std == 0.0:
            ic_information_ratio = float("nan")
        else:
            ic_information_ratio = mean_rank_ic / ic_std
        ic_tstat = (
            ic_information_ratio * math.sqrt(n_dates)
            if not math.isnan(ic_information_ratio)
            else float("nan")
        )
        sign_target = np.sign(mean_rank_ic)
        sign_consistency = float((np.sign(valid["rank_ic"]) == sign_target).mean())
        mean_names = float(valid["n_names"].mean())
        rows.append(
            {
                "factor_id": factor_id,
                "horizon": int(horizon),
                "mean_rank_ic": mean_rank_ic,
                "ic_std": ic_std,
                "ic_information_ratio": ic_information_ratio,
                "ic_tstat": ic_tstat,
                "sign_consistency": sign_consistency,
                "n_dates": n_dates,
                "mean_names": mean_names,
            }
        )
    ic = pd.DataFrame(rows, columns=_IC_COLUMNS)
    return ic.sort_values(["factor_id", "horizon"], kind="stable").reset_index(drop=True)


def _compute_ic_decay(ic: pd.DataFrame, horizons: tuple[int, ...]) -> pd.DataFrame:
    if ic.empty:
        return pd.DataFrame(columns=_IC_DECAY_COLUMNS)

    ordered = sorted(set(horizons))
    ladder_position = {horizon: position + 1 for position, horizon in enumerate(ordered)}
    shortest = ordered[0]

    rows: list[dict[str, Any]] = []
    for factor_id, group in ic.groupby("factor_id", sort=True):
        by_horizon = dict(zip(group["horizon"], group["mean_rank_ic"]))
        base = by_horizon.get(shortest, float("nan"))
        for horizon in ordered:
            if horizon not in by_horizon:
                continue
            mean_rank_ic = by_horizon[horizon]
            if horizon == shortest:
                decay_ratio = 1.0
            elif math.isnan(base) or base == 0.0 or (isinstance(mean_rank_ic, float) and math.isnan(mean_rank_ic)):
                decay_ratio = float("nan")
            else:
                decay_ratio = float(mean_rank_ic / base)
            rows.append(
                {
                    "factor_id": factor_id,
                    "horizon": int(horizon),
                    "ladder_position": ladder_position[horizon],
                    "mean_rank_ic": mean_rank_ic,
                    "decay_ratio": decay_ratio,
                }
            )
    decay = pd.DataFrame(rows, columns=_IC_DECAY_COLUMNS)
    return decay.sort_values(["factor_id", "horizon"], kind="stable").reset_index(drop=True)


def compute_information_coefficient(
    panel: pd.DataFrame,
    forward_returns: pd.DataFrame,
    *,
    horizons: Iterable[int] = IC_HORIZONS,
) -> IcResult:
    """Per-(factor, as_of_date, horizon) Spearman rank-IC, aggregated across dates.

    ``panel``: security_id, as_of_date, factor_id, value.
    ``forward_returns``: security_id, as_of_date, horizon, forward_return.

    Cross-sections with fewer than 3 non-null (value, forward_return) pairs produce a NaN
    rank_ic and are excluded from the date-level aggregate. All outputs are stable-sorted
    by ["factor_id", "horizon"] with a reset index so identical inputs (any row order)
    reproduce byte-identical rows.
    """

    horizons = tuple(horizons)
    if panel is None or forward_returns is None or panel.empty or forward_returns.empty:
        return _empty_ic_result()

    # Normalize as_of_date on BOTH frames to one dtype before merging: the panel (from
    # DuckDB) is datetime64 while forward_returns (from compute_forward_returns) is
    # datetime.date objects, and pandas refuses to merge across that dtype mismatch.
    panel_norm = _normalize_asof(panel.loc[:, ["security_id", "as_of_date", "factor_id", "value"]])
    forward_norm = _normalize_asof(
        forward_returns.loc[:, ["security_id", "as_of_date", "horizon", "forward_return"]]
    )
    merged = panel_norm.merge(
        forward_norm,
        on=["security_id", "as_of_date"],
        how="inner",
    )
    merged = merged[merged["horizon"].isin(horizons)]
    if merged.empty:
        return _empty_ic_result()

    per_date_rows: list[dict[str, Any]] = []
    for (factor_id, as_of_date, horizon), group in merged.groupby(
        ["factor_id", "as_of_date", "horizon"], sort=True
    ):
        rank_ic, n_names = _rank_corr(group["value"], group["forward_return"])
        per_date_rows.append(
            {
                "factor_id": factor_id,
                "as_of_date": as_of_date,
                "horizon": int(horizon),
                "rank_ic": rank_ic,
                "n_names": n_names,
            }
        )
    per_date = pd.DataFrame(per_date_rows, columns=_PER_DATE_COLUMNS)
    per_date = per_date.sort_values(["factor_id", "horizon", "as_of_date"], kind="stable").reset_index(drop=True)

    ic = _aggregate_ic(per_date)
    ic_decay = _compute_ic_decay(ic, horizons)
    return IcResult(ic=ic, ic_decay=ic_decay, per_date=per_date)


def _empty_quantile_spread() -> pd.DataFrame:
    return pd.DataFrame(columns=_QUANTILE_SPREAD_COLUMNS)


def compute_quantile_spread(
    panel: pd.DataFrame,
    forward_returns: pd.DataFrame,
    *,
    n_quantiles: int = DEFAULT_N_QUANTILES,
    horizons: Iterable[int] = IC_HORIZONS,
) -> pd.DataFrame:
    """Per-(factor, horizon) decile long-short spread, hit-rate, and monotonicity.

    ``panel``: security_id, as_of_date, factor_id, value.
    ``forward_returns``: security_id, as_of_date, horizon, forward_return.

    Within each ``(factor_id, as_of_date, horizon)`` cross-section, names are bucketed into
    ``n_quantiles`` quantiles via ``value.rank(method="first")`` (deterministic tie-break)
    then ``pd.qcut(..., labels=False) + 1``. Cross-sections with fewer than ``n_quantiles``
    names are skipped entirely. The top-minus-bottom spread is computed per date and then
    averaged across dates (Fama-MacBeth style: ``long_short_spread = mean(top_minus_bottom)``,
    ``long_short_hit_rate = mean(top_minus_bottom > 0)``); per-quantile
    ``mean_forward_return``/``mean_factor_value``/``n_obs`` pool every observation assigned
    to that quantile across all dates. ``decile_monotonicity`` is the Spearman rank
    correlation (via ``_rank_corr``) between the quantile index and its pooled
    ``mean_forward_return``. Rows are stable-sorted by ["factor_id", "horizon", "quantile"]
    with a reset index so identical inputs (any row order) reproduce byte-identical output.
    """

    horizons = tuple(horizons)
    if panel is None or forward_returns is None or panel.empty or forward_returns.empty:
        return _empty_quantile_spread()

    panel_norm = _normalize_asof(panel.loc[:, ["security_id", "as_of_date", "factor_id", "value"]])
    forward_norm = _normalize_asof(
        forward_returns.loc[:, ["security_id", "as_of_date", "horizon", "forward_return"]]
    )
    merged = panel_norm.merge(forward_norm, on=["security_id", "as_of_date"], how="inner")
    merged = merged[merged["horizon"].isin(horizons)]
    merged = merged.dropna(subset=["value", "forward_return"])
    if merged.empty:
        return _empty_quantile_spread()
    # Canonicalize row order BEFORE any group-wise rank(method="first") so ties (if any)
    # break by security_id ascending regardless of the caller's input row order -- required
    # for compute_quantile_spread to be truly order-invariant, not merely "no ties observed".
    merged = merged.sort_values(
        ["factor_id", "as_of_date", "horizon", "security_id"], kind="stable"
    ).reset_index(drop=True)

    bucket_rows: list[dict[str, Any]] = []
    spread_rows: list[dict[str, Any]] = []
    for (factor_id, as_of_date, horizon), group in merged.groupby(
        ["factor_id", "as_of_date", "horizon"], sort=True
    ):
        n_names = int(len(group))
        if n_names < n_quantiles:
            continue
        ranks = group["value"].rank(method="first")
        quantile = (pd.qcut(ranks, n_quantiles, labels=False) + 1).astype(int)
        bucketed = group.assign(quantile=quantile)
        for quantile_value, bucket in bucketed.groupby("quantile", sort=True):
            bucket_rows.append(
                {
                    "factor_id": factor_id,
                    "horizon": int(horizon),
                    "quantile": int(quantile_value),
                    "forward_return_sum": float(bucket["forward_return"].sum()),
                    "factor_value_sum": float(bucket["value"].sum()),
                    "n_obs": int(len(bucket)),
                }
            )
        top_mean = bucketed.loc[bucketed["quantile"] == n_quantiles, "forward_return"].mean()
        bottom_mean = bucketed.loc[bucketed["quantile"] == 1, "forward_return"].mean()
        spread_rows.append(
            {
                "factor_id": factor_id,
                "horizon": int(horizon),
                "as_of_date": as_of_date,
                "top_minus_bottom": float(top_mean - bottom_mean),
            }
        )

    if not bucket_rows:
        return _empty_quantile_spread()

    buckets = pd.DataFrame(bucket_rows)
    quantile_agg = (
        buckets.groupby(["factor_id", "horizon", "quantile"], sort=True)
        .agg(
            forward_return_sum=("forward_return_sum", "sum"),
            factor_value_sum=("factor_value_sum", "sum"),
            n_obs=("n_obs", "sum"),
        )
        .reset_index()
    )
    quantile_agg["mean_forward_return"] = quantile_agg["forward_return_sum"] / quantile_agg["n_obs"]
    quantile_agg["mean_factor_value"] = quantile_agg["factor_value_sum"] / quantile_agg["n_obs"]

    spreads = pd.DataFrame(spread_rows)
    summary_rows: list[dict[str, Any]] = []
    for (factor_id, horizon), group in spreads.groupby(["factor_id", "horizon"], sort=True):
        summary_rows.append(
            {
                "factor_id": factor_id,
                "horizon": int(horizon),
                "long_short_spread": float(group["top_minus_bottom"].mean()),
                "long_short_hit_rate": float((group["top_minus_bottom"] > 0).mean()),
            }
        )
    summary = pd.DataFrame(summary_rows).set_index(["factor_id", "horizon"])

    rows: list[dict[str, Any]] = []
    for (factor_id, horizon), group in quantile_agg.groupby(["factor_id", "horizon"], sort=True):
        group = group.sort_values("quantile", kind="stable").reset_index(drop=True)
        decile_monotonicity, _ = _rank_corr(
            group["quantile"].astype(float), group["mean_forward_return"]
        )
        summary_row = summary.loc[(factor_id, horizon)]
        for _, row in group.iterrows():
            rows.append(
                {
                    "factor_id": factor_id,
                    "horizon": int(horizon),
                    "n_quantiles": int(n_quantiles),
                    "quantile": int(row["quantile"]),
                    "mean_forward_return": float(row["mean_forward_return"]),
                    "mean_factor_value": float(row["mean_factor_value"]),
                    "n_obs": int(row["n_obs"]),
                    "long_short_spread": float(summary_row["long_short_spread"]),
                    "long_short_hit_rate": float(summary_row["long_short_hit_rate"]),
                    "decile_monotonicity": decile_monotonicity,
                }
            )

    result = pd.DataFrame(rows, columns=_QUANTILE_SPREAD_COLUMNS)
    return result.sort_values(["factor_id", "horizon", "quantile"], kind="stable").reset_index(drop=True)


def _empty_turnover() -> pd.DataFrame:
    return pd.DataFrame(columns=_TURNOVER_COLUMNS)


def _quantile_membership(values: pd.Series, n_quantiles: int) -> pd.Series | None:
    """1..``n_quantiles`` labels for ``values`` (indexed by security_id), or ``None`` if too few names."""

    if len(values) < n_quantiles:
        return None
    ranks = values.rank(method="first")
    return (pd.qcut(ranks, n_quantiles, labels=False) + 1).astype(int)


def compute_turnover(panel: pd.DataFrame, *, n_quantiles: int = DEFAULT_N_QUANTILES) -> pd.DataFrame:
    """Per-factor rebalance-to-rebalance top/bottom-decile membership churn + rank autocorrelation.

    ``panel``: security_id, as_of_date, factor_id, value. For each factor, distinct
    ``as_of_date`` values are ordered ascending and every consecutive pair forms one
    "rebalance": top/bottom-decile membership churn is a Jaccard-complement
    (``1 - |A∩B| / |A∪B|``) over the quantile-``n_quantiles``/quantile-1 membership sets on
    each date, and the rank autocorrelation is the Spearman correlation (via ``_rank_corr``)
    of the two dates' factor values restricted to names present on both dates. A rebalance
    is only formed between two dates that each have at least ``n_quantiles`` names (mirroring
    ``compute_quantile_spread``'s cross-section skip rule); rank autocorrelation additionally
    requires at least 3 intersecting names. Stable-sorted by ["factor_id"] with a reset
    index so identical inputs (any row order) reproduce byte-identical output.
    """

    if panel is None or panel.empty:
        return _empty_turnover()

    frame = panel.loc[:, ["security_id", "as_of_date", "factor_id", "value"]].dropna(subset=["value"])
    if frame.empty:
        return _empty_turnover()
    frame = _normalize_asof(frame)
    # Canonicalize row order so per-date rank(method="first") tie-breaks by security_id
    # ascending regardless of the caller's input row order.
    frame = frame.sort_values(["factor_id", "as_of_date", "security_id"], kind="stable").reset_index(drop=True)

    rows: list[dict[str, Any]] = []
    for factor_id, factor_group in frame.groupby("factor_id", sort=True):
        by_date: dict[Any, pd.Series] = {
            as_of_date: date_group.set_index("security_id")["value"]
            for as_of_date, date_group in factor_group.groupby("as_of_date", sort=True)
        }
        dates = sorted(by_date.keys())

        top_churns: list[float] = []
        bottom_churns: list[float] = []
        autocorrs: list[float] = []
        for prev_date, next_date in zip(dates, dates[1:]):
            prev_values = by_date[prev_date]
            next_values = by_date[next_date]
            prev_quantile = _quantile_membership(prev_values, n_quantiles)
            next_quantile = _quantile_membership(next_values, n_quantiles)
            if prev_quantile is None or next_quantile is None:
                continue

            prev_top = set(prev_quantile.index[prev_quantile == n_quantiles])
            next_top = set(next_quantile.index[next_quantile == n_quantiles])
            prev_bottom = set(prev_quantile.index[prev_quantile == 1])
            next_bottom = set(next_quantile.index[next_quantile == 1])
            top_churns.append(1.0 - len(prev_top & next_top) / len(prev_top | next_top))
            bottom_churns.append(1.0 - len(prev_bottom & next_bottom) / len(prev_bottom | next_bottom))

            common = prev_values.index.intersection(next_values.index)
            if len(common) >= 3:
                autocorr, _ = _rank_corr(
                    prev_values.loc[common].reset_index(drop=True),
                    next_values.loc[common].reset_index(drop=True),
                )
                autocorrs.append(autocorr)

        rows.append(
            {
                "factor_id": factor_id,
                "n_quantiles": int(n_quantiles),
                "top_decile_turnover": float(np.mean(top_churns)) if top_churns else float("nan"),
                "bottom_decile_turnover": float(np.mean(bottom_churns)) if bottom_churns else float("nan"),
                "mean_rank_autocorrelation": float(np.nanmean(autocorrs)) if autocorrs else float("nan"),
                "n_rebalances": len(top_churns),
            }
        )

    result = pd.DataFrame(rows, columns=_TURNOVER_COLUMNS)
    return result.sort_values(["factor_id"], kind="stable").reset_index(drop=True)


def _empty_correlation() -> pd.DataFrame:
    return pd.DataFrame(columns=_CORRELATION_COLUMNS)


def compute_factor_correlation(panel: pd.DataFrame) -> pd.DataFrame:
    """Per-ordered-pair factor-to-factor cross-sectional Pearson correlation, averaged over dates.

    ``panel``: security_id, as_of_date, factor_id, value. The panel is pivoted wide to
    ``(security_id, as_of_date) x factor_id``; within each ``as_of_date`` the factor x factor
    Pearson correlation is computed pairwise over the names present that date (``min_periods=3``,
    mirroring the >= 3 common-names floor used elsewhere in this module). Each ordered pair
    ``(a, b)``, ``a != b``, is then averaged over exactly the dates where that pair's
    correlation was defined (both factors present on >= 3 common names); a pair with zero
    such dates is omitted rather than emitted as NaN. Correlations are computed strictly
    within one as-of cross-section and never pooled across dates. Stable-sorted by
    ["factor_id_a", "factor_id_b"] with a reset index so identical inputs (any row order)
    reproduce byte-identical rows.
    """

    if panel is None or panel.empty:
        return _empty_correlation()

    frame = panel.loc[:, ["security_id", "as_of_date", "factor_id", "value"]].copy()
    frame = _normalize_asof(frame)
    factor_ids = sorted(frame["factor_id"].unique())
    if len(factor_ids) < 2:
        return _empty_correlation()

    pair_values: dict[tuple[Any, Any], list[float]] = {}
    for _as_of_date, date_group in frame.groupby("as_of_date", sort=True):
        wide = date_group.pivot_table(index="security_id", columns="factor_id", values="value", aggfunc="first")
        wide = wide.reindex(columns=factor_ids)
        corr_matrix = wide.corr(method="pearson", min_periods=3)
        for a in factor_ids:
            for b in factor_ids:
                if a == b:
                    continue
                value = corr_matrix.loc[a, b]
                if pd.isna(value):
                    continue
                pair_values.setdefault((a, b), []).append(float(value))

    rows: list[dict[str, Any]] = []
    for a in factor_ids:
        for b in factor_ids:
            if a == b:
                continue
            values = pair_values.get((a, b))
            if not values:
                continue
            arr = np.asarray(values, dtype=float)
            rows.append(
                {
                    "factor_id_a": a,
                    "factor_id_b": b,
                    "mean_correlation": float(arr.mean()),
                    "mean_abs_correlation": float(np.abs(arr).mean()),
                    "n_dates": int(len(arr)),
                }
            )

    result = pd.DataFrame(rows, columns=_CORRELATION_COLUMNS)
    return result.sort_values(["factor_id_a", "factor_id_b"], kind="stable").reset_index(drop=True)


def _empty_crowding() -> pd.DataFrame:
    return pd.DataFrame(columns=_CROWDING_COLUMNS)


def compute_crowding(correlation: pd.DataFrame) -> pd.DataFrame:
    """Per-factor crowding summary derived from ``compute_factor_correlation``'s output.

    Per ``factor_id_a``: ``max_abs_correlation``/``avg_abs_correlation`` are the max/mean of
    that factor's ``mean_abs_correlation`` across its peers; ``most_correlated_factor_id`` is
    the peer ``factor_id_b`` that maximizes ``abs(mean_correlation)`` (the signed correlation,
    not the abs-averaged one -- a peer can have a high abs-averaged correlation while its
    signed mean is small if the sign flips across dates); ``n_peers`` is the number of peers
    with a defined correlation. Ties in the argmax break on the first peer in
    ``factor_id_b`` ascending order (the input is already sorted that way). Stable-sorted by
    ["factor_id"] with a reset index.
    """

    if correlation is None or correlation.empty:
        return _empty_crowding()

    rows: list[dict[str, Any]] = []
    for factor_id, group in correlation.groupby("factor_id_a", sort=True):
        abs_signed = group["mean_correlation"].abs()
        top_index = abs_signed.idxmax()
        rows.append(
            {
                "factor_id": factor_id,
                "max_abs_correlation": float(group["mean_abs_correlation"].max()),
                "avg_abs_correlation": float(group["mean_abs_correlation"].mean()),
                "most_correlated_factor_id": group.loc[top_index, "factor_id_b"],
                "n_peers": int(len(group)),
            }
        )

    result = pd.DataFrame(rows, columns=_CROWDING_COLUMNS)
    return result.sort_values(["factor_id"], kind="stable").reset_index(drop=True)


def _empty_breadth() -> pd.DataFrame:
    return pd.DataFrame(columns=_BREADTH_COLUMNS)


def compute_breadth(panel: pd.DataFrame, universe_counts: pd.DataFrame | None = None) -> pd.DataFrame:
    """Per-(factor, as_of_date) cross-sectional breadth: names, non-null coverage, universe fraction.

    ``panel``: security_id, as_of_date, factor_id, value (and, when available,
    ``available_at``). ``n_names`` counts distinct ``security_id`` rows present for the factor
    on that date (whether or not ``value`` is null); ``n_non_null`` counts only the non-null
    ``value`` rows. If ``universe_counts`` (``as_of_date``, ``universe_size``) is supplied it is
    left-joined on ``as_of_date`` to derive ``coverage_fraction = n_non_null / universe_size``;
    without it ``universe_size`` and ``coverage_fraction`` are NaN (SQL NULL once persisted).
    ``effective_breadth`` is documented in ``field_catalog`` as a placeholder equal to
    ``n_non_null`` -- a true weight-based effective-number-of-bets measure is a later sprint's
    concern.

    ``available_at`` is the PIT availability of the breadth fact: ``max(panel.available_at)``
    over that ``(factor_id, as_of_date)`` group -- the breadth could not be *known* before all
    of its input factor rows were themselves available. When the panel carries no
    ``available_at`` column (pure-transform fixtures that omit it), it falls back to a single
    compute-time ``now()`` for every row, which is conservative/PIT-safe (never earlier than any
    input). Any group whose input ``available_at`` values are all NULL likewise falls back to
    that ``now()``. Stable-sorted by ["factor_id", "as_of_date"] with a reset index so identical
    inputs (any row order) reproduce byte-identical rows (the ``available_at`` ``max`` aggregate
    is itself order-independent).
    """

    if panel is None or panel.empty:
        return _empty_breadth()

    has_available = "available_at" in panel.columns
    input_columns = ["security_id", "as_of_date", "factor_id", "value"]
    if has_available:
        input_columns = input_columns + ["available_at"]
    frame = panel.loc[:, input_columns].copy()
    frame = _normalize_asof(frame)
    if has_available:
        frame["available_at"] = pd.to_datetime(frame["available_at"])

    agg_kwargs: dict[str, tuple[str, str]] = {
        "n_names": ("security_id", "nunique"),
        "n_non_null": ("value", "count"),
    }
    if has_available:
        agg_kwargs["available_at"] = ("available_at", "max")
    breadth = frame.groupby(["factor_id", "as_of_date"], sort=True).agg(**agg_kwargs).reset_index()
    breadth["n_names"] = breadth["n_names"].astype(int)
    breadth["n_non_null"] = breadth["n_non_null"].astype(int)

    # Conservative PIT-safe availability fallback: a single compute-time now() where the panel
    # supplied no available_at at all, or where a group's inputs were all NULL.
    fallback_available_at = pd.Timestamp.now()
    if has_available:
        breadth["available_at"] = breadth["available_at"].fillna(fallback_available_at)
    else:
        breadth["available_at"] = fallback_available_at

    if universe_counts is not None and not universe_counts.empty:
        uni = universe_counts.loc[:, ["as_of_date", "universe_size"]].copy()
        uni = _normalize_asof(uni)
        breadth = breadth.merge(uni, on="as_of_date", how="left")
    else:
        breadth["universe_size"] = float("nan")

    breadth["coverage_fraction"] = breadth["n_non_null"] / breadth["universe_size"]
    breadth["effective_breadth"] = breadth["n_non_null"].astype(float)

    result = breadth.loc[:, _BREADTH_COLUMNS]
    return result.sort_values(["factor_id", "as_of_date"], kind="stable").reset_index(drop=True)


def _empty_leakage() -> pd.DataFrame:
    return pd.DataFrame(columns=_LEAKAGE_COLUMNS)


def compute_leakage(
    panel: pd.DataFrame,
    same_day_returns: pd.DataFrame,
    *,
    threshold: float = DEFAULT_LEAKAGE_ABS_CORR_THRESHOLD,
) -> pd.DataFrame:
    """Per-factor pooled |Pearson corr| between the factor value and the SAME-DAY (t+0) return.

    ``panel``: security_id, as_of_date, factor_id, value.
    ``same_day_returns``: security_id, as_of_date, same_day_return -- the return realized
    AT ``as_of_date`` (contemporaneous, NOT a forward/future return). This t+0 return is used
    here purely as an adversarial leakage probe: it is never a scoring target (unlike
    ``compute_information_coefficient``'s forward returns) and is never fed back into any
    factor value.

    Merges on ``(security_id, as_of_date)``; per ``factor_id`` computes ONE pooled Pearson
    correlation across every merged (value, same_day_return) pair for that factor -- unlike
    the rank-IC surface, this is deliberately NOT aggregated per as-of-date first, because the
    failure mode under test is a factor that IS (or is a near-copy of) the contemporaneous
    return, which a single pooled statistic over all names/dates catches directly.
    ``is_leaky = abs_corr > threshold``. A factor with fewer than 2 valid pairs or with no
    variance in either series has an undefined (NaN) correlation and is conservatively NOT
    flagged leaky (``is_leaky = False``) rather than flagged on a degenerate statistic.
    Stable-sorted by ["factor_id"] with a reset index so identical inputs (any row order)
    reproduce byte-identical rows.
    """

    if panel is None or same_day_returns is None or panel.empty or same_day_returns.empty:
        return _empty_leakage()

    panel_norm = _normalize_asof(panel.loc[:, ["security_id", "as_of_date", "factor_id", "value"]])
    returns_norm = _normalize_asof(
        same_day_returns.loc[:, ["security_id", "as_of_date", "same_day_return"]]
    )
    merged = panel_norm.merge(returns_norm, on=["security_id", "as_of_date"], how="inner")
    merged = merged.dropna(subset=["value", "same_day_return"])
    if merged.empty:
        return _empty_leakage()

    rows: list[dict[str, Any]] = []
    for factor_id, group in merged.groupby("factor_id", sort=True):
        if (
            len(group) < 2
            or group["value"].nunique() < 2
            or group["same_day_return"].nunique() < 2
        ):
            abs_corr = float("nan")
        else:
            corr = float(group["value"].corr(group["same_day_return"]))
            abs_corr = abs(corr) if not math.isnan(corr) else float("nan")
        is_leaky = bool(abs_corr > threshold) if not math.isnan(abs_corr) else False
        rows.append(
            {
                "factor_id": factor_id,
                "abs_corr": abs_corr,
                "threshold": float(threshold),
                "is_leaky": is_leaky,
            }
        )
    result = pd.DataFrame(rows, columns=_LEAKAGE_COLUMNS)
    return result.sort_values(["factor_id"], kind="stable").reset_index(drop=True)


def _empty_coverage() -> pd.DataFrame:
    return pd.DataFrame(columns=_COVERAGE_COLUMNS)


def compute_coverage(
    panel: pd.DataFrame,
    universe_counts: pd.DataFrame,
    *,
    min_fraction: float = DEFAULT_COVERAGE_MIN_FRACTION,
) -> pd.DataFrame:
    """Per-factor mean-over-dates coverage of the as-of universe.

    ``panel``: security_id, as_of_date, factor_id, value.
    ``universe_counts``: as_of_date, universe_size -- the as-of universe membership size
    (NOT a pooled roster; the same as-of-only contract ``compute_breadth`` documents).

    Per ``(factor_id, as_of_date)``: ``date_fraction = n_non_null / universe_size``. Per
    ``factor_id``: ``coverage_fraction`` is the mean of that per-date fraction across every
    as_of_date the factor has any row for AND that has a known universe_size (a date with no
    universe_size is skipped -- the fraction is undefined there, not zero). Dates the factor
    has no row for at all are likewise simply absent from the mean, mirroring
    ``compute_breadth``'s per-group aggregation. ``is_undercovered = coverage_fraction <
    min_fraction``. Stable-sorted by ["factor_id"] with a reset index so identical inputs
    (any row order) reproduce byte-identical rows.
    """

    if panel is None or panel.empty or universe_counts is None or universe_counts.empty:
        return _empty_coverage()

    frame = panel.loc[:, ["security_id", "as_of_date", "factor_id", "value"]].copy()
    frame = _normalize_asof(frame)
    per_date = (
        frame.groupby(["factor_id", "as_of_date"], sort=True)
        .agg(n_non_null=("value", "count"))
        .reset_index()
    )

    uni = universe_counts.loc[:, ["as_of_date", "universe_size"]].copy()
    uni = _normalize_asof(uni)
    merged = per_date.merge(uni, on="as_of_date", how="inner")
    merged = merged[merged["universe_size"] > 0]
    if merged.empty:
        return _empty_coverage()
    merged = merged.copy()
    merged["date_fraction"] = merged["n_non_null"] / merged["universe_size"]

    rows: list[dict[str, Any]] = []
    for factor_id, group in merged.groupby("factor_id", sort=True):
        coverage_fraction = float(group["date_fraction"].mean())
        rows.append(
            {
                "factor_id": factor_id,
                "coverage_fraction": coverage_fraction,
                "min_fraction": float(min_fraction),
                "is_undercovered": bool(coverage_fraction < min_fraction),
            }
        )
    result = pd.DataFrame(rows, columns=_COVERAGE_COLUMNS)
    return result.sort_values(["factor_id"], kind="stable").reset_index(drop=True)


# ---------------------------------------------------------------------------
# panel read (read-only) + persistence + orchestration (DuckDB)
# ---------------------------------------------------------------------------


def load_panel_for_eval(
    store,
    *,
    start_date=None,
    end_date=None,
    factor_ids: Iterable[str] | None = None,
) -> pd.DataFrame:
    """Read-only SELECT over ``v_factor_panel`` (the pf3-S10 exported panel).

    Never writes to the panel; only filters by an optional date window and/or factor_id set.
    """

    filters: list[str] = []
    params: list[Any] = []
    if start_date is not None:
        filters.append("p.as_of_date >= ?")
        params.append(start_date)
    if end_date is not None:
        filters.append("p.as_of_date <= ?")
        params.append(end_date)

    factor_list = [str(factor_id) for factor_id in factor_ids] if factor_ids else []
    join_sql = ""
    registered = False
    if factor_list:
        store.con.register("signal_eval_factor_filter", pd.DataFrame({"factor_id": factor_list}))
        registered = True
        join_sql = "JOIN signal_eval_factor_filter ff ON ff.factor_id = p.factor_id"

    where_sql = f"WHERE {' AND '.join(filters)}" if filters else ""
    try:
        return store.con.execute(
            f"""
            SELECT
                p.security_id,
                p.as_of_date,
                p.factor_id,
                p.value,
                p.available_at
            FROM v_factor_panel p
            {join_sql}
            {where_sql}
            ORDER BY p.factor_id, p.as_of_date, p.security_id
            """,
            params,
        ).df()
    finally:
        if registered:
            store.con.unregister("signal_eval_factor_filter")


def _hash_eval_id(prefix: str, *parts: object) -> str:
    """Deterministic id hash, mirroring ``db.alpha_research._hash_id``."""

    payload = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(f"{prefix}|{payload}".encode("utf-8")).hexdigest()


def _build_ic_manifest(
    panel: pd.DataFrame,
    ic: pd.DataFrame,
    *,
    universe_id: str,
    horizons: tuple[int, ...],
    n_quantiles: int,
    source: str,
    run_id: str | None,
) -> pd.DataFrame:
    """One ``factor_eval_manifest`` row per factor scored by ``compute_information_coefficient``.

    ``eval_id`` is deterministic over (factor_id, eval_kind, universe_id, date window,
    params) so re-evaluating the same factor/window/params replaces its manifest row
    regardless of ``run_id`` -- mirroring ``alpha_research.py``'s ``backtest_id`` idiom.
    """

    if ic.empty:
        return pd.DataFrame(columns=_FACTOR_EVAL_MANIFEST_COLUMNS)

    params_json = json_dumps({"horizons": list(horizons), "n_quantiles": n_quantiles})

    if panel.empty:
        panel_stats = pd.DataFrame(
            columns=["start_date", "end_date", "evaluation_days", "factor_row_count"]
        )
    else:
        panel_stats = panel.groupby("factor_id").agg(
            start_date=("as_of_date", "min"),
            end_date=("as_of_date", "max"),
            evaluation_days=("as_of_date", "nunique"),
            factor_row_count=("value", "size"),
        )

    rows: list[dict[str, Any]] = []
    for factor_id in sorted(ic["factor_id"].unique()):
        if factor_id in panel_stats.index:
            stats = panel_stats.loc[factor_id]
            start_date = stats["start_date"]
            end_date = stats["end_date"]
            evaluation_days = int(stats["evaluation_days"])
            factor_row_count = int(stats["factor_row_count"])
        else:
            start_date = None
            end_date = None
            evaluation_days = None
            factor_row_count = None

        eval_id = _hash_eval_id(
            "factor_eval_manifest", factor_id, "ic", universe_id, start_date, end_date, params_json
        )
        rows.append(
            {
                "eval_id": eval_id,
                "factor_id": factor_id,
                "eval_kind": "ic",
                "universe_id": universe_id,
                "start_date": start_date,
                "end_date": end_date,
                "horizon_days": None,
                "n_quantiles": None,
                "evaluation_days": evaluation_days,
                "factor_row_count": factor_row_count,
                "params_json": params_json,
                "source": source,
                "run_id": run_id,
            }
        )
    manifest = pd.DataFrame(rows, columns=_FACTOR_EVAL_MANIFEST_COLUMNS)
    return manifest.sort_values(["factor_id"], kind="stable").reset_index(drop=True)


def _panel_stats_by_factor(panel: pd.DataFrame) -> pd.DataFrame:
    """Per-factor start_date/end_date/evaluation_days/factor_row_count over ``panel``.

    Shared by the quantile-spread and turnover manifest builders (mirrors the inline
    aggregation ``_build_ic_manifest`` computes for the IC manifest).
    """

    if panel is None or panel.empty:
        return pd.DataFrame(columns=["start_date", "end_date", "evaluation_days", "factor_row_count"])
    return panel.groupby("factor_id").agg(
        start_date=("as_of_date", "min"),
        end_date=("as_of_date", "max"),
        evaluation_days=("as_of_date", "nunique"),
        factor_row_count=("value", "size"),
    )


def _build_factor_manifest_rows(
    factor_ids: Iterable[str],
    panel_stats: pd.DataFrame,
    *,
    eval_kind: str,
    universe_id: str,
    horizon_days: int | None,
    n_quantiles: int | None,
    params_json: str,
    source: str,
    run_id: str | None,
) -> pd.DataFrame:
    """One ``factor_eval_manifest`` row per factor_id, keyed by a deterministic ``eval_id``.

    Shared row-building logic for the quantile-spread and turnover manifests; the ``eval_id``
    hash includes ``eval_kind`` so it never collides with the IC manifest's rows (or each
    other) in the shared ``factor_eval_manifest`` table.
    """

    rows: list[dict[str, Any]] = []
    for factor_id in sorted(set(factor_ids)):
        if factor_id in panel_stats.index:
            stats = panel_stats.loc[factor_id]
            start_date = stats["start_date"]
            end_date = stats["end_date"]
            evaluation_days = int(stats["evaluation_days"])
            factor_row_count = int(stats["factor_row_count"])
        else:
            start_date = None
            end_date = None
            evaluation_days = None
            factor_row_count = None

        eval_id = _hash_eval_id(
            "factor_eval_manifest", factor_id, eval_kind, universe_id, start_date, end_date, params_json
        )
        rows.append(
            {
                "eval_id": eval_id,
                "factor_id": factor_id,
                "eval_kind": eval_kind,
                "universe_id": universe_id,
                "start_date": start_date,
                "end_date": end_date,
                "horizon_days": horizon_days,
                "n_quantiles": n_quantiles,
                "evaluation_days": evaluation_days,
                "factor_row_count": factor_row_count,
                "params_json": params_json,
                "source": source,
                "run_id": run_id,
            }
        )
    manifest = pd.DataFrame(rows, columns=_FACTOR_EVAL_MANIFEST_COLUMNS)
    return manifest.sort_values(["factor_id"], kind="stable").reset_index(drop=True)


def _build_quantile_manifest(
    panel: pd.DataFrame,
    quantile_spread: pd.DataFrame,
    *,
    universe_id: str,
    horizons: tuple[int, ...],
    n_quantiles: int,
    source: str,
    run_id: str | None,
) -> pd.DataFrame:
    """One ``factor_eval_manifest`` row per factor scored by ``compute_quantile_spread``."""

    if quantile_spread.empty:
        return pd.DataFrame(columns=_FACTOR_EVAL_MANIFEST_COLUMNS)
    params_json = json_dumps({"horizons": list(horizons), "n_quantiles": n_quantiles})
    return _build_factor_manifest_rows(
        quantile_spread["factor_id"].unique(),
        _panel_stats_by_factor(panel),
        eval_kind="quantile_spread",
        universe_id=universe_id,
        horizon_days=None,
        n_quantiles=n_quantiles,
        params_json=params_json,
        source=source,
        run_id=run_id,
    )


def _build_turnover_manifest(
    panel: pd.DataFrame,
    turnover: pd.DataFrame,
    *,
    universe_id: str,
    n_quantiles: int,
    source: str,
    run_id: str | None,
) -> pd.DataFrame:
    """One ``factor_eval_manifest`` row per factor scored by ``compute_turnover``."""

    if turnover.empty:
        return pd.DataFrame(columns=_FACTOR_EVAL_MANIFEST_COLUMNS)
    params_json = json_dumps({"n_quantiles": n_quantiles})
    return _build_factor_manifest_rows(
        turnover["factor_id"].unique(),
        _panel_stats_by_factor(panel),
        eval_kind="turnover",
        universe_id=universe_id,
        horizon_days=None,
        n_quantiles=n_quantiles,
        params_json=params_json,
        source=source,
        run_id=run_id,
    )


def _build_correlation_manifest(
    panel: pd.DataFrame,
    correlation: pd.DataFrame,
    *,
    universe_id: str,
    source: str,
    run_id: str | None,
) -> pd.DataFrame:
    """One ``factor_eval_manifest`` row per factor scored by ``compute_factor_correlation``.

    Keyed by the union of ``factor_id_a``/``factor_id_b`` (correlation is symmetric, so in
    practice every factor with >= 1 qualifying peer appears on both sides).
    """

    if correlation.empty:
        return pd.DataFrame(columns=_FACTOR_EVAL_MANIFEST_COLUMNS)
    factor_ids = set(correlation["factor_id_a"].unique()) | set(correlation["factor_id_b"].unique())
    params_json = json_dumps({})
    return _build_factor_manifest_rows(
        factor_ids,
        _panel_stats_by_factor(panel),
        eval_kind="factor_correlation",
        universe_id=universe_id,
        horizon_days=None,
        n_quantiles=None,
        params_json=params_json,
        source=source,
        run_id=run_id,
    )


def _build_breadth_manifest(
    panel: pd.DataFrame,
    breadth: pd.DataFrame,
    *,
    universe_id: str,
    source: str,
    run_id: str | None,
) -> pd.DataFrame:
    """One ``factor_eval_manifest`` row per factor scored by ``compute_breadth``."""

    if breadth.empty:
        return pd.DataFrame(columns=_FACTOR_EVAL_MANIFEST_COLUMNS)
    params_json = json_dumps({})
    return _build_factor_manifest_rows(
        breadth["factor_id"].unique(),
        _panel_stats_by_factor(panel),
        eval_kind="breadth",
        universe_id=universe_id,
        horizon_days=None,
        n_quantiles=None,
        params_json=params_json,
        source=source,
        run_id=run_id,
    )


def _attach_manifest_lineage(
    frame: pd.DataFrame,
    manifest: pd.DataFrame,
    *,
    universe_id: str,
    run_id: str | None,
) -> pd.DataFrame:
    lineage_columns = ["factor_id", "eval_id", "start_date", "end_date", "source"]
    lineage = (
        manifest.loc[:, lineage_columns] if not manifest.empty else pd.DataFrame(columns=lineage_columns)
    )
    out = frame.merge(lineage, on="factor_id", how="left")
    out["universe_id"] = universe_id
    out["run_id"] = run_id
    return out


def _attach_manifest_lineage_by(
    frame: pd.DataFrame,
    manifest: pd.DataFrame,
    *,
    join_column: str,
    universe_id: str,
    run_id: str | None,
) -> pd.DataFrame:
    """Like ``_attach_manifest_lineage`` but joins the manifest's ``factor_id`` against an
    arbitrarily-named column on ``frame`` (``factor_correlation`` keys on ``factor_id_a``,
    not ``factor_id``).
    """

    lineage_columns = ["factor_id", "eval_id", "source"]
    lineage = (
        manifest.loc[:, lineage_columns] if not manifest.empty else pd.DataFrame(columns=lineage_columns)
    )
    lineage = lineage.rename(columns={"factor_id": join_column})
    out = frame.merge(lineage, on=join_column, how="left")
    out["universe_id"] = universe_id
    out["run_id"] = run_id
    return out


def _replace_rows(
    store,
    frame: pd.DataFrame,
    *,
    table: str,
    relation_name: str,
    key_columns: tuple[str, ...],
) -> int:
    """Idempotent DELETE-then-``insert_frame`` replace, keyed by ``key_columns``.

    Mirrors ``db.alpha_research``'s manifest discipline. This is a deliberate null-safe
    variant of ``db.warehouse.replace_by_relation``: that sibling helper keys its DELETE on
    ``coalesce(cast(col AS VARCHAR), '')``, which conflates SQL NULL with the empty string,
    whereas here we use ``IS NOT DISTINCT FROM`` so a nullable natural-key component
    (``run_id``) matches NULL-to-NULL without collapsing NULL and '' -- required for
    ``factor_ic``/``factor_ic_decay`` idempotency when ``run_id`` is not supplied.
    """

    if frame.empty:
        return 0
    store.con.register(relation_name, frame)
    try:
        predicate = " AND ".join(f"dst.{column} IS NOT DISTINCT FROM src.{column}" for column in key_columns)
        store.con.execute(
            f"""
            DELETE FROM {table} AS dst
            USING {relation_name} AS src
            WHERE {predicate}
            """
        )
        return insert_frame(store, frame, table, f"{relation_name}_insert")
    finally:
        store.con.unregister(relation_name)


def persist_factor_ic(
    store,
    *,
    manifest: pd.DataFrame,
    ic: pd.DataFrame,
    ic_decay: pd.DataFrame,
    universe_id: str,
    run_id: str | None,
) -> dict[str, int]:
    """Attach manifest lineage to the IC / IC-decay rows and persist all three tables."""

    ic_persist = _attach_manifest_lineage(ic, manifest, universe_id=universe_id, run_id=run_id)
    ic_decay_persist = _attach_manifest_lineage(ic_decay, manifest, universe_id=universe_id, run_id=run_id)

    with store.transaction():
        manifest_count = _replace_rows(
            store,
            manifest,
            table="factor_eval_manifest",
            relation_name="factor_eval_manifest_load",
            key_columns=("eval_id",),
        )
        ic_count = _replace_rows(
            store,
            ic_persist.loc[:, _FACTOR_IC_TABLE_COLUMNS],
            table="factor_ic",
            relation_name="factor_ic_load",
            key_columns=("factor_id", "horizon", "universe_id", "run_id"),
        )
        ic_decay_count = _replace_rows(
            store,
            ic_decay_persist.loc[:, _FACTOR_IC_DECAY_TABLE_COLUMNS],
            table="factor_ic_decay",
            relation_name="factor_ic_decay_load",
            key_columns=("factor_id", "horizon", "universe_id", "run_id"),
        )

    return {
        "factor_eval_manifest": manifest_count,
        "factor_ic": ic_count,
        "factor_ic_decay": ic_decay_count,
    }


def persist_quantile_spread(
    store,
    *,
    manifest: pd.DataFrame,
    quantile_spread: pd.DataFrame,
    universe_id: str,
    run_id: str | None,
) -> dict[str, int]:
    """Attach manifest lineage to the quantile-spread rows and persist manifest + table."""

    quantile_persist = _attach_manifest_lineage(quantile_spread, manifest, universe_id=universe_id, run_id=run_id)

    with store.transaction():
        manifest_count = _replace_rows(
            store,
            manifest,
            table="factor_eval_manifest",
            relation_name="factor_eval_manifest_load_quantile",
            key_columns=("eval_id",),
        )
        quantile_count = _replace_rows(
            store,
            quantile_persist.loc[:, _FACTOR_QUANTILE_SPREAD_TABLE_COLUMNS],
            table="factor_quantile_spread",
            relation_name="factor_quantile_spread_load",
            key_columns=("factor_id", "horizon", "quantile", "universe_id", "run_id"),
        )

    return {
        "factor_eval_manifest": manifest_count,
        "factor_quantile_spread": quantile_count,
    }


def persist_turnover(
    store,
    *,
    manifest: pd.DataFrame,
    turnover: pd.DataFrame,
    universe_id: str,
    run_id: str | None,
) -> dict[str, int]:
    """Attach manifest lineage to the turnover rows and persist manifest + table."""

    turnover_persist = _attach_manifest_lineage(turnover, manifest, universe_id=universe_id, run_id=run_id)

    with store.transaction():
        manifest_count = _replace_rows(
            store,
            manifest,
            table="factor_eval_manifest",
            relation_name="factor_eval_manifest_load_turnover",
            key_columns=("eval_id",),
        )
        turnover_count = _replace_rows(
            store,
            turnover_persist.loc[:, _FACTOR_TURNOVER_TABLE_COLUMNS],
            table="factor_turnover",
            relation_name="factor_turnover_load",
            key_columns=("factor_id", "universe_id", "run_id"),
        )

    return {
        "factor_eval_manifest": manifest_count,
        "factor_turnover": turnover_count,
    }


def persist_correlation_crowding(
    store,
    *,
    manifest: pd.DataFrame,
    correlation: pd.DataFrame,
    crowding: pd.DataFrame,
    universe_id: str,
    run_id: str | None,
) -> dict[str, int]:
    """Attach manifest lineage to the correlation / crowding rows and persist all three tables.

    ``correlation`` is keyed on ``factor_id_a`` (joined via ``_attach_manifest_lineage_by``);
    ``crowding`` is keyed on ``factor_id`` (joined via the shared ``_attach_manifest_lineage``).
    Both share the same manifest (one row per factor, ``eval_kind="factor_correlation"``) so a
    single ``factor_eval_manifest`` delete-then-insert covers this surface.
    """

    correlation_persist = _attach_manifest_lineage_by(
        correlation, manifest, join_column="factor_id_a", universe_id=universe_id, run_id=run_id
    )
    crowding_persist = _attach_manifest_lineage(crowding, manifest, universe_id=universe_id, run_id=run_id)

    with store.transaction():
        manifest_count = _replace_rows(
            store,
            manifest,
            table="factor_eval_manifest",
            relation_name="factor_eval_manifest_load_correlation",
            key_columns=("eval_id",),
        )
        correlation_count = _replace_rows(
            store,
            correlation_persist.loc[:, _FACTOR_CORRELATION_TABLE_COLUMNS],
            table="factor_correlation",
            relation_name="factor_correlation_load",
            key_columns=("factor_id_a", "factor_id_b", "universe_id", "run_id"),
        )
        crowding_count = _replace_rows(
            store,
            crowding_persist.loc[:, _FACTOR_CROWDING_TABLE_COLUMNS],
            table="factor_crowding",
            relation_name="factor_crowding_load",
            key_columns=("factor_id", "universe_id", "run_id"),
        )

    return {
        "factor_eval_manifest": manifest_count,
        "factor_correlation": correlation_count,
        "factor_crowding": crowding_count,
    }


def persist_breadth(
    store,
    *,
    manifest: pd.DataFrame,
    breadth: pd.DataFrame,
    universe_id: str,
    run_id: str | None,
) -> dict[str, int]:
    """Attach manifest lineage to the breadth rows and persist manifest + table."""

    breadth_persist = _attach_manifest_lineage(breadth, manifest, universe_id=universe_id, run_id=run_id)

    with store.transaction():
        manifest_count = _replace_rows(
            store,
            manifest,
            table="factor_eval_manifest",
            relation_name="factor_eval_manifest_load_breadth",
            key_columns=("eval_id",),
        )
        breadth_count = _replace_rows(
            store,
            breadth_persist.loc[:, _FACTOR_BREADTH_TABLE_COLUMNS],
            table="factor_breadth",
            relation_name="factor_breadth_load",
            key_columns=("factor_id", "as_of_date", "universe_id", "run_id"),
        )

    return {
        "factor_eval_manifest": manifest_count,
        "factor_breadth": breadth_count,
    }


def _derive_forward_returns_from_prices(store, *, horizons: tuple[int, ...]) -> pd.DataFrame:
    prices = store.con.execute(
        """
        SELECT security_id, trade_date AS as_of_date, close
        FROM equity_daily_bars
        WHERE close IS NOT NULL
        ORDER BY security_id, trade_date
        """
    ).df()
    return compute_forward_returns(prices, horizons=horizons)


def evaluate_panel(
    store,
    *,
    forward_returns: pd.DataFrame | None = None,
    n_quantiles: int = DEFAULT_N_QUANTILES,
    horizons: Iterable[int] = IC_HORIZONS,
    universe_id: str = DEFAULT_UNIVERSE_ID,
    run_id: str | None = None,
) -> dict[str, int]:
    """Orchestrate the signal-evaluation surface: read the panel, score, persist.

    Reads ``v_factor_panel`` read-only via ``load_panel_for_eval``. If ``forward_returns``
    is not supplied, it is derived from ``equity_daily_bars`` (never re-derived from the
    panel itself, keeping the scoring target strictly separate from factor inputs).

    Runs and persists, in order: the IC / IC-decay surface (PF4-S1-0), the quantile/decile
    spread and turnover surfaces (PF4-S1-1), then the factor-to-factor correlation/crowding
    and per-date breadth surfaces (PF4-S1-2). Each surface writes its own
    ``factor_eval_manifest`` rows (keyed by a distinct, eval-kind-scoped ``eval_id``) plus
    its own metric table(s); the returned dict merges every surface's per-table row counts,
    summing ``factor_eval_manifest`` across surfaces. Breadth is computed without a live
    as-of universe-size join here (``universe_counts=None``): wiring the as-of universe
    membership count is deferred, matching the sprint doc's note that the live
    price x fundamental overlap is currently empty pending PF4-S4/PF4-S6; ``compute_breadth``
    still emits ``n_names``/``n_non_null`` with ``universe_size``/``coverage_fraction`` NULL.
    The gated leakage/coverage DQC checks are PF4-S1-3 and are not run here.
    """

    horizons = tuple(horizons)
    panel = load_panel_for_eval(store)
    if forward_returns is None:
        forward_returns = _derive_forward_returns_from_prices(store, horizons=horizons)

    ic_result = compute_information_coefficient(panel, forward_returns, horizons=horizons)
    ic_manifest = _build_ic_manifest(
        panel,
        ic_result.ic,
        universe_id=universe_id,
        horizons=horizons,
        n_quantiles=n_quantiles,
        source=SOURCE_NAME,
        run_id=run_id,
    )
    counts: dict[str, int] = dict(
        persist_factor_ic(
            store,
            manifest=ic_manifest,
            ic=ic_result.ic,
            ic_decay=ic_result.ic_decay,
            universe_id=universe_id,
            run_id=run_id,
        )
    )

    quantile_spread = compute_quantile_spread(panel, forward_returns, n_quantiles=n_quantiles, horizons=horizons)
    quantile_manifest = _build_quantile_manifest(
        panel,
        quantile_spread,
        universe_id=universe_id,
        horizons=horizons,
        n_quantiles=n_quantiles,
        source=SOURCE_NAME,
        run_id=run_id,
    )
    quantile_counts = persist_quantile_spread(
        store,
        manifest=quantile_manifest,
        quantile_spread=quantile_spread,
        universe_id=universe_id,
        run_id=run_id,
    )

    turnover = compute_turnover(panel, n_quantiles=n_quantiles)
    turnover_manifest = _build_turnover_manifest(
        panel,
        turnover,
        universe_id=universe_id,
        n_quantiles=n_quantiles,
        source=SOURCE_NAME,
        run_id=run_id,
    )
    turnover_counts = persist_turnover(
        store,
        manifest=turnover_manifest,
        turnover=turnover,
        universe_id=universe_id,
        run_id=run_id,
    )

    correlation = compute_factor_correlation(panel)
    crowding = compute_crowding(correlation)
    correlation_manifest = _build_correlation_manifest(
        panel,
        correlation,
        universe_id=universe_id,
        source=SOURCE_NAME,
        run_id=run_id,
    )
    correlation_counts = persist_correlation_crowding(
        store,
        manifest=correlation_manifest,
        correlation=correlation,
        crowding=crowding,
        universe_id=universe_id,
        run_id=run_id,
    )

    breadth = compute_breadth(panel, universe_counts=None)
    breadth_manifest = _build_breadth_manifest(
        panel,
        breadth,
        universe_id=universe_id,
        source=SOURCE_NAME,
        run_id=run_id,
    )
    breadth_counts = persist_breadth(
        store,
        manifest=breadth_manifest,
        breadth=breadth,
        universe_id=universe_id,
        run_id=run_id,
    )

    for extra_counts in (quantile_counts, turnover_counts, correlation_counts, breadth_counts):
        for table, count in extra_counts.items():
            counts[table] = counts.get(table, 0) + count

    return counts


# ---------------------------------------------------------------------------
# gated factor DQC (leakage + coverage) -- PF4-S1-3
# ---------------------------------------------------------------------------


def _relation_row_count(store, relation_name: str) -> int | None:
    """Row count of ``relation_name`` (table or view), or ``None`` if it does not exist.

    Mirrors ``db.factor_panel``'s ``_duckdb_columns``/``_table_or_view_exists`` helpers:
    querying ``duckdb_columns()`` catalog metadata (which works uniformly for both tables
    and views) rather than a bare ``SELECT`` that would raise on a missing object.
    """

    columns = store.con.execute(
        """
        SELECT column_name
        FROM duckdb_columns()
        WHERE schema_name = 'main' AND table_name = ?
        LIMIT 1
        """,
        [relation_name],
    ).fetchall()
    if not columns:
        return None
    return int(store.con.execute(f"SELECT count(*) FROM {relation_name}").fetchone()[0])


def _derive_same_day_returns_from_prices(store) -> pd.DataFrame:
    """Contemporaneous (t+0) return per (security_id, as_of_date) from ``equity_daily_bars``.

    Used ONLY by the leakage DQC probe -- never as a scoring target, never fed back into
    any factor value.
    """

    return store.con.execute(
        """
        SELECT
            security_id,
            trade_date AS as_of_date,
            close / lag(close) OVER (PARTITION BY security_id ORDER BY trade_date) - 1
                AS same_day_return
        FROM equity_daily_bars
        WHERE close IS NOT NULL
        ORDER BY security_id, trade_date
        """
    ).df()


def _derive_universe_counts(
    store, panel: pd.DataFrame, *, universe_id: str = DEFAULT_UNIVERSE_ID
) -> pd.DataFrame:
    """As-of universe membership size for every ``as_of_date`` present in ``panel``.

    Read-only, PIT-correct join against ``universe_membership`` (mirrors the membership
    predicate ``db.factor_panel.factor_panel_export_gate_report`` uses): a date absent from
    ``universe_membership`` or with the table missing entirely yields an empty frame, which
    ``compute_coverage`` treats as "coverage undefined for that date" rather than zero.
    """

    columns = ["as_of_date", "universe_size"]
    if panel is None or panel.empty:
        return pd.DataFrame(columns=columns)
    if not _relation_row_count(store, "universe_membership"):
        return pd.DataFrame(columns=columns)

    dates = sorted(pd.to_datetime(panel["as_of_date"]).dt.date.unique())
    if not dates:
        return pd.DataFrame(columns=columns)

    store.con.register("signal_eval_coverage_dates", pd.DataFrame({"as_of_date": dates}))
    try:
        return store.con.execute(
            """
            SELECT
                d.as_of_date,
                count(DISTINCT u.security_id) AS universe_size
            FROM signal_eval_coverage_dates d
            LEFT JOIN universe_membership u
              ON u.universe_id = ?
             AND u.valid_from <= d.as_of_date
             AND (u.valid_to IS NULL OR u.valid_to >= d.as_of_date)
             AND u.as_of_date <= d.as_of_date
             AND u.is_member
             AND u.is_latest_revision
             AND (u.available_at IS NULL OR CAST(u.available_at AS DATE) <= d.as_of_date)
            GROUP BY d.as_of_date
            ORDER BY d.as_of_date
            """,
            [universe_id],
        ).df()
    finally:
        store.con.unregister("signal_eval_coverage_dates")


def factor_leakage_report(
    store,
    *,
    panel: pd.DataFrame | None = None,
    same_day_returns: pd.DataFrame | None = None,
    threshold: float = DEFAULT_LEAKAGE_ABS_CORR_THRESHOLD,
) -> dict[str, object]:
    """Gate-report shape (mirrors ``factor_panel_export_gate_report``) over ``compute_leakage``.

    Uses injected ``panel``/``same_day_returns`` when supplied (offline fixtures, tests);
    otherwise reads ``v_factor_panel`` read-only via ``load_panel_for_eval`` and derives the
    same-day return from ``equity_daily_bars``. Returns
    ``{"violation_count": float(n_leaky), "rows": [...]}``.
    """

    if panel is None:
        panel = load_panel_for_eval(store)
    if same_day_returns is None:
        same_day_returns = _derive_same_day_returns_from_prices(store)

    leakage = compute_leakage(panel, same_day_returns, threshold=threshold)
    leaky = leakage[leakage["is_leaky"].astype(bool)] if not leakage.empty else leakage
    rows = [
        {
            "factor_id": row["factor_id"],
            "abs_corr": None if pd.isna(row["abs_corr"]) else float(row["abs_corr"]),
            "threshold": float(row["threshold"]),
        }
        for _, row in leaky.iterrows()
    ]
    return {"violation_count": float(len(rows)), "rows": rows}


def factor_coverage_report(
    store,
    *,
    panel: pd.DataFrame | None = None,
    universe_counts: pd.DataFrame | None = None,
    min_fraction: float = DEFAULT_COVERAGE_MIN_FRACTION,
) -> dict[str, object]:
    """Gate-report shape (mirrors ``factor_panel_export_gate_report``) over ``compute_coverage``.

    Uses injected ``panel``/``universe_counts`` when supplied (offline fixtures, tests);
    otherwise reads ``v_factor_panel`` read-only via ``load_panel_for_eval`` and derives
    ``universe_counts`` from the as-of ``universe_membership`` roster. Returns
    ``{"violation_count": float(n_undercovered), "rows": [...]}``.
    """

    if panel is None:
        panel = load_panel_for_eval(store)
    if universe_counts is None:
        universe_counts = _derive_universe_counts(store, panel)

    coverage = compute_coverage(panel, universe_counts, min_fraction=min_fraction)
    undercovered = (
        coverage[coverage["is_undercovered"].astype(bool)] if not coverage.empty else coverage
    )
    rows = [
        {
            "factor_id": row["factor_id"],
            "coverage_fraction": (
                None if pd.isna(row["coverage_fraction"]) else float(row["coverage_fraction"])
            ),
            "min_fraction": float(row["min_fraction"]),
        }
        for _, row in undercovered.iterrows()
    ]
    return {"violation_count": float(len(rows)), "rows": rows}


def _dqc_check_requested(
    check_name: str,
    *,
    requested_checks: Iterable[str] | None,
    requested_datasets: Iterable[str] | None,
) -> bool:
    """Mirrors the ``PANEL_EXPORT_GATE_CHECK_NAME`` request-filter logic in ``db.quality._runner``."""

    return (
        (requested_checks is None and requested_datasets is None)
        or (requested_checks is not None and check_name in requested_checks)
        or (requested_datasets is not None and "factor_panel" in requested_datasets)
    )


def _skipped_dqc_result(check_name: str, *, checked_at: Any, reason: str) -> "QualityResult":
    return QualityResult(
        dataset_id="factor_panel",
        table_name="v_factor_panel",
        check_name=check_name,
        status="skipped",
        observed_value=None,
        threshold_value=0.0,
        details={"checked_at": checked_at.isoformat(), "reason": reason},
        severity="critical",
    )


def signal_eval_dqc_results(
    store,
    *,
    registry,
    requested_checks,
    requested_datasets,
    checked_at,
) -> list["QualityResult"]:
    """The two clause-G gated factor DQC checks (leakage, coverage), gate-ready.

    Mirrors the ``PANEL_EXPORT_GATE_CHECK_NAME`` block in ``db.quality._runner``: each check
    is gated by ``_registry_allows_check`` plus the same requested-checks/requested-datasets
    filter logic, and emits 0/1/2 ``QualityResult`` rows total. If ``v_factor_panel`` or
    ``equity_daily_bars`` does not exist at all, the corresponding check is emitted as
    ``status="skipped"`` (mirrors ``warn_if_missing``) so a warehouse that predates these
    objects never spuriously halts the gate. An EMPTY-but-present relation (e.g. a freshly
    migrated, not-yet-populated warehouse) is deliberately NOT special-cased here: it flows
    into ``factor_leakage_report``/``factor_coverage_report`` as normal, whose underlying
    ``compute_leakage``/``compute_coverage`` calls already return an empty result frame for
    empty input, which naturally resolves to ``violation_count == 0`` -> ``status="passed"``
    -- this keeps ``status`` within the same ``{"passed", "failed", "warning"}`` vocabulary
    every other production check in the sweep already uses, rather than introducing a new
    literal for a case ("no data yet") that is not actually a violation. Both checks read the
    panel read-only via ``factor_leakage_report``/``factor_coverage_report``; recording
    results to ``data_quality_checks`` is the runner's job (``_record_quality_result``),
    matching every other check in the sweep -- this function has no side effects of its own.
    """

    results: list[QualityResult] = []

    panel_rows = _relation_row_count(store, "v_factor_panel")
    bars_rows = _relation_row_count(store, "equity_daily_bars")
    panel_exists = panel_rows is not None
    bars_exists = bars_rows is not None

    if _registry_allows_check(LEAKAGE_DQC_CHECK_NAME, registry) and _dqc_check_requested(
        LEAKAGE_DQC_CHECK_NAME,
        requested_checks=requested_checks,
        requested_datasets=requested_datasets,
    ):
        if not (panel_exists and bars_exists):
            results.append(
                _skipped_dqc_result(
                    LEAKAGE_DQC_CHECK_NAME,
                    checked_at=checked_at,
                    reason="v_factor_panel or equity_daily_bars does not exist",
                )
            )
        else:
            report = factor_leakage_report(store, threshold=DEFAULT_LEAKAGE_ABS_CORR_THRESHOLD)
            observed_value = float(report["violation_count"])
            results.append(
                QualityResult(
                    dataset_id="factor_panel",
                    table_name="v_factor_panel",
                    check_name=LEAKAGE_DQC_CHECK_NAME,
                    status="passed" if observed_value == 0.0 else "failed",
                    observed_value=observed_value,
                    threshold_value=0.0,
                    details={"checked_at": checked_at.isoformat(), "rows": report["rows"]},
                    severity="critical",
                )
            )

    if _registry_allows_check(COVERAGE_DQC_CHECK_NAME, registry) and _dqc_check_requested(
        COVERAGE_DQC_CHECK_NAME,
        requested_checks=requested_checks,
        requested_datasets=requested_datasets,
    ):
        if not panel_exists:
            results.append(
                _skipped_dqc_result(
                    COVERAGE_DQC_CHECK_NAME,
                    checked_at=checked_at,
                    reason="v_factor_panel does not exist",
                )
            )
        else:
            report = factor_coverage_report(store, min_fraction=DEFAULT_COVERAGE_MIN_FRACTION)
            observed_value = float(report["violation_count"])
            results.append(
                QualityResult(
                    dataset_id="factor_panel",
                    table_name="v_factor_panel",
                    check_name=COVERAGE_DQC_CHECK_NAME,
                    status="passed" if observed_value == 0.0 else "failed",
                    observed_value=observed_value,
                    threshold_value=0.0,
                    details={"checked_at": checked_at.isoformat(), "rows": report["rows"]},
                    severity="critical",
                )
            )

    return results


def _leakage_to_dqc_rows(leakage: pd.DataFrame, *, run_id: str | None) -> pd.DataFrame:
    if leakage is None or leakage.empty:
        return pd.DataFrame(columns=_FACTOR_DQC_RESULT_COLUMNS)
    rows: list[dict[str, Any]] = []
    for _, row in leakage.iterrows():
        abs_corr = None if pd.isna(row["abs_corr"]) else float(row["abs_corr"])
        is_leaky = bool(row["is_leaky"])
        rows.append(
            {
                "check_name": LEAKAGE_DQC_CHECK_NAME,
                "factor_id": row["factor_id"],
                "status": "failed" if is_leaky else "passed",
                "observed_value": abs_corr,
                "threshold_value": float(row["threshold"]),
                "severity": "critical",
                "details_json": json_dumps({"abs_corr": abs_corr, "is_leaky": is_leaky}),
                "run_id": run_id,
            }
        )
    return pd.DataFrame(rows, columns=_FACTOR_DQC_RESULT_COLUMNS)


def _coverage_to_dqc_rows(coverage: pd.DataFrame, *, run_id: str | None) -> pd.DataFrame:
    if coverage is None or coverage.empty:
        return pd.DataFrame(columns=_FACTOR_DQC_RESULT_COLUMNS)
    rows: list[dict[str, Any]] = []
    for _, row in coverage.iterrows():
        coverage_fraction = (
            None if pd.isna(row["coverage_fraction"]) else float(row["coverage_fraction"])
        )
        is_undercovered = bool(row["is_undercovered"])
        rows.append(
            {
                "check_name": COVERAGE_DQC_CHECK_NAME,
                "factor_id": row["factor_id"],
                "status": "failed" if is_undercovered else "passed",
                "observed_value": coverage_fraction,
                "threshold_value": float(row["min_fraction"]),
                "severity": "critical",
                "details_json": json_dumps(
                    {"coverage_fraction": coverage_fraction, "is_undercovered": is_undercovered}
                ),
                "run_id": run_id,
            }
        )
    return pd.DataFrame(rows, columns=_FACTOR_DQC_RESULT_COLUMNS)


def persist_factor_dqc(
    store,
    *,
    leakage: pd.DataFrame | None = None,
    coverage: pd.DataFrame | None = None,
    run_id: str | None = None,
) -> dict[str, int]:
    """Persist per-factor leakage/coverage DQC detail rows into ``factor_dqc_result``.

    ``leakage``/``coverage`` are the per-factor frames from ``compute_leakage``/
    ``compute_coverage`` (either may be omitted/empty to persist only the other half). This
    is the per-factor AUDIT TRAIL for the two gated checks -- distinct from (and
    complementary to) the aggregate ``QualityResult`` rows ``signal_eval_dqc_results``
    returns, which the runner records into ``data_quality_checks`` via
    ``_record_quality_result``. Idempotent DELETE-then-insert keyed on
    ``(check_name, factor_id, run_id)``, mirroring every other ``persist_*`` helper in this
    module.
    """

    frames = [
        frame
        for frame in (
            _leakage_to_dqc_rows(leakage, run_id=run_id) if leakage is not None else None,
            _coverage_to_dqc_rows(coverage, run_id=run_id) if coverage is not None else None,
        )
        if frame is not None and not frame.empty
    ]
    if not frames:
        return {"factor_dqc_result": 0}

    combined = pd.concat(frames, ignore_index=True)
    combined = combined.sort_values(["check_name", "factor_id"], kind="stable").reset_index(drop=True)

    with store.transaction():
        count = _replace_rows(
            store,
            combined,
            table="factor_dqc_result",
            relation_name="factor_dqc_result_load",
            key_columns=("check_name", "factor_id", "run_id"),
        )

    return {"factor_dqc_result": count}
