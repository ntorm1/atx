"""S10a: consolidated inline-XBRL canonical-metric extraction.

The narrow SEC companyfacts feed this warehouse caches carries only ~16 concepts, so
balance-sheet detail (current assets/liabilities, cash, inventory, ...) needed for
liquidity/solvency ratios is absent from ``fundamental_statement_points``. That detail
*is* present in the already-cached inline-XBRL facts (``xbrl_filing_facts``), which hold
~1,000 distinct us-gaap numeric concepts. This module turns the consolidated subset of
those facts into canonical metric rows the ratio engine can consume — entirely offline,
no network.

Consolidated-total selection: a fact is the entity-level (non-segment) value iff its
filing context has zero dimension members (``dimension_count = 0`` and
``explicit_member_count = 0``); dimensioned facts are product/geography/segment
breakdowns and are excluded.

The math-free :func:`normalize_xbrl_metric_rows` is a pure transform (concept mapping,
restatement-vintage flagging, id hashing) tested without DuckDB; :func:`refresh_fundamental_xbrl_metrics`
just feeds it the consolidated candidate facts pulled from the warehouse.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "Consolidated inline-XBRL canonical metrics"
DEFAULT_SOURCE = "sec_inline_xbrl_v1"

# us-gaap concept local name -> warehouse canonical metric. S10a covers the instant
# (balance) concepts that unlock liquidity/solvency ratios; flow concepts (COGS, etc.)
# follow in a later tranche once trailing-twelve-month stitching is added.
CONCEPT_MAP = {
    "AssetsCurrent": "current_assets",
    "LiabilitiesCurrent": "current_liabilities",
    "CashAndCashEquivalentsAtCarryingValue": "cash_and_equivalents",
    "InventoryNet": "inventory",
}

XBRL_METRIC_COLUMNS = [
    "metric_id", "source", "security_id", "symbol", "cik", "canonical_metric",
    "concept", "taxonomy", "unit", "period_type", "period_start", "period_end",
    "fiscal_year", "fiscal_period", "accession_number", "value", "raw_value",
    "revision_seq", "is_latest_revision", "as_of_date", "available_at", "run_id",
]


@dataclass(frozen=True)
class FundamentalXbrlMetricOptions:
    source: str = DEFAULT_SOURCE
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


def _metric_id(source: str, security_id, canonical_metric: str, period_end, accession_number) -> str:
    payload = "|".join(
        "" if p is None else str(p)
        for p in (source, security_id, canonical_metric, period_end, accession_number)
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def normalize_xbrl_metric_rows(
    candidates: pd.DataFrame,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Map consolidated candidate facts to canonical metric rows (pure transform).

    Drops concepts not in CONCEPT_MAP, keeps every filing vintage per
    (security, metric, period_end) ordered by availability, and flags the most
    recent as ``is_latest_revision``.
    """
    if candidates is None or candidates.empty:
        return pd.DataFrame(columns=XBRL_METRIC_COLUMNS)

    out = candidates.copy()
    out["canonical_metric"] = out["concept"].map(CONCEPT_MAP)
    out = out[out["canonical_metric"].notna()].copy()
    if out.empty:
        return pd.DataFrame(columns=XBRL_METRIC_COLUMNS)

    out["available_at"] = pd.to_datetime(out["available_at"], errors="coerce")
    out["source"] = source
    out["run_id"] = run_id
    out["as_of_date"] = out["period_end"]
    out["raw_value"] = out["value"].map(lambda v: "" if pd.isna(v) else str(v))
    out["taxonomy"] = out.get("taxonomy", "us-gaap")
    for optional in ("symbol", "cik", "unit", "period_start", "fiscal_year", "fiscal_period"):
        if optional not in out.columns:
            out[optional] = pd.NA

    # One value per filing per key: a single 10-K/10-Q can tag the same (concept,
    # period_end) in several undimensioned contexts (current + prior-year comparative);
    # collapse those to one row so the natural key (and metric_id) is unique.
    natkey = ["security_id", "canonical_metric", "period_end", "accession_number"]
    out = out.sort_values(natkey + ["available_at", "value"]).drop_duplicates(natkey, keep="last")

    # Vintages across filings.
    key = ["security_id", "canonical_metric", "period_end"]
    out = out.sort_values(key + ["available_at", "accession_number"]).reset_index(drop=True)
    out["revision_seq"] = out.groupby(key).cumcount()
    out["is_latest_revision"] = ~out.duplicated(key, keep="last")
    out["metric_id"] = [
        _metric_id(source, sid, cm, pe, acc)
        for sid, cm, pe, acc in zip(
            out["security_id"], out["canonical_metric"], out["period_end"], out["accession_number"]
        )
    ]
    return out[XBRL_METRIC_COLUMNS]


