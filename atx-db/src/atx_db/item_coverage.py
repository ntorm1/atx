"""Tier1-S2 T9: measure and publish standardized item coverage.

Coverage is (distinct securities in the universe with a non-null standardized
value) / (distinct securities in the universe) for each
(universe_id, item_id, basis, fiscal_year). The spec gate is
ITEM_COVERAGE_TARGET_ITEMS items at or above ITEM_COVERAGE_TARGET_PCT on the
top-3000 by market cap for fiscal years at or after 2015; this module measures
it, it does not assert it.
"""
from __future__ import annotations

import hashlib
from collections.abc import Sequence
from dataclasses import dataclass, field
from typing import Any

import pandas as pd

from .connection import DuckDBStore

DEFAULT_SOURCE = "fundamental_standardization_v1"
DEFAULT_UNIVERSE_ID = "us_common_equity_liquid_v1"
ITEM_COVERAGE_TARGET_ITEMS = 110
ITEM_COVERAGE_TARGET_PCT = 90.0
ITEM_COVERAGE_TARGET_TOP_N = 3000
ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR = 2015

ITEM_COVERAGE_COLUMNS = (
    "source",
    "universe_id",
    "item_id",
    "canonical_code",
    "basis",
    "fiscal_year",
    "n_securities",
    "n_with_value",
    "coverage_pct",
)


@dataclass(frozen=True)
class ItemCoverageOptions:
    source: str = DEFAULT_SOURCE
    universe_id: str = DEFAULT_UNIVERSE_ID
    bases: tuple[str, ...] = ("annual", "quarterly", "instant", "ttm")
    item_ids: tuple[int, ...] = field(default=())
    minimum_fiscal_year: int = 1990
    run_id: str | None = None


def _coverage_id(*parts: Any) -> str:
    payload = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def compute_item_coverage_rows(
    standardized: pd.DataFrame,
    universe: pd.DataFrame,
    options: ItemCoverageOptions | None = None,
) -> pd.DataFrame:
    """Pure transform: standardized long facts + universe -> coverage rows."""

    options = options or ItemCoverageOptions()
    if universe.empty:
        return pd.DataFrame(columns=list(ITEM_COVERAGE_COLUMNS))

    universe_counts = universe.groupby("fiscal_year", sort=True)["security_id"].nunique()
    universe_size: dict[int, int] = {
        int(year): int(count)
        for year, count in zip(universe_counts.index.tolist(), universe_counts.to_numpy().tolist(), strict=True)
    }

    facts = standardized.copy()
    if not facts.empty:
        facts = facts[facts["basis"].isin(options.bases)]
        facts = facts[facts["fiscal_year"] >= options.minimum_fiscal_year]
        facts = facts[facts["value"].notna()]
        facts = facts[facts["security_id"].isin(set(universe["security_id"]))]

    labels: dict[int, str] = {}
    if not standardized.empty:
        labels = {
            int(item_id): str(code)
            for item_id, code in standardized[["item_id", "canonical_code"]]
            .drop_duplicates()
            .itertuples(index=False, name=None)
        }

    observed_items = sorted({int(value) for value in standardized.get("item_id", pd.Series(dtype=int))})
    requested_items = sorted(set(options.item_ids)) or observed_items

    records: list[dict[str, Any]] = []
    for basis in sorted(options.bases):
        basis_facts = facts[facts["basis"] == basis] if not facts.empty else facts
        for fiscal_year in sorted(universe_size):
            if fiscal_year < options.minimum_fiscal_year:
                continue
            n_securities = int(universe_size[fiscal_year])
            year_facts = (
                basis_facts[basis_facts["fiscal_year"] == fiscal_year]
                if not basis_facts.empty
                else basis_facts
            )
            per_item = (
                year_facts.groupby("item_id")["security_id"].nunique().to_dict()
                if not year_facts.empty
                else {}
            )
            for item_id in requested_items:
                n_with_value = int(per_item.get(item_id, 0))
                records.append(
                    {
                        "source": options.source,
                        "universe_id": options.universe_id,
                        "item_id": int(item_id),
                        "canonical_code": labels.get(int(item_id), f"item_{item_id}"),
                        "basis": basis,
                        "fiscal_year": int(fiscal_year),
                        "n_securities": n_securities,
                        "n_with_value": n_with_value,
                        "coverage_pct": round(100.0 * n_with_value / n_securities, 6) if n_securities else 0.0,
                    }
                )

    frame = pd.DataFrame(records, columns=list(ITEM_COVERAGE_COLUMNS))
    if frame.empty:
        return frame
    return frame.sort_values(["basis", "item_id", "fiscal_year"], kind="mergesort").reset_index(drop=True)


