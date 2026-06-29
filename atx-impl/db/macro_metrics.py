"""S13: derived macro analytics (`macro_metrics`).

The cached FRED feed (``macro_observations`` + ``macro_series``) holds raw levels for
the canonical regime drivers — 10Y/2Y Treasury yields, fed funds, unemployment, CPI,
VIX. This module turns them into a typed, point-in-time analytics surface — one row per
``(series_id, observation_date)`` — adding the transforms quant strategies actually
condition on: change vs the prior observation, year-over-year change/growth (CPI's YoY
growth *is* the inflation rate), an expanding z-score (where does today sit vs the
series' own history), plus a synthetic ``T10Y2Y`` term-spread series (DGS10 - DGS2, the
canonical yield-curve recession signal).

Point-in-time discipline: ``as_of_date`` is the observation date and ``available_at`` is
carried from the source observation. Every derived value uses only the current and
earlier observations (YoY looks ~1 year back; the z-score is expanding), so there is no
forward leakage. Caveat: the raw feed is the latest-revision FRED graph CSV, not ALFRED
vintages, so macro-revision PIT is approximate (``available_at`` reflects the warehouse
load, not the true first-release instant) — documented, not silently assumed.

The math lives in :func:`compute_macro_metrics`, a pure DataFrame->DataFrame transform
unit-tested without DuckDB; :class:`MacroMetricsDataset` / :func:`refresh_macro_metrics`
feed it the cached observations and write the result. No network.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass

import numpy as np
import pandas as pd

from .asof import macro_metrics_asof  # noqa: F401  (re-exported for callers)
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "Derived macro analytics"
DEFAULT_SOURCE = "derived_macro_metrics_v1"

# Synthetic cross-series definitions: (output series_id, minuend, subtrahend, units).
TERM_SPREAD_SERIES = "T10Y2Y"
YOY_TOLERANCE_DAYS = 20  # how far from exactly 1 year a YoY base observation may sit
ZSCORE_MIN_PERIODS = 24  # need this many history points before a z-score is defined

MACRO_METRIC_COLUMNS = [
    "metric_id", "source", "series_id", "observation_date", "frequency", "units",
    "value", "change_abs", "change_yoy", "yoy_growth", "zscore", "is_synthetic",
    "is_latest_revision", "as_of_date", "available_at", "run_id",
]


@dataclass(frozen=True)
class MacroMetricsOptions:
    source: str = DEFAULT_SOURCE
    series_ids: tuple[str, ...] | None = None
    run_id: str | None = None


def _metric_id(source: str, series_id: str, observation_date) -> str:
    payload = "|".join(str(p) for p in (source, series_id, observation_date))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _yoy_base(g: pd.DataFrame) -> pd.Series:
    """Value ~1 year before each observation (nearest within YOY_TOLERANCE_DAYS)."""
    g = g.sort_values("observation_date")
    lookup = g[["observation_date", "value"]].copy()
    lookup["lookup_date"] = lookup["observation_date"] - pd.Timedelta(days=365)
    base = g[["observation_date", "value"]].rename(
        columns={"observation_date": "base_date", "value": "base_value"}
    )
    merged = pd.merge_asof(
        lookup.sort_values("lookup_date"),
        base.sort_values("base_date"),
        left_on="lookup_date",
        right_on="base_date",
        direction="nearest",
        tolerance=pd.Timedelta(days=YOY_TOLERANCE_DAYS),
    )
    return merged.set_index("observation_date")["base_value"]


def _derive_one_series(g: pd.DataFrame) -> pd.DataFrame:
    """Per-series transforms over a single series' observations (sorted by date)."""
    g = g.sort_values("observation_date").reset_index(drop=True)
    value = pd.to_numeric(g["value"], errors="coerce")
    g["value"] = value
    g["change_abs"] = value.diff()
    base_arr = _yoy_base(g).reindex(g["observation_date"]).to_numpy()
    val_arr = value.to_numpy(dtype="float64")
    g["change_yoy"] = val_arr - base_arr
    with np.errstate(divide="ignore", invalid="ignore"):
        growth = val_arr / base_arr - 1.0
    growth = np.where(np.isnan(base_arr) | (base_arr <= 0), np.nan, growth)
    g["yoy_growth"] = growth
    emean = value.expanding(min_periods=ZSCORE_MIN_PERIODS).mean()
    estd = value.expanding(min_periods=ZSCORE_MIN_PERIODS).std(ddof=0)
    g["zscore"] = (value - emean) / estd.where(estd > 0)
    return g