def _fetch_consolidated_candidates(
    store: DuckDBStore, concepts: tuple[str, ...], symbols: tuple[str, ...] | None
) -> pd.DataFrame:
    placeholders = ", ".join(["?"] * len(concepts))
    params: list = list(concepts)
    symbol_pred = ""
    if symbols:
        sym_ph = ", ".join(["?"] * len(symbols))
        symbol_pred = f"AND s.primary_symbol IN ({sym_ph})"
        params.extend(s.strip().upper() for s in symbols)
    sql = f"""
        SELECT
            f.security_id,
            s.primary_symbol AS symbol,
            f.cik,
            f.concept,
            f.taxonomy,
            f.unit_ref AS unit,
            f.numeric_value AS value,
            ctx.period_type,
            ctx.period_start,
            coalesce(ctx.instant_date, ctx.period_end) AS period_end,
            f.accession_number,
            coalesce(ctx.acceptance_datetime, f.acceptance_datetime, ctx.filing_date::TIMESTAMP) AS available_at,
            CAST(NULL AS INTEGER) AS fiscal_year,
            CAST(NULL AS VARCHAR) AS fiscal_period
        FROM xbrl_filing_facts f
        JOIN xbrl_filing_contexts ctx ON ctx.filing_context_id = f.filing_context_id
        LEFT JOIN securities s ON s.security_id = f.security_id
        WHERE f.taxonomy = 'us-gaap'
          AND f.is_numeric
          AND ctx.period_type = 'instant'
          AND coalesce(ctx.dimension_count, 0) = 0
          AND coalesce(ctx.explicit_member_count, 0) = 0
          AND coalesce(ctx.instant_date, ctx.period_end) IS NOT NULL
          AND f.concept IN ({placeholders})
          {symbol_pred}
    """
    return store.con.execute(sql, params).df()


def refresh_fundamental_xbrl_metrics(store: DuckDBStore, options: FundamentalXbrlMetricOptions) -> int:
    """Extract consolidated inline-XBRL canonical metrics and replace the source's rows."""
    store.initialize()
    candidates = _fetch_consolidated_candidates(store, tuple(CONCEPT_MAP), options.symbols)
    rows = normalize_xbrl_metric_rows(candidates, source=options.source, run_id=options.run_id)
    with store.transaction():
        store.con.execute("DELETE FROM fundamental_xbrl_metric WHERE source = ?", [options.source])
        if not rows.empty:
            insert_frame(store, rows, "fundamental_xbrl_metric", "fundamental_xbrl_metric_insert")
    return int(len(rows))


class FundamentalXbrlMetricDataset(Dataset):
    dataset_id = "fundamental_xbrl_metric"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FundamentalXbrlMetricOptions) -> DatasetLoadResult:
        rows = refresh_fundamental_xbrl_metrics(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="fundamental_xbrl_metric",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source, "concepts": sorted(CONCEPT_MAP)},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"canonical_metrics": sorted(set(CONCEPT_MAP.values()))},
        )