def load_item_coverage_inputs(
    store: DuckDBStore,
    options: ItemCoverageOptions | None = None,
) -> tuple[pd.DataFrame, pd.DataFrame]:
    """Read latest-revision standardized facts and the universe by fiscal year."""

    options = options or ItemCoverageOptions()
    standardized = store.con.execute(
        """
        SELECT security_id, item_id, canonical_code, basis, fiscal_year, value
        FROM fundamental_standardized
        WHERE source = ?
          AND is_latest_revision
          AND fiscal_year IS NOT NULL
        """,
        [options.source],
    ).df()
    # The per-year cohort is every universe_membership row whose own
    # [valid_from, valid_to) validity window overlaps the fiscal year -- NOT
    # "securities that happened to report a fact that year". A true top-3000
    # constituent that filed nothing this year must still land in the
    # denominator (as 0% for every item); joining through
    # fundamental_standardized instead would silently drop it and inflate
    # coverage_pct. fiscal_year is approximated as the calendar year
    # [Y-01-01, Y-12-31]; the candidate year list itself still comes from the
    # standardized facts because that is the only source of "years we have
    # data to measure".
    universe = store.con.execute(
        """
        SELECT DISTINCT u.security_id, fy.fiscal_year
        FROM universe_membership u
        CROSS JOIN (
            SELECT DISTINCT fiscal_year
            FROM fundamental_standardized
            WHERE source = ? AND fiscal_year IS NOT NULL
        ) fy
        WHERE u.universe_id = ?
          AND u.is_latest_revision
          AND u.valid_from <= make_date(fy.fiscal_year, 12, 31)
          AND (u.valid_to IS NULL OR u.valid_to >= make_date(fy.fiscal_year, 1, 1))
        """,
        [options.source, options.universe_id],
    ).df()
    return standardized, universe


def refresh_item_coverage(
    store: DuckDBStore,
    options: ItemCoverageOptions | None = None,
    *,
    frame: pd.DataFrame | None = None,
) -> int:
    """Recompute and replace the coverage rows for this source and universe.

    The DELETE is scoped to exactly the (basis[, item_id]) slice this call
    recomputes -- ``options.bases`` always, and ``options.item_ids`` too when
    the caller narrowed it -- not to the whole (source, universe_id) pair. A
    narrower refresh (e.g. ``--basis annual``) must never drop previously
    published rows for bases/items it was not asked to recompute.

    ``frame`` lets a caller that already computed the coverage frame (e.g. to
    also render it) pass it straight through instead of paying for
    ``load_item_coverage_inputs`` + ``compute_item_coverage_rows`` twice.
    """

    options = options or ItemCoverageOptions()
    store.initialize()
    if frame is None:
        standardized, universe = load_item_coverage_inputs(store, options)
        frame = compute_item_coverage_rows(standardized, universe, options)
    if frame.empty:
        return 0
    frame = frame.copy()
    frame["coverage_id"] = [
        _coverage_id(row.source, row.universe_id, row.item_id, row.basis, row.fiscal_year)
        for row in frame.itertuples(index=False)
    ]
    frame["as_of_date"] = None
    frame["available_at"] = None
    frame["run_id"] = options.run_id
    with store.transaction():
        basis_placeholders = ", ".join("?" for _ in options.bases)
        delete_sql = (
            "DELETE FROM fundamental_item_coverage "
            "WHERE source = ? AND universe_id = ? "
            f"AND basis IN ({basis_placeholders})"
        )
        delete_params: list[Any] = [options.source, options.universe_id, *options.bases]
        if options.item_ids:
            item_placeholders = ", ".join("?" for _ in options.item_ids)
            delete_sql += f" AND item_id IN ({item_placeholders})"
            delete_params.extend(options.item_ids)
        store.con.execute(delete_sql, delete_params)
        store.con.register("_item_coverage_frame", frame)
        try:
            store.con.execute(
                """
                INSERT INTO fundamental_item_coverage (
                    coverage_id, source, universe_id, item_id, canonical_code, basis,
                    fiscal_year, n_securities, n_with_value, coverage_pct,
                    as_of_date, available_at, run_id, source_loaded_at
                )
                SELECT
                    coverage_id, source, universe_id, item_id, canonical_code, basis,
                    fiscal_year, n_securities, n_with_value, coverage_pct,
                    CAST(as_of_date AS DATE), CAST(available_at AS TIMESTAMP), run_id, now()
                FROM _item_coverage_frame
                """
            )
        finally:
            store.con.unregister("_item_coverage_frame")
    return len(frame)


