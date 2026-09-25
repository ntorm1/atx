"""Bounded item coverage over an explicit annual PIT top-3000 cohort."""

from __future__ import annotations

import datetime as dt
import hashlib
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any, cast

import pandas as pd

from .connection import DuckDBStore
from .item_coverage_cohort import (
    ANNUAL_COVERAGE_UNIVERSE_ID,
    COHORT_RULE,
    COHORT_SIZE,
    coverage_year_bounds,
)

DEFAULT_SOURCE = "fundamental_standardization_v1"
DEFAULT_UNIVERSE_ID = ANNUAL_COVERAGE_UNIVERSE_ID
ITEM_COVERAGE_TARGET_ITEMS = 110
ITEM_COVERAGE_TARGET_PCT = 90.0
ITEM_COVERAGE_TARGET_TOP_N = COHORT_SIZE
ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR = 2015
ITEM_COVERAGE_GATE_BASIS = "annual"
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
    "as_of_date",
    "available_at",
    "cohort_status",
    "ranking_date",
)


@dataclass(frozen=True)
class ItemCoverageOptions:
    as_of_date: dt.date
    source: str = DEFAULT_SOURCE
    universe_id: str = DEFAULT_UNIVERSE_ID
    bases: tuple[str, ...] = ("annual", "quarterly", "instant", "ttm")
    item_ids: tuple[int, ...] = ()
    minimum_fiscal_year: int = ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR
    maximum_fiscal_year: int | None = None
    run_id: str | None = None

    def __post_init__(self) -> None:
        coverage_year_bounds(self.as_of_date, self.minimum_fiscal_year, self.maximum_fiscal_year)
        if not self.bases or any(b not in ("annual", "quarterly", "instant", "ttm") for b in self.bases):
            raise ValueError("at least one supported coverage basis is required")


def _years(options: ItemCoverageOptions) -> range:
    low, high = coverage_year_bounds(options.as_of_date, options.minimum_fiscal_year, options.maximum_fiscal_year)
    return range(low, high + 1)


def _selected_states(facts: pd.DataFrame) -> pd.DataFrame:
    """Latest visible state per revision group, chosen before fiscal-year attribution.

    Mirrors the SQL: a NULL group id is its own group, and the selected state's
    period-own ``fiscal_year`` (not an earlier state's) is the year it counts in.
    """
    if facts.empty or "revision_group_id" not in facts:
        return facts
    order = [c for c in ("available_at", "revision_sequence", "standardized_id") if c in facts]
    ranked = facts.sort_values(order, kind="mergesort") if order else facts
    fallback = pd.Series("row:" + ranked.index.astype(str), index=ranked.index)
    if "standardized_id" in ranked:
        fallback = ranked["standardized_id"].where(ranked["standardized_id"].notna(), fallback)
    group = ranked["revision_group_id"].where(ranked["revision_group_id"].notna(), fallback)
    return ranked[~group.duplicated(keep="last")]


