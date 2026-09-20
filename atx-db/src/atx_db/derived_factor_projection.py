"""Project an engine metric onto the monthly factor-panel grid.

Every one of the 46 pandas per-metric modules writes two numbers per
``(factor_id, security_id, as_of_date)``: ``raw_value`` (the metric itself)
and ``value`` (that metric winsorized and z-scored cross-sectionally within
``(factor_id, as_of_date)`` on a monthly rebalance grid gated by
``universe_membership``). This module is the generic replacement for a
retired per-metric module: it reads an already-computed engine metric
(quarterly, from :mod:`atx_db.derived_metrics`, or daily, from
:mod:`atx_db.market_daily`), ASOF-joins it onto the same monthly rebalance
grid the pandas modules use, and applies the identical
:mod:`atx_db.factors.cross_section` winsorize/zscore pair, so a retired
``factor_id`` keeps publishing both numbers without the bespoke SQL.
"""

from __future__ import annotations

import csv
import datetime as dt
import hashlib
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .derived_registry import DERIVED_SOURCE_NAME
from .factors.cross_section import winsorize, zscore
from .market_daily import END_OF_DAY_HOURS, MARKET_DAILY_SOURCE_NAME
from .universe import DEFAULT_UNIVERSE_ID
from .warehouse import insert_frame, json_dumps

__all__ = [
    "PROJECTION_OUTPUT_COLUMNS",
    "PROJECTION_SEED_COLUMNS",
    "PROJECTION_SEED_PATH",
    "FactorProjection",
    "FactorProjectionOptions",
    "compute_projection_rows",
    "default_projections",
    "load_projection_inputs",
    "read_projection_seed",
    "refresh_projected_factor_values",
]

#: Stable provenance tag for rows this engine writes.
PROJECTION_SOURCE_NAME = "atx-db derived factor projection v1"

PROJECTION_SEED_PATH = Path(__file__).resolve().parent / "seeds" / "derived_factor_projections.csv"
PROJECTION_SEED_COLUMNS = (
    "factor_id",
    "metric_code",
    "source_window",
    "orientation",
    "factor_name",
    "family",
    "winsor_limit",
    "minimum_names_per_date",
    "retired_module",
)
#: The identical 15-column shape all 46 pandas per-metric modules write into
#: ``fundamental_factor_values``.
PROJECTION_OUTPUT_COLUMNS = (
    "factor_value_id",
    "factor_id",
    "factor_name",
    "family",
    "security_id",
    "symbol",
    "as_of_date",
    "raw_value",
    "value",
    "available_at",
    "input_ids_json",
    "input_lineage_json",
    "is_latest_revision",
    "run_id",
    "source",
)


@dataclass(frozen=True)
class FactorProjection:
    factor_id: str
    metric_code: str
    source_window: str
    orientation: int
    factor_name: str
    family: str
    winsor_limit: float
    minimum_names_per_date: int
    retired_module: str


@dataclass(frozen=True)
class FactorProjectionOptions:
    source: str = PROJECTION_SOURCE_NAME
    derived_source: str = DERIVED_SOURCE_NAME
    market_source: str = MARKET_DAILY_SOURCE_NAME
    universe_id: str = DEFAULT_UNIVERSE_ID
    factor_ids: tuple[str, ...] | None = None
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    run_id: str | None = None


def read_projection_seed(path: Path | str = PROJECTION_SEED_PATH) -> tuple[FactorProjection, ...]:
    seed_path = Path(path)
    rows: list[FactorProjection] = []
    with seed_path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if tuple(reader.fieldnames or ()) != PROJECTION_SEED_COLUMNS:
            raise ValueError(f"{seed_path} header must be {PROJECTION_SEED_COLUMNS}")
        for raw in reader:
            orientation = int(raw["orientation"])
            if orientation not in (1, -1):
                raise ValueError(f"orientation must be 1 or -1, got {orientation}")
            if raw["source_window"] not in ("quarter", "daily"):
                raise ValueError(f"unknown source_window {raw['source_window']!r}")
            rows.append(
                FactorProjection(
                    factor_id=raw["factor_id"],
                    metric_code=raw["metric_code"],
                    source_window=raw["source_window"],
                    orientation=orientation,
                    factor_name=raw["factor_name"],
                    family=raw["family"],
                    winsor_limit=float(raw["winsor_limit"]),
                    minimum_names_per_date=int(raw["minimum_names_per_date"]),
                    retired_module=raw["retired_module"],
                )
            )
    return tuple(rows)


@lru_cache(maxsize=1)
def default_projections() -> tuple[FactorProjection, ...]:
    return read_projection_seed()