def evaluate_item_coverage_gate(
    frame: pd.DataFrame,
    *,
    minimum_items: int = ITEM_COVERAGE_TARGET_ITEMS,
    minimum_coverage_pct: float = ITEM_COVERAGE_TARGET_PCT,
    minimum_fiscal_year: int = ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR,
    basis: str = "annual",
) -> dict[str, Any]:
    """Count items clearing the spec threshold in every in-scope fiscal year.

    The spec gate ("at least 110 items with at least 90% coverage ... for
    FY2015+") is stated per fiscal year, i.e. over annual filings; it is
    evaluated on the ``basis`` slice alone (default ``"annual"``) so a thin
    quarterly/instant/ttm basis for the same item can never drag an
    otherwise-passing annual item below threshold, or vice versa. This must
    stay the same basis ``render_item_coverage_markdown`` renders by default,
    or the published "Observed: N items" summary will disagree with the
    visible table.
    """

    if frame.empty:
        return {
            "items_meeting_threshold": 0,
            "target_items": minimum_items,
            "target_coverage_pct": minimum_coverage_pct,
            "minimum_fiscal_year": minimum_fiscal_year,
            "basis": basis,
            "shortfall_items": minimum_items,
            "status": "degraded",
        }
    scoped = frame[(frame["fiscal_year"] >= minimum_fiscal_year) & (frame["basis"] == basis)]
    per_item = scoped.groupby("item_id")["coverage_pct"].min()
    meeting = int((per_item >= minimum_coverage_pct).sum())
    return {
        "items_meeting_threshold": meeting,
        "target_items": minimum_items,
        "target_coverage_pct": minimum_coverage_pct,
        "minimum_fiscal_year": minimum_fiscal_year,
        "basis": basis,
        "shortfall_items": max(0, minimum_items - meeting),
        "status": "passed" if meeting >= minimum_items else "degraded",
    }


def render_item_coverage_markdown(
    frame: pd.DataFrame,
    *,
    generated_from: str,
    bases: Sequence[str] = ("annual",),
) -> str:
    """Render a clock-free markdown coverage report."""

    gate = evaluate_item_coverage_gate(frame)
    lines = [
        "# Standardized item coverage",
        "",
        f"Generated by `{generated_from}`. Regenerate with "
        "`.venv\\Scripts\\python.exe scripts\\measure_item_coverage.py --write-docs`.",
        "",
        "## Spec gate",
        "",
        f"- Target: at least {gate['target_items']} items at or above "
        f"{gate['target_coverage_pct']}% coverage for fiscal years at or after "
        f"{gate['minimum_fiscal_year']} on the top-{ITEM_COVERAGE_TARGET_TOP_N} by market cap.",
        f"- Observed: {gate['items_meeting_threshold']} items. Shortfall: {gate['shortfall_items']}.",
        f"- Status: {gate['status']}.",
        "",
        "## Coverage by item and fiscal year",
        "",
        "| item_id | canonical_code | basis | fiscal_year | n_securities | n_with_value | coverage_pct |",
        "| ---: | --- | --- | ---: | ---: | ---: | ---: |",
    ]
    scoped = frame[frame["basis"].isin(list(bases))] if not frame.empty else frame
    for row in scoped.sort_values(["basis", "item_id", "fiscal_year"], kind="mergesort").itertuples(index=False):
        lines.append(
            f"| {row.item_id} | {row.canonical_code} | {row.basis} | {row.fiscal_year} | "
            f"{row.n_securities} | {row.n_with_value} | {row.coverage_pct:.2f} |"
        )
    lines.append("")
    return "\n".join(lines)