def compute_item_coverage_rows(
    standardized: pd.DataFrame,
    universe: pd.DataFrame,
    options: ItemCoverageOptions,
    *,
    registry: pd.DataFrame | None = None,
    cohort_years: pd.DataFrame | None = None,
) -> pd.DataFrame:
    """Small-fixture transform; production uses SQL, never a full fact DataFrame.

    The registry argument includes items absent from facts. Membership matches the
    exact (security_id,fiscal_year) pair; a rejected interval is never a member.
    Cohort evidence is required for a passing gate; counts alone cannot certify it.
    """
    members = universe.copy()
    if "is_member" in members:
        members = members[members["is_member"].fillna(False).astype(bool)]
    members = members.reindex(columns=["security_id", "fiscal_year"]).drop_duplicates()
    counts = members.groupby("fiscal_year")["security_id"].nunique().to_dict()
    items = (
        (registry if registry is not None else standardized)
        .reindex(columns=["item_id", "canonical_code"])
        .drop_duplicates("item_id")
    )
    labels = {int(cast(Any, row.item_id)): str(row.canonical_code) for row in items.itertuples(index=False)}
    requested = sorted(set(options.item_ids)) or sorted(labels)
    facts = standardized.copy()
    if not facts.empty:
        if "available_at" in facts:
            facts = facts[
                pd.to_datetime(facts["available_at"]) <= pd.Timestamp(options.as_of_date) + pd.Timedelta(hours=22)
            ]
        facts = _selected_states(facts)
        facts = facts.merge(members, on=["security_id", "fiscal_year"], how="inner")
        facts = facts[facts["basis"].isin(options.bases) & facts["value"].notna()]
        if "fiscal_period" in facts:
            facts = facts[facts["basis"].ne("annual") | facts["fiscal_period"].eq("FY")]
    evidence = {} if cohort_years is None else {int(row["fiscal_year"]): row for row in cohort_years.to_dict("records")}
    records = []
    for basis in sorted(set(options.bases)):
        for item_id in requested:
            for year in _years(options):
                selected = (
                    facts[(facts["basis"] == basis) & (facts["item_id"] == item_id) & (facts["fiscal_year"] == year)]
                    if not facts.empty
                    else facts
                )
                n = int(counts.get(year, 0))
                k = int(selected["security_id"].nunique()) if not selected.empty else 0
                meta = evidence.get(year, {})
                status = str(meta.get("status", "unverified"))
                if year == options.as_of_date.year:
                    status = "incomplete_year"
                clocks = [meta.get("available_at")]
                if not selected.empty and "available_at" in selected:
                    clocks.append(selected["available_at"].max())
                known_clocks = [pd.Timestamp(c) for c in clocks if c is not None and not pd.isna(c)]
                records.append(
                    {
                        "source": options.source,
                        "universe_id": options.universe_id,
                        "item_id": item_id,
                        "canonical_code": labels.get(item_id, f"item_{item_id}"),
                        "basis": basis,
                        "fiscal_year": year,
                        "n_securities": n,
                        "n_with_value": k,
                        "coverage_pct": round(100.0 * k / n, 6) if n else 0.0,
                        "as_of_date": options.as_of_date,
                        "available_at": max(known_clocks) if known_clocks else None,
                        "cohort_status": status,
                        "ranking_date": meta.get("ranking_date"),
                    }
                )
    return pd.DataFrame(records, columns=list(ITEM_COVERAGE_COLUMNS))


