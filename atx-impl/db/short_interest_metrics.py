"""S11: derived FINRA short-interest analytics (`short_interest_metrics`).

The cached ``finra_short_interest`` feed carries the raw bi-monthly FINRA
consolidated short-interest report (current/previous short position, average daily
volume, source days-to-cover). This module turns it into a typed, point-in-time
analytics surface — one row per ``(security_id, settlement_date)`` — adding the piece
the raw feed lacks: the **cross-sectional percentile** of days-to-cover and
short-interest change within each settlement cohort (the short-interest anomaly that
quant long/short books trade), alongside a recomputed days-to-cover, the
share-position change, and short interest as a fraction of point-in-time shares
outstanding.

Point-in-time discipline: ``as_of_date`` is the settlement date and ``available_at``
is the FINRA publication time, carried straight from the source row. Every security in
a settlement is published together, so a percentile ranked within a single settlement
cohort is knowable at that one publication instant — it never peeks across time. The
loader keeps only the latest FINRA vintage per key (revisions supersede), and the
optional short-%-float join takes the most recent share count effective on or before
the settlement.

The math lives in :func:`compute_short_interest_metrics`, a pure DataFrame->DataFrame
transform unit-tested without DuckDB; :class:`ShortInterestMetricsDataset` /
:func:`refresh_short_interest_metrics` feed it the deduped source rows and write the
result. No network — a pure transform of already-cached warehouse tables.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass

import pandas as pd

from .asof import short_interest_metrics_asof  # noqa: F401  (re-exported for callers)
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "Derived FINRA short-interest analytics"
DEFAULT_SOURCE = "derived_short_interest_metrics_v1"

SHORT_INTEREST_METRIC_COLUMNS = [
    "metric_id", "source", "security_id", "symbol", "issue_name", "settlement_date",
    "current_short_position", "previous_short_position", "average_daily_volume",
    "short_interest_change", "short_interest_change_pct", "days_to_cover",
    "days_to_cover_source", "short_pct_shares_outstanding",
    "days_to_cover_percentile", "short_interest_change_pct_percentile",
    "universe_count", "is_latest_revision", "as_of_date", "available_at", "run_id",
]


@dataclass(frozen=True)
class ShortInterestMetricsOptions:
    source: str = DEFAULT_SOURCE
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


def _metric_id(source: str, security_id, settlement_date, available_at) -> str:
    payload = "|".join(str(p) for p in (source, security_id, settlement_date, available_at))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _safe_ratio(num: pd.Series, den: pd.Series) -> pd.Series:
    """Elementwise num/den, NaN where the denominator is missing or non-positive."""
    den = pd.to_numeric(den, errors="coerce")
    num = pd.to_numeric(num, errors="coerce")
    out = num / den.where(den > 0)
    return out


def compute_short_interest_metrics(
    rows: pd.DataFrame,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: deduped source rows -> typed short-interest metric rows.

    Input carries one row per ``(security_id, settlement_date)`` (latest FINRA vintage)
    with ``current_short_position``, ``previous_short_position``, ``average_daily_volume``,
    ``days_to_cover_source``, an optional ``shares_outstanding``, plus ``available_at`` /
    ``run_id`` passthroughs. Percentiles are ranked within each ``settlement_date`` cohort.
    """
    if rows is None or rows.empty:
        return pd.DataFrame(columns=SHORT_INTEREST_METRIC_COLUMNS)

    out = rows.copy()
    cur = pd.to_numeric(out["current_short_position"], errors="coerce")
    prev = pd.to_numeric(out["previous_short_position"], errors="coerce")
    adv = pd.to_numeric(out["average_daily_volume"], errors="coerce")

    out["current_short_position"] = cur
    out["previous_short_position"] = prev
    out["average_daily_volume"] = adv
    out["short_interest_change"] = cur - prev
    out["short_interest_change_pct"] = _safe_ratio(cur - prev, prev)
    out["days_to_cover"] = _safe_ratio(cur, adv)
    if "days_to_cover_source" not in out.columns:
        out["days_to_cover_source"] = pd.NA
    if "shares_outstanding" in out.columns:
        out["short_pct_shares_outstanding"] = _safe_ratio(cur, out["shares_outstanding"])
    else:
        out["short_pct_shares_outstanding"] = pd.NA

    # Cross-sectional percentile within each settlement cohort (the short-interest
    # anomaly signal). rank(pct=True) yields (0, 1]; NaN inputs stay NaN.
    grp = out.groupby("settlement_date")
    out["days_to_cover_percentile"] = grp["days_to_cover"].rank(pct=True)
    out["short_interest_change_pct_percentile"] = grp["short_interest_change_pct"].rank(pct=True)
    out["universe_count"] = grp["days_to_cover"].transform("count").astype("Int64")

    out["source"] = source
    out["run_id"] = run_id
    out["as_of_date"] = out["settlement_date"]
    out["is_latest_revision"] = True
    out["available_at"] = pd.to_datetime(out["available_at"], errors="coerce")
    out["metric_id"] = [
        _metric_id(source, sid, sd, av)
        for sid, sd, av in zip(out["security_id"], out["settlement_date"], out["available_at"])
    ]
    for optional in ("symbol", "issue_name"):
        if optional not in out.columns:
            out[optional] = pd.NA
    return out[SHORT_INTEREST_METRIC_COLUMNS]