#: The rebalance grid: the last trade date of each calendar month per
#: security (from ``equity_daily_bars``), gated by ``universe_membership``.
#: Identical to the grid :func:`atx_db.asset_growth.load_asset_growth_inputs`
#: builds, so a retired factor keeps the same monthly cadence it always had.
_GRID_SQL = """
WITH price_dedup AS (
    SELECT security_id, any_value(symbol) AS symbol, trade_date,
           max(available_at) AS price_available_at
    FROM equity_daily_bars
    WHERE close > 0 AND trade_date IS NOT NULL AND available_at IS NOT NULL
      AND available_at <= CAST(trade_date AS TIMESTAMP) + INTERVAL {end_of_day} HOUR
    GROUP BY security_id, trade_date
), price_months AS (
    SELECT *, row_number() OVER (PARTITION BY security_id, year(trade_date), month(trade_date)
                                 ORDER BY trade_date DESC) AS month_rank
    FROM price_dedup
), rebalances AS (
    SELECT * FROM price_months WHERE month_rank = 1 {date_predicate}
), governed AS (
    SELECT p.security_id, p.symbol, p.trade_date, p.price_available_at,
           u.available_at AS universe_available_at,
           row_number() OVER (PARTITION BY p.security_id, p.trade_date
                              ORDER BY u.valid_from DESC, u.available_at DESC NULLS LAST,
                                       u.source_loaded_at DESC, u.source DESC) AS universe_rank
    FROM rebalances p
    JOIN universe_membership u
      ON u.universe_id = ? AND u.security_id = p.security_id
     AND u.valid_from <= p.trade_date AND (u.valid_to IS NULL OR u.valid_to >= p.trade_date)
     AND u.as_of_date <= p.trade_date AND u.is_member AND u.is_latest_revision
     AND (u.available_at IS NULL OR u.available_at <= p.price_available_at)
), grid AS (
    SELECT * EXCLUDE (universe_rank),
           CAST(trade_date AS TIMESTAMP) + INTERVAL {end_of_day} HOUR AS cutoff
    FROM governed WHERE universe_rank = 1
)
"""