def measure_item_coverage(store: DuckDBStore, options: ItemCoverageOptions) -> pd.DataFrame:
    """Return only item x basis x year aggregates; source facts remain inside DuckDB."""
    years = _years(options)
    cutoff = dt.datetime.combine(options.as_of_date, dt.time(22))
    params: list[object] = [years.start, years.stop]
    if options.universe_id == DEFAULT_UNIVERSE_ID:
        membership = """
            SELECT c.security_id,c.fiscal_year,c.available_at
            FROM item_coverage_annual_cohort c JOIN years y USING(fiscal_year)
            WHERE c.universe_id=? AND c.as_of_date=? AND c.available_at<=?
        """
        params.extend([options.universe_id, options.as_of_date, cutoff])
        metadata = """SELECT fiscal_year,status,ranking_date,available_at
            FROM item_coverage_cohort_years WHERE universe_id=? AND as_of_date=?"""
        params.extend([options.universe_id, options.as_of_date])
    else:
        # Both legacy and US-listed membership valid_to dates are inclusive.
        membership = """
            SELECT DISTINCT u.security_id,y.fiscal_year,u.available_at
            FROM universe_membership u CROSS JOIN years y
            WHERE u.universe_id=? AND u.is_member AND u.is_latest_revision
              AND u.valid_from<=make_date(y.fiscal_year,12,31)
              AND (u.valid_to IS NULL OR u.valid_to>=make_date(y.fiscal_year,1,1))
              AND u.available_at<=?
        """
        params.extend([options.universe_id, cutoff])
        metadata = """SELECT fiscal_year,'non_authoritative_cohort' AS status,
            NULL::DATE AS ranking_date,NULL::TIMESTAMP AS available_at FROM years"""
    item_filter = ""
    if options.item_ids:
        item_filter = " WHERE item_id IN (" + ",".join("?" for _ in options.item_ids) + ")"
        params.extend(options.item_ids)
    bases_values = ",".join("(?)" for _ in sorted(set(options.bases)))
    params.extend(sorted(set(options.bases)))
    params.extend(
        [
            options.source,
            cutoff,
            years.start - 2,
            years.stop + 1,  # last requested year + 2 (range stop is exclusive)
            options.source,
            options.universe_id,
            options.as_of_date,
            options.as_of_date.year,
        ]
    )
    return store.con.execute(
        f"""
        WITH years AS (SELECT range::INTEGER AS fiscal_year FROM range(?,?)),
        members AS ({membership}), metadata AS ({metadata}),
        items AS (SELECT item_id,canonical_code FROM fundamental_item{item_filter}),
        bases(basis) AS (VALUES {bases_values}),
        denominators AS (
            SELECT fiscal_year,count(DISTINCT security_id) AS n,max(available_at) AS available_at
            FROM members GROUP BY fiscal_year
        ), visible_states AS (
            SELECT f.security_id,f.item_id,f.basis,f.fiscal_year,f.fiscal_period,f.value,f.available_at,
                   row_number() OVER (
                       -- State selection precedes attribution and usability
                       -- filtering.  A later NULL reported-EPS conflict shares
                       -- this revision group with the earlier release and must
                       -- remove that formerly valid value from coverage, and the
                       -- selected state's period-own fiscal_year -- never a
                       -- re-reporting filing's label -- names the year it counts in.
                       PARTITION BY coalesce(f.revision_group_id,f.standardized_id)
                       ORDER BY f.available_at DESC,f.revision_sequence DESC,f.standardized_id DESC
                   ) AS revision_rank
            FROM fundamental_standardized f
            JOIN items i ON i.item_id=f.item_id JOIN bases b ON b.basis=f.basis
            WHERE f.source=? AND f.available_at<=?
              AND f.security_id IN (SELECT security_id FROM members)
              -- period_end is constant within a revision group and a period-own
              -- fiscal year lies within year(period_end)+-1; with a further year
              -- of margin this bound never splits a group or drops a requested year.
              AND f.period_end BETWEEN make_date(?,1,1) AND make_date(?,12,31)
        ), eligible_facts AS (
            SELECT s.security_id,s.item_id,s.basis,s.fiscal_year,s.value,s.available_at
            FROM visible_states s
            JOIN (SELECT DISTINCT security_id,fiscal_year FROM members) u
              ON s.security_id=u.security_id AND s.fiscal_year=u.fiscal_year
            WHERE s.revision_rank=1
              -- Annual coverage is the fiscal year itself, never an off-cycle
              -- twelve-months-ended column labelled by its end quarter.
              AND (s.basis<>'annual' OR s.fiscal_period='FY')
        ), numerators AS (
            SELECT item_id,basis,fiscal_year,count(DISTINCT security_id) AS k,
                   max(available_at) AS available_at
            FROM eligible_facts
            WHERE value IS NOT NULL AND isfinite(value)
            GROUP BY item_id,basis,fiscal_year
        )
        SELECT ? AS source,? AS universe_id,i.item_id,i.canonical_code,b.basis,y.fiscal_year,
               coalesce(d.n,0)::BIGINT AS n_securities,coalesce(n.k,0)::BIGINT AS n_with_value,
               coalesce(round(100.0*n.k/nullif(d.n,0),6),0.0) AS coverage_pct,
               CAST(? AS DATE) AS as_of_date,
               greatest(d.available_at,n.available_at,m.available_at) AS available_at,
               CASE WHEN y.fiscal_year>=? THEN 'incomplete_year'
                    ELSE coalesce(m.status,'missing_cohort') END AS cohort_status,m.ranking_date
        FROM items i CROSS JOIN bases b CROSS JOIN years y
        LEFT JOIN denominators d USING(fiscal_year) LEFT JOIN metadata m USING(fiscal_year)
        LEFT JOIN numerators n ON n.item_id=i.item_id AND n.basis=b.basis AND n.fiscal_year=y.fiscal_year
        ORDER BY b.basis,i.item_id,y.fiscal_year
        """,
        params,
    ).df()


