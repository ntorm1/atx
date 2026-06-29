"""S16: derived 13F manager-level position-flow analytics (`thirteenf_position_metrics`).

The cached SEC Form 13F feed (``thirteenf_security_positions`` + ``thirteenf_manager_reports``)
records, per institutional manager, the common-share positions held in each issuer at
each quarterly report period. The issuer-level rollup already exists
(``thirteenf_security_ownership`` via :mod:`db.ownership`). This module adds the
complementary **manager-level conviction-flow** surface — one row per
``(manager_id, security_id, report_period)`` — that the issuer aggregate cannot express:
quarter-over-quarter share/value change, a NEW / ADDED / TRIMMED / UNCHANGED / EXITED
position action (the canonical 13F "smart-money flow" signal), and voting-authority
concentration. Exits are inferred the standard way: a manager that *filed* a 13F for a
quarter but no longer reports an issuer it held the immediately-prior quarter has exited
that position (a synthetic zero-share row is emitted).

Point-in-time discipline: ``as_of_date`` is the report period and ``available_at`` is the
filing's availability instant. Every value uses only this manager's current and
immediately-prior-quarter filings, both knowable by ``available_at`` — there is no
forward leakage. Caveat: the cached feed's ``filing_date`` / ``available_at`` reflect the
warehouse ingest instant (a one-time bulk load), not the true first-public SEC filing
moment (~45 days after period end), so the availability is approximate but conservative
(it never makes a filing visible *earlier* than its real publication) — documented, not
silently assumed.

The math lives in :func:`compute_position_metrics`, a pure DataFrame->DataFrame transform
unit-tested without DuckDB; :class:`ThirteenFPositionMetricsDataset` /
:func:`refresh_position_metrics` aggregate the duplicate holding lines per key and write
the result. No network — a pure transform of already-cached warehouse tables.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass

import numpy as np
import pandas as pd

from .asof import thirteenf_position_metrics_asof  # noqa: F401  (re-exported for callers)
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "Derived 13F manager-level position analytics"
DEFAULT_SOURCE = "derived_thirteenf_position_metrics_v1"

POSITION_METRIC_COLUMNS = [
    "metric_id", "source", "manager_id", "security_id", "symbol", "cusip",
    "name_of_issuer", "report_period", "filing_date", "shares_held", "value_usd",
    "portfolio_weight", "shares_held_prev", "shares_change", "shares_change_pct",
    "value_change", "position_action", "is_new_position", "is_closed_position",
    "voting_sole_pct", "is_latest_revision", "as_of_date", "available_at", "run_id",
]


@dataclass(frozen=True)
class ThirteenFPositionMetricsOptions:
    source: str = DEFAULT_SOURCE
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


def _metric_id(source: str, manager_id, security_id, report_period) -> str:
    payload = "|".join(str(p) for p in (source, manager_id, security_id, report_period))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _quarter_ordinal(dates: pd.Series) -> pd.Series:
    """Integer quarter index for quarter-end dates: year*4 + (month-1)//3."""
    d = pd.to_datetime(dates)
    return (d.dt.year * 4 + (d.dt.month - 1) // 3).astype("Int64")


def compute_position_metrics(
    positions: pd.DataFrame,
    filed: pd.DataFrame,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: aggregated common-share holdings -> typed manager-flow rows.

    ``positions`` carries one row per ``(manager_id, security_id, report_period)`` with
    ``shares_held``, ``value_usd``, ``portfolio_weight``, voting authority counts, and
    ``available_at``. ``filed`` carries one row per ``(manager_id, report_period)`` the
    manager filed (with ``filing_date`` / ``available_at``); it is used only to detect
    exits (a filed quarter where a prior holding disappeared).
    """
    if positions is None or positions.empty:
        return pd.DataFrame(columns=POSITION_METRIC_COLUMNS)

    df = positions.copy()
    df["report_period"] = pd.to_datetime(df["report_period"])
    for col in ("shares_held", "value_usd", "portfolio_weight",
                "voting_sole", "voting_shared", "voting_none"):
        if col not in df.columns:
            df[col] = np.nan
        df[col] = pd.to_numeric(df[col], errors="coerce")
    df["available_at"] = pd.to_datetime(df["available_at"], errors="coerce")
    df["qord"] = _quarter_ordinal(df["report_period"])

    # Prior-quarter holding for the same (manager, security): align last quarter's row
    # onto this quarter via qord+1, so only an *immediately-consecutive* quarter counts
    # as a continuation (a gap reads as NEW, the standard 13F change convention).
    prior = df[["manager_id", "security_id", "qord", "shares_held", "value_usd"]].copy()
    prior["qord"] = prior["qord"] + 1
    prior = prior.rename(columns={"shares_held": "shares_held_prev", "value_usd": "value_usd_prev"})
    df = df.merge(prior, on=["manager_id", "security_id", "qord"], how="left")

    df["shares_change"] = df["shares_held"] - df["shares_held_prev"]
    prev = df["shares_held_prev"]
    df["shares_change_pct"] = (df["shares_change"] / prev.where(prev > 0))
    df["value_change"] = df["value_usd"] - df["value_usd_prev"]
    df["is_new_position"] = df["shares_held_prev"].isna()
    df["is_closed_position"] = False

    change = df["shares_change"]
    action = np.where(
        df["is_new_position"], "NEW",
        np.where(change > 0, "ADDED", np.where(change < 0, "TRIMMED", "UNCHANGED")),
    )
    df["position_action"] = action

    vote_total = (df["voting_sole"].fillna(0) + df["voting_shared"].fillna(0)
                  + df["voting_none"].fillna(0))
    df["voting_sole_pct"] = df["voting_sole"] / vote_total.where(vote_total > 0)

    exits = _build_exit_rows(df, filed)
    out = pd.concat([df, exits], ignore_index=True) if not exits.empty else df

    out["report_period"] = pd.to_datetime(out["report_period"])
    out["filing_date"] = pd.to_datetime(out["filing_date"], errors="coerce")
    out["available_at"] = pd.to_datetime(out["available_at"], errors="coerce")
    out["source"] = source
    out["run_id"] = run_id
    out["is_latest_revision"] = True
    out["as_of_date"] = out["report_period"].dt.date
    out["metric_id"] = [
        _metric_id(source, m, s, rp.date() if hasattr(rp, "date") else rp)
        for m, s, rp in zip(out["manager_id"], out["security_id"], out["report_period"])
    ]
    out["report_period"] = out["report_period"].dt.date
    out["filing_date"] = out["filing_date"].dt.date
    for optional in ("symbol", "cusip", "name_of_issuer"):
        if optional not in out.columns:
            out[optional] = pd.NA
    return out[POSITION_METRIC_COLUMNS].reset_index(drop=True)


def _build_exit_rows(df: pd.DataFrame, filed: pd.DataFrame) -> pd.DataFrame:
    """Synthetic zero-share EXITED rows for holdings dropped in a filed next quarter."""
    if filed is None or filed.empty:
        return pd.DataFrame(columns=df.columns)
    f = filed.copy()
    f["report_period"] = pd.to_datetime(f["report_period"])
    f["available_at"] = pd.to_datetime(f["available_at"], errors="coerce")
    if "filing_date" in f.columns:
        f["filing_date"] = pd.to_datetime(f["filing_date"], errors="coerce")
    else:
        f["filing_date"] = pd.NaT
    f["qord"] = _quarter_ordinal(f["report_period"])
    # next-quarter filing metadata, keyed by (manager_id, qord)
    filed_meta = {
        (r.manager_id, int(r.qord)): (r.report_period, r.filing_date, r.available_at)
        for r in f.itertuples(index=False)
    }
    present_keys = set(zip(df["manager_id"], df["security_id"], df["qord"].astype("int64")))

    rows = []
    for r in df.itertuples(index=False):
        target = int(r.qord) + 1
        meta = filed_meta.get((r.manager_id, target))
        if meta is None:
            continue  # manager did not file the next quarter -> cannot infer an exit
        if (r.manager_id, r.security_id, target) in present_keys:
            continue  # still held next quarter
        rep, fdate, avail = meta
        prev_shares = r.shares_held
        rows.append({
            "manager_id": r.manager_id,
            "security_id": r.security_id,
            "symbol": r.symbol,
            "cusip": r.cusip,
            "name_of_issuer": r.name_of_issuer,
            "report_period": rep,
            "filing_date": fdate,
            "shares_held": 0.0,
            "value_usd": 0.0,
            "portfolio_weight": 0.0,
            "shares_held_prev": prev_shares,
            "shares_change": -prev_shares if pd.notna(prev_shares) else np.nan,
            "shares_change_pct": -1.0 if (pd.notna(prev_shares) and prev_shares > 0) else np.nan,
            "value_change": -r.value_usd if pd.notna(r.value_usd) else np.nan,
            "position_action": "EXITED",
            "is_new_position": False,
            "is_closed_position": True,
            "voting_sole_pct": np.nan,
            "available_at": avail,
            "qord": target,
        })
    return pd.DataFrame(rows, columns=list(df.columns))


_LOAD_POSITIONS_SQL = """
    SELECT
        p.manager_id,
        p.security_id,
        any_value(p.symbol)        AS symbol,
        any_value(p.cusip)         AS cusip,
        any_value(p.name_of_issuer) AS name_of_issuer,
        p.report_period,
        max(p.filing_date)         AS filing_date,
        sum(coalesce(p.share_quantity, 0))   AS shares_held,
        sum(coalesce(p.value_usd, 0))        AS value_usd,
        sum(coalesce(p.portfolio_weight, 0)) AS portfolio_weight,
        sum(coalesce(p.voting_auth_sole, 0))   AS voting_sole,
        sum(coalesce(p.voting_auth_shared, 0)) AS voting_shared,
        sum(coalesce(p.voting_auth_none, 0))   AS voting_none,
        max(p.available_at)        AS available_at
    FROM thirteenf_security_positions p
    WHERE p.is_common_share
      AND (p.put_call IS NULL OR p.put_call = '')
      AND p.manager_id IS NOT NULL
      AND p.security_id IS NOT NULL
      {symbol_pred}
    GROUP BY p.manager_id, p.security_id, p.report_period
"""

_LOAD_FILED_SQL = """
    SELECT
        manager_id,
        report_period,
        max(filing_date) AS filing_date,
        max(available_at) AS available_at
    FROM thirteenf_manager_reports
    WHERE manager_id IS NOT NULL AND report_period IS NOT NULL
    GROUP BY manager_id, report_period
"""


def load_position_inputs(store: DuckDBStore, options: ThirteenFPositionMetricsOptions):
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    symbol_pred = ""
    if symbols:
        store.con.register(
            "tf_symbol_filter",
            pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}),
        )
        registered = True
        symbol_pred = "AND p.symbol IN (SELECT symbol FROM tf_symbol_filter)"
    sql = _LOAD_POSITIONS_SQL.format(symbol_pred=symbol_pred)
    try:
        positions = store.con.execute(sql).df()
    finally:
        if registered:
            store.con.unregister("tf_symbol_filter")
    filed = store.con.execute(_LOAD_FILED_SQL).df()
    return positions, filed


def refresh_position_metrics(store: DuckDBStore, options: ThirteenFPositionMetricsOptions) -> int:
    """Recompute and replace the 13F position-metric rows for ``options.source``."""
    store.initialize()
    positions, filed = load_position_inputs(store, options)
    rows = compute_position_metrics(positions, filed, source=options.source, run_id=options.run_id)
    with store.transaction():
        store.con.execute("DELETE FROM thirteenf_position_metrics WHERE source = ?", [options.source])
        if not rows.empty:
            insert_frame(store, rows, "thirteenf_position_metrics", "thirteenf_position_metrics_insert")
    return int(len(rows))


class ThirteenFPositionMetricsDataset(Dataset):
    dataset_id = "thirteenf_position_metrics"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: ThirteenFPositionMetricsOptions) -> DatasetLoadResult:
        rows = refresh_position_metrics(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="thirteenf_position_metrics",
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
            details={"grain": "manager_id,security_id,report_period"},
        )