def load_projection_inputs(
    store: DuckDBStore,
    projection: FactorProjection,
    options: FactorProjectionOptions,
) -> pd.DataFrame:
    """Resolve ``(security_id, as_of_date, metric_value, ...)`` on the monthly grid.

    ``source_window == 'quarter'`` ASOF-joins the metric from
    ``derived_metric_values`` (any quarterly-grid window: ``q``, ``ttm``,
    ``avg2``, ``instant``) on ``available_at <= trade_date + END_OF_DAY_HOURS``.
    ``source_window == 'daily'`` reads the metric column directly from
    ``market_daily_metrics`` at that ``trade_date``.
    """

    date_fragments: list[str] = []
    params: list[Any] = []
    if options.start_date is not None:
        date_fragments.append("AND trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        date_fragments.append("AND trade_date <= ?")
        params.append(options.end_date)
    prefix = _GRID_SQL.format(date_predicate=" ".join(date_fragments), end_of_day=END_OF_DAY_HOURS)
    params.append(options.universe_id)
    if projection.source_window == "quarter":
        sql = (
            prefix
            + """
            SELECT g.security_id, g.symbol, g.trade_date AS as_of_date,
                   d.metric.value AS metric_value,
                   d.metric.available_at AS metric_available_at,
                   d.metric.period_end AS period_end,
                   greatest(g.price_available_at,
                            coalesce(g.universe_available_at, g.price_available_at),
                            d.metric.available_at) AS decision_available_at
            FROM grid g
            ASOF JOIN (
                -- At each event keep the newest period known then. A late
                -- revision of an older period must not replace a newer one.
                SELECT security_id, available_at,
                       arg_max(
                           struct_pack(value := value, period_end := period_end,
                                       available_at := available_at),
                           (period_end, available_at, source_loaded_at, derived_value_id)
                       ) OVER (
                           PARTITION BY security_id ORDER BY available_at
                           RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
                       ) AS metric
                FROM derived_metric_values
                WHERE source = ? AND metric_code = ?
                QUALIFY row_number() OVER (
                    PARTITION BY security_id, available_at
                    ORDER BY period_end DESC, source_loaded_at DESC, derived_value_id DESC
                ) = 1
            ) d ON d.security_id = g.security_id AND g.cutoff >= d.available_at
            ORDER BY g.trade_date, g.security_id
            """
        )
        params.extend([options.derived_source, projection.metric_code])
    else:
        metric_column = projection.metric_code.replace('"', '""')
        sql = (
            prefix
            + f"""
            SELECT g.security_id, g.symbol, g.trade_date AS as_of_date,
                   m."{metric_column}" AS metric_value,
                   m.available_at AS metric_available_at,
                   CAST(NULL AS DATE) AS period_end,
                   greatest(g.price_available_at,
                            coalesce(g.universe_available_at, g.price_available_at),
                            m.available_at) AS decision_available_at
            FROM grid g
            JOIN market_daily_metrics m
              ON m.security_id = g.security_id AND m.trade_date = g.trade_date
             AND m.source = ?
             AND m.available_at <= g.cutoff
            QUALIFY row_number() OVER (
                PARTITION BY g.security_id, g.trade_date
                ORDER BY m.available_at DESC, m.source_loaded_at DESC, m.market_daily_id DESC
            ) = 1
            ORDER BY g.trade_date, g.security_id
            """
        )
        params.append(options.market_source)
    return store.con.execute(sql, params).df()


def _factor_value_id(source: str, factor_id: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, factor_id, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def compute_projection_rows(
    inputs: pd.DataFrame,
    projection: FactorProjection,
    options: FactorProjectionOptions,
) -> pd.DataFrame:
    """Sign, winsorize, and z-score one metric's grid rows into the 15-column shape."""

    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=list(PROJECTION_OUTPUT_COLUMNS))
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["decision_available_at"], errors="coerce")
    rows["metric_value"] = pd.to_numeric(rows["metric_value"], errors="coerce")
    rows = rows.dropna(subset=["security_id", "as_of_date", "available_at", "metric_value"])
    if rows.empty:
        # A metric whose declarative formula never resolves on this warehouse
        # (e.g. one of its inputs is never populated) leaves every row NaN,
        # and dropna above empties the frame. Guard here rather than falling
        # through to groupby: some pandas builds drop every column (not just
        # every row) from a DataFrame that started with rows and ended with
        # none, which turns the groupby below into a spurious KeyError on the
        # very column the previous line just used.
        return pd.DataFrame(columns=list(PROJECTION_OUTPUT_COLUMNS))
    rows = rows[rows["metric_value"].apply(lambda value: value == value and abs(value) != float("inf"))]
    if rows.empty:
        return pd.DataFrame(columns=list(PROJECTION_OUTPUT_COLUMNS))
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= projection.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=list(PROJECTION_OUTPUT_COLUMNS))
    rows["factor_id"] = projection.factor_id
    rows["factor_name"] = projection.factor_name
    rows["family"] = projection.family
    rows["raw_value"] = projection.orientation * rows["metric_value"]
    # Both cross-sectional operators consume every eligible peer. Preserve
    # each row's own input time, then publish only once the whole cohort exists.
    rows["input_decision_available_at"] = rows["available_at"]
    rows["available_at"] = rows.groupby(["factor_id", "as_of_date"])["available_at"].transform("max")
    rows = winsorize(
        rows,
        value_column="raw_value",
        output_column="winsorized_value",
        partition_columns=("factor_id", "as_of_date"),
        limits=projection.winsor_limit,
    )
    rows = zscore(
        rows,
        value_column="winsorized_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = json_dumps([f"metric:{projection.metric_code}", f"universe:{options.universe_id}"])
    rows["input_lineage_json"] = [
        json_dumps(
            {
                "method": "derived_metric_projection_v1",
                "metric_code": projection.metric_code,
                "source_window": projection.source_window,
                "orientation": projection.orientation,
                "winsor_limits": [projection.winsor_limit, projection.winsor_limit],
                "decision": {
                    "as_of_date": as_of_date,
                    "available_at": available_at,
                    "input_available_at": input_available_at,
                    "universe_id": options.universe_id,
                },
                "metric": {
                    "period_end": period_end,
                    "value": metric_value,
                    "available_at": metric_available_at,
                },
            }
        )
        for as_of_date, available_at, input_available_at, period_end, metric_value, metric_available_at in zip(
            rows["as_of_date"],
            rows["available_at"],
            rows["input_decision_available_at"],
            rows["period_end"],
            rows["metric_value"],
            rows["metric_available_at"],
            strict=True,
        )
    ]
    rows["is_latest_revision"] = True
    rows["run_id"] = options.run_id
    rows["source"] = options.source
    rows["factor_value_id"] = [
        _factor_value_id(options.source, projection.factor_id, security_id, as_of_date)
        for security_id, as_of_date in zip(rows["security_id"], rows["as_of_date"], strict=True)
    ]
    return (
        rows[list(PROJECTION_OUTPUT_COLUMNS)]
        .dropna(subset=["value"])
        .sort_values(["as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_projected_factor_values(
    store: DuckDBStore,
    options: FactorProjectionOptions | None = None,
) -> int:
    """Recompute every seeded projection's rows (or just ``options.factor_ids``)."""

    options = options or FactorProjectionOptions()
    if options.start_date is not None and options.end_date is not None and options.start_date > options.end_date:
        raise ValueError("start_date must not be after end_date")
    store.initialize()
    wanted = set(options.factor_ids) if options.factor_ids is not None else None
    total = 0
    for projection in default_projections():
        if wanted is not None and projection.factor_id not in wanted:
            continue
        frame = compute_projection_rows(load_projection_inputs(store, projection, options), projection, options)
        delete_sql = "DELETE FROM fundamental_factor_values WHERE source = ? AND factor_id = ?"
        delete_params: list[Any] = [options.source, projection.factor_id]
        if options.start_date is not None:
            delete_sql += " AND as_of_date >= ?"
            delete_params.append(options.start_date)
        if options.end_date is not None:
            delete_sql += " AND as_of_date <= ?"
            delete_params.append(options.end_date)
        with store.transaction():
            store.con.execute(delete_sql, delete_params)
            if not frame.empty:
                insert_frame(store, frame, "fundamental_factor_values", f"projection_{projection.factor_id}_insert")
        total += len(frame)
    return total