def refresh_item_coverage(
    store: DuckDBStore, options: ItemCoverageOptions, *, frame: pd.DataFrame | None = None
) -> int:
    """Replace precisely requested years/bases/items, even when the new slice is empty."""
    years = _years(options)
    frame = measure_item_coverage(store, options) if frame is None else frame.copy()
    if not frame.empty:
        if not (
            frame["source"].eq(options.source)
            & frame["universe_id"].eq(options.universe_id)
            & frame["basis"].isin(options.bases)
            & frame["fiscal_year"].isin(years)
        ).all():
            raise ValueError("coverage frame is outside the requested replacement slice")
        if options.item_ids and not frame["item_id"].isin(options.item_ids).all():
            raise ValueError("coverage frame contains unrequested items")
        frame["coverage_id"] = [
            hashlib.sha256(f"{r.source}|{r.universe_id}|{r.item_id}|{r.basis}|{r.fiscal_year}".encode()).hexdigest()
            for r in frame.itertuples(index=False)
        ]
        frame["run_id"] = options.run_id
    with store.transaction():
        delete_sql = (
            "DELETE FROM fundamental_item_coverage WHERE source=? AND universe_id=? "
            "AND fiscal_year BETWEEN ? AND ? AND basis IN (" + ",".join("?" for _ in options.bases) + ")"
        )
        params: list[object] = [options.source, options.universe_id, years.start, years.stop - 1, *options.bases]
        if options.item_ids:
            delete_sql += " AND item_id IN (" + ",".join("?" for _ in options.item_ids) + ")"
            params.extend(options.item_ids)
        store.con.execute(delete_sql, params)
        if not frame.empty:
            columns = ",".join(("coverage_id", *ITEM_COVERAGE_COLUMNS, "run_id"))
            store.con.register("_item_coverage_frame", frame)
            try:
                store.con.execute(
                    f"INSERT INTO fundamental_item_coverage ({columns}) SELECT {columns} FROM _item_coverage_frame"
                )
            finally:
                store.con.unregister("_item_coverage_frame")
    return len(frame)


def evaluate_item_coverage_gate(
    frame: pd.DataFrame,
    *,
    as_of_date: dt.date | None = None,
    minimum_items: int = ITEM_COVERAGE_TARGET_ITEMS,
    minimum_coverage_pct: float = ITEM_COVERAGE_TARGET_PCT,
    minimum_fiscal_year: int = ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR,
    basis: str = ITEM_COVERAGE_GATE_BASIS,
    source: str = DEFAULT_SOURCE,
) -> dict[str, Any]:
    """Count items meeting the threshold in EVERY explicitly completed fiscal year."""
    if as_of_date is None and "as_of_date" in frame and frame["as_of_date"].notna().any():
        as_of_date = pd.Timestamp(frame["as_of_date"].max()).date()
    expected = set(range(minimum_fiscal_year, as_of_date.year)) if as_of_date else set()
    meeting = 0
    missing = sorted(expected)
    incomplete: list[int] = []
    required = {
        "fiscal_year",
        "basis",
        "item_id",
        "n_securities",
        "n_with_value",
        "coverage_pct",
        "cohort_status",
        "universe_id",
        "as_of_date",
        "source",
    }
    if as_of_date is not None and expected and required.issubset(frame.columns):
        scoped = frame[
            frame["basis"].eq(basis)
            & frame["fiscal_year"].isin(expected)
            & frame["universe_id"].eq(DEFAULT_UNIVERSE_ID)
            & frame["source"].eq(source)
            & pd.to_datetime(frame["as_of_date"]).dt.year.eq(as_of_date.year)
            & pd.to_datetime(frame["as_of_date"]).le(pd.Timestamp(as_of_date))
        ]
        missing = sorted(expected - set(scoped["fiscal_year"]))
        valid = (
            scoped["n_securities"].eq(COHORT_SIZE)
            & scoped["cohort_status"].eq("complete")
            & scoped["n_with_value"].between(0, COHORT_SIZE)
            & (scoped["coverage_pct"] - 100.0 * scoped["n_with_value"] / COHORT_SIZE).abs().le(0.000001)
        )
        incomplete = sorted(set(scoped.loc[~valid, "fiscal_year"]))
        for _, rows in scoped.groupby("item_id"):
            if (
                set(rows["fiscal_year"]) == expected
                and not rows["fiscal_year"].duplicated().any()
                and valid.loc[rows.index].all()
                and rows["coverage_pct"].ge(minimum_coverage_pct).all()
            ):
                meeting += 1
    return {
        "items_meeting_threshold": meeting,
        "target_items": minimum_items,
        "target_coverage_pct": minimum_coverage_pct,
        "minimum_fiscal_year": minimum_fiscal_year,
        "completed_fiscal_years": sorted(expected),
        "as_of_date": as_of_date,
        "missing_years": missing,
        "incomplete_cohort_years": incomplete,
        "basis": basis,
        "shortfall_items": max(0, minimum_items - meeting),
        "status": "passed" if expected and meeting >= minimum_items else "degraded",
    }