def compute_macro_metrics(
    observations: pd.DataFrame,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: raw FRED observations -> typed macro metric rows.

    Input carries one row per ``(series_id, observation_date)`` with ``value``,
    ``available_at``, and per-series ``frequency`` / ``units``. A synthetic
    ``T10Y2Y`` series (DGS10 - DGS2, inner-joined on observation_date) is appended
    before the per-series transforms run.
    """
    if observations is None or observations.empty:
        return pd.DataFrame(columns=MACRO_METRIC_COLUMNS)

    obs = observations.copy()
    # Pin nanosecond resolution: DuckDB returns DATE as datetime64[us], but Timedelta
    # arithmetic in the YoY merge_asof promotes to [ns], and merge_asof rejects mixed
    # resolutions. Normalizing here keeps every downstream date key consistent.
    obs["observation_date"] = pd.to_datetime(obs["observation_date"]).astype("datetime64[ns]")
    obs["value"] = pd.to_numeric(obs["value"], errors="coerce")
    obs["available_at"] = pd.to_datetime(obs["available_at"], errors="coerce")
    for col in ("frequency", "units"):
        if col not in obs.columns:
            obs[col] = pd.NA
    obs["is_synthetic"] = False

    # Synthetic 10Y-2Y term spread (both daily; inner join on date; availability is the
    # later of the two legs so the spread is knowable only once both yields are out).
    legs = obs[obs["series_id"].isin(["DGS10", "DGS2"])]
    if {"DGS10", "DGS2"}.issubset(set(legs["series_id"])):
        wide = legs.pivot_table(index="observation_date", values="value", columns="series_id", aggfunc="last")
        avail = legs.pivot_table(index="observation_date", values="available_at", columns="series_id", aggfunc="last")
        spread = (wide["DGS10"] - wide["DGS2"]).dropna()
        if not spread.empty:
            syn = pd.DataFrame({
                "source": source,
                "series_id": TERM_SPREAD_SERIES,
                "observation_date": spread.index,
                "frequency": "daily",
                "units": "percentage_points",
                "value": spread.to_numpy(),
                "available_at": avail.reindex(spread.index).max(axis=1).to_numpy(),
                "is_synthetic": True,
            })
            obs = pd.concat([obs, syn], ignore_index=True)

    derived = (
        obs.groupby("series_id", group_keys=False)[obs.columns.tolist()]
        .apply(_derive_one_series)
        .reset_index(drop=True)
    )

    derived["source"] = source
    derived["run_id"] = run_id
    derived["as_of_date"] = derived["observation_date"]
    derived["is_latest_revision"] = True
    derived["metric_id"] = [
        _metric_id(source, sid, od.date() if hasattr(od, "date") else od)
        for sid, od in zip(derived["series_id"], derived["observation_date"])
    ]
    derived["observation_date"] = derived["observation_date"].dt.date
    derived["as_of_date"] = derived["as_of_date"].dt.date
    return derived[MACRO_METRIC_COLUMNS]


_LOAD_SQL = """
    SELECT
        o.series_id,
        o.observation_date,
        o.value,
        o.available_at,
        s.frequency,
        s.units
    FROM macro_observations o
    LEFT JOIN macro_series s ON s.series_id = o.series_id
    {series_pred}
"""


def load_macro_inputs(store: DuckDBStore, options: MacroMetricsOptions) -> pd.DataFrame:
    series = tuple(s for s in (options.series_ids or ()) if str(s).strip())
    registered = False
    series_pred = ""
    if series:
        store.con.register(
            "macro_series_filter",
            pd.DataFrame({"series_id": sorted({str(s).strip().upper() for s in series})}),
        )
        registered = True
        series_pred = "WHERE o.series_id IN (SELECT series_id FROM macro_series_filter)"
    sql = _LOAD_SQL.format(series_pred=series_pred)
    try:
        return store.con.execute(sql).df()
    finally:
        if registered:
            store.con.unregister("macro_series_filter")


def refresh_macro_metrics(store: DuckDBStore, options: MacroMetricsOptions) -> int:
    """Recompute and replace the macro metric rows for ``options.source``."""
    store.initialize()
    inputs = load_macro_inputs(store, options)
    rows = compute_macro_metrics(inputs, source=options.source, run_id=options.run_id)
    with store.transaction():
        store.con.execute("DELETE FROM macro_metrics WHERE source = ?", [options.source])
        if not rows.empty:
            insert_frame(store, rows, "macro_metrics", "macro_metrics_insert")
    return int(len(rows))


class MacroMetricsDataset(Dataset):
    dataset_id = "macro_metrics"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: MacroMetricsOptions) -> DatasetLoadResult:
        rows = refresh_macro_metrics(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="macro_metrics",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"grain": "series_id,observation_date"},
        )