_LOAD_SQL = """
    WITH latest AS (
        -- Keep the most recent FINRA vintage per (security, settlement); a revision
        -- (revision_flag='R') supersedes the original print.
        SELECT *
        FROM (
            SELECT
                f.*,
                row_number() OVER (
                    PARTITION BY f.security_id, f.settlement_date
                    ORDER BY f.available_at DESC, f.source_loaded_at DESC
                ) AS rn
            FROM finra_short_interest f
            WHERE f.security_id IS NOT NULL
            {symbol_pred}
        )
        WHERE rn = 1
    ),
    shares AS (
        -- Most recent point-in-time shares outstanding effective on/before settlement.
        SELECT
            l.security_id,
            l.settlement_date,
            (
                SELECT s.share_count
                FROM shares_outstanding_history s
                WHERE s.security_id = l.security_id
                  AND s.share_count_type = 'shares_outstanding'
                  AND s.effective_date <= l.settlement_date
                  AND s.available_at <= l.available_at
                ORDER BY s.effective_date DESC, s.available_at DESC
                LIMIT 1
            ) AS shares_outstanding
        FROM latest l
    )
    SELECT
        l.security_id,
        l.symbol,
        l.issue_name,
        l.settlement_date,
        l.current_short_position_quantity AS current_short_position,
        l.previous_short_position_quantity AS previous_short_position,
        l.average_daily_volume_quantity AS average_daily_volume,
        l.days_to_cover_quantity AS days_to_cover_source,
        sh.shares_outstanding,
        l.available_at
    FROM latest l
    LEFT JOIN shares sh
      ON sh.security_id = l.security_id AND sh.settlement_date = l.settlement_date
"""


def load_short_interest_inputs(store: DuckDBStore, options: ShortInterestMetricsOptions) -> pd.DataFrame:
    """Pull the latest-vintage FINRA rows (+ optional PIT shares) into the input frame."""
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    symbol_pred = ""
    if symbols:
        store.con.register(
            "si_symbol_filter",
            pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}),
        )
        registered = True
        symbol_pred = "AND f.symbol IN (SELECT symbol FROM si_symbol_filter)"
    sql = _LOAD_SQL.format(symbol_pred=symbol_pred)
    try:
        return store.con.execute(sql).df()
    finally:
        if registered:
            store.con.unregister("si_symbol_filter")


def refresh_short_interest_metrics(store: DuckDBStore, options: ShortInterestMetricsOptions) -> int:
    """Recompute and replace the short-interest metric rows for ``options.source``."""
    store.initialize()
    inputs = load_short_interest_inputs(store, options)
    rows = compute_short_interest_metrics(inputs, source=options.source, run_id=options.run_id)
    with store.transaction():
        store.con.execute("DELETE FROM short_interest_metrics WHERE source = ?", [options.source])
        if not rows.empty:
            insert_frame(store, rows, "short_interest_metrics", "short_interest_metrics_insert")
    return int(len(rows))


class ShortInterestMetricsDataset(Dataset):
    dataset_id = "short_interest_metrics"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: ShortInterestMetricsOptions) -> DatasetLoadResult:
        rows = refresh_short_interest_metrics(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="short_interest_metrics",
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
            details={"grain": "security_id,settlement_date"},
        )