def coverage_gate_count_sql(
    *,
    source: str = DEFAULT_SOURCE,
    universe_id: str = DEFAULT_UNIVERSE_ID,
    as_of_date: dt.date | None = None,
    minimum_fiscal_year: int = 2015,
    target_pct: float = ITEM_COVERAGE_TARGET_PCT,
    basis: str = ITEM_COVERAGE_GATE_BASIS,
) -> str:
    """One shared SQL gate for provider SLOs and DQC; absent years cannot pass."""

    def literal(value: str) -> str:
        return "'" + value.replace("'", "''") + "'"

    measured = f"max(DATE '{as_of_date.isoformat()}')" if as_of_date else "max(as_of_date)"
    return f"""
        WITH measured AS (
            SELECT {measured} AS as_of_date FROM fundamental_item_coverage
            WHERE source={literal(source)} AND universe_id={literal(universe_id)}
        ), passing AS (
            SELECT c.item_id
            FROM fundamental_item_coverage c CROSS JOIN measured m
            JOIN item_coverage_cohort_years y ON y.universe_id=c.universe_id
                AND y.fiscal_year=c.fiscal_year AND y.as_of_date=c.as_of_date
            WHERE c.source={literal(source)} AND c.universe_id={literal(DEFAULT_UNIVERSE_ID)}
                AND c.universe_id={literal(universe_id)} AND c.basis={literal(basis)}
                AND c.fiscal_year BETWEEN {minimum_fiscal_year} AND year(m.as_of_date)-1
                AND year(c.as_of_date)=year(m.as_of_date) AND c.as_of_date<=m.as_of_date
            GROUP BY c.item_id,m.as_of_date
            HAVING count(*)=year(m.as_of_date)-{minimum_fiscal_year}
                AND count(DISTINCT c.fiscal_year)=year(m.as_of_date)-{minimum_fiscal_year}
                AND bool_and(coalesce(c.cohort_status='complete' AND y.status='complete'
                    AND y.selected_count=3000 AND c.n_securities=3000
                    AND c.n_with_value BETWEEN 0 AND 3000
                    AND abs(c.coverage_pct-100.0*c.n_with_value/3000)<=0.000001
                    AND c.coverage_pct>={target_pct},false))
        ) SELECT count(*)::BIGINT FROM passing
    """


def render_item_coverage_markdown(
    frame: pd.DataFrame,
    *,
    generated_from: str,
    bases: Sequence[str] = ("annual",),
    as_of_date: dt.date | None = None,
) -> str:
    gate = evaluate_item_coverage_gate(frame, as_of_date=as_of_date)
    cohorts = sorted(set(frame["universe_id"])) if not frame.empty else [DEFAULT_UNIVERSE_ID]
    lines = [
        "# Standardized item coverage",
        "",
        f"Generated by `{generated_from}`.",
        "",
        f"Cohort: {', '.join(f'`{c}`' for c in cohorts)}.",
        f"Authoritative rule: {COHORT_RULE}",
        "Other cohorts are descriptive and cannot satisfy this gate.",
        "",
        f"As of: {gate['as_of_date']}. Completed fiscal years: {gate['completed_fiscal_years']}.",
        "The current year is incomplete and excluded from the completed-year gate.",
        f"Target: {gate['target_items']} annual items with at least {gate['target_coverage_pct']}% "
        "coverage in every completed FY2015+ year; each cohort must contain exactly 3000 names.",
        f"Observed: {gate['items_meeting_threshold']} items; status: {gate['status']}.",
        f"Missing years: {gate['missing_years']}; incomplete cohorts: {gate['incomplete_cohort_years']}.",
        "",
        "| item_id | canonical_code | basis | fiscal_year | n_securities | n_with_value | coverage_pct | cohort_status |",
        "| ---: | --- | --- | ---: | ---: | ---: | ---: | --- |",
    ]
    scoped = frame[frame["basis"].isin(bases)] if not frame.empty else frame
    for row in scoped.sort_values(["basis", "item_id", "fiscal_year"]).itertuples(index=False):
        lines.append(
            f"| {row.item_id} | {row.canonical_code} | {row.basis} | {row.fiscal_year} | "
            f"{row.n_securities} | {row.n_with_value} | {row.coverage_pct:.2f} | {row.cohort_status} |"
        )
    return "\n".join([*lines, ""])
