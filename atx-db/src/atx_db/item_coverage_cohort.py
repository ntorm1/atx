"""SQL-only annual, point-in-time top-3000 common-equity coverage cohorts."""

from __future__ import annotations

import datetime as dt
import json
from dataclasses import dataclass

from .connection import DuckDBStore

ANNUAL_COVERAGE_UNIVERSE_ID = "us_common_equity_top3000_market_cap_annual_v1"
COHORT_SOURCE = "atx annual item coverage cohort v1"
COHORT_SIZE = 3000
COHORT_RULE = (
    "Last observed market session of calendar year; common stock with PIT US-listed "
    "membership and positive finite market cap known by session+22h; rank market cap "
    "descending, security_id ascending; retain 3000. Fiscal-year Y uses calendar-year Y."
)


@dataclass(frozen=True)
class AnnualCoverageCohortOptions:
    as_of_date: dt.date
    minimum_fiscal_year: int = 2015
    maximum_fiscal_year: int | None = None
    market_source: str = "atx-db daily market panel v1"
    listing_universe_id: str = "us_listed_v1"
    listing_source: str = "atx-db us-listed universe builder"
    run_id: str | None = None


def coverage_year_bounds(as_of_date: dt.date, minimum: int, maximum: int | None) -> tuple[int, int]:
    upper = as_of_date.year if maximum is None else maximum
    if not 1900 <= minimum <= upper <= as_of_date.year:
        raise ValueError("fiscal-year bounds must be ordered and no later than as_of_date")
    return minimum, upper


def refresh_item_coverage_cohort(store: DuckDBStore, options: AnnualCoverageCohortOptions) -> dict[str, object]:
    """Replace requested annual snapshots, including explicit missing/undersized years.

    Only aggregate year summaries cross into Python. The warehouse must already have
    migration 0311. No initialization, migration, or clock read is performed here.
    US-listed valid_to is the last included session, matching its writer/readers.
    source_loaded_at is the explicit as_of_date at 22h on every identical rerun;
    available_at remains the input-derived clock. No directory data is backdated.
    """
    lower, upper = coverage_year_bounds(options.as_of_date, options.minimum_fiscal_year, options.maximum_fiscal_year)
    cutoff = dt.datetime.combine(options.as_of_date, dt.time(22))
    rules = json.dumps(
        {
            "rule": COHORT_RULE,
            "top_n": COHORT_SIZE,
            "market_source": options.market_source,
            "listing_universe_id": options.listing_universe_id,
            "listing_source": options.listing_source,
        },
        sort_keys=True,
    )
    with store.transaction():
        store.con.execute(
            """
            CREATE OR REPLACE TEMP TABLE _coverage_candidates AS
            WITH sessions AS (
                SELECT year(trade_date)::INTEGER AS fiscal_year, max(trade_date) AS ranking_date
                FROM market_daily_metrics
                WHERE source=? AND trade_date<=? AND available_at<=?
                  AND year(trade_date) BETWEEN ? AND ?
                GROUP BY year(trade_date)
            ), market AS (
                SELECT s.fiscal_year, s.ranking_date, m.security_id, m.market_cap,
                       m.available_at AS market_available_at, m.market_daily_id
                FROM sessions s JOIN market_daily_metrics m ON m.trade_date=s.ranking_date
                WHERE m.source=? AND m.available_at<=s.ranking_date+INTERVAL 22 HOUR
                QUALIFY row_number() OVER (
                    PARTITION BY s.fiscal_year,m.security_id
                    ORDER BY m.available_at DESC,m.market_daily_id DESC
                )=1
            )
            SELECT m.*, l.membership_id, l.available_at AS listing_available_at,
                   CASE WHEN l.membership_id IS NULL THEN 'no_pit_common_listing'
                        WHEN m.market_cap IS NULL OR NOT isfinite(m.market_cap)
                             OR m.market_cap<=0 THEN 'invalid_market_cap'
                        ELSE 'eligible' END AS disposition
            FROM market m
            LEFT JOIN LATERAL (
                SELECT u.membership_id,u.available_at
                FROM universe_us_listed_membership u
                WHERE u.universe_id=? AND u.source=? AND u.security_id=m.security_id
                  AND u.valid_from<=m.ranking_date
                  AND (u.valid_to IS NULL OR m.ranking_date<=u.valid_to)
                  AND u.available_at<=m.ranking_date+INTERVAL 22 HOUR
                  AND u.security_type='common'
                  AND u.exchange_code IN ('XNAS','XNYS','XASE','ARCX','BATS')
                ORDER BY u.available_at DESC,u.valid_from DESC,u.membership_id DESC LIMIT 1
            ) l ON true
            """,
            [
                options.market_source,
                options.as_of_date,
                cutoff,
                lower,
                upper,
                options.market_source,
                options.listing_universe_id,
                options.listing_source,
            ],
        )
        store.con.execute(
            """CREATE OR REPLACE TEMP TABLE _coverage_ranked AS
            SELECT *, row_number() OVER (
                PARTITION BY fiscal_year ORDER BY market_cap DESC,security_id
            )::INTEGER AS market_cap_rank
            FROM _coverage_candidates WHERE disposition='eligible'"""
        )
        for table in ("item_coverage_annual_cohort", "item_coverage_cohort_years"):
            store.con.execute(
                f"DELETE FROM {table} WHERE universe_id=? AND fiscal_year BETWEEN ? AND ?",
                [ANNUAL_COVERAGE_UNIVERSE_ID, lower, upper],
            )
        # Even a same-date rebuild can change constituents. Invalidate measurements
        # atomically, so consumers cannot attach an old numerator to the new cohort.
        store.con.execute(
            "DELETE FROM fundamental_item_coverage WHERE universe_id=? AND fiscal_year BETWEEN ? AND ?",
            [ANNUAL_COVERAGE_UNIVERSE_ID, lower, upper],
        )
        store.con.execute(
            """
            INSERT INTO item_coverage_cohort_years
            WITH sessions AS (
                SELECT year(trade_date)::INTEGER AS fiscal_year,max(trade_date) ranking_date
                FROM market_daily_metrics WHERE source=? AND trade_date<=? AND available_at<=?
                    AND year(trade_date) BETWEEN ? AND ? GROUP BY year(trade_date)
            ), stats AS (
                SELECT fiscal_year,count(*) candidate_count,
                       count(*) FILTER (WHERE disposition='eligible') eligible_count,
                       count(*) FILTER (WHERE disposition='no_pit_common_listing') excluded_listing,
                       count(*) FILTER (WHERE disposition='invalid_market_cap') excluded_market_cap,
                       max(greatest(market_available_at,listing_available_at)) input_available_at
                FROM _coverage_candidates GROUP BY fiscal_year
            )
            SELECT ?,y::INTEGER,s.ranking_date,?, ?,?, ?,
                   coalesce(candidate_count,0),coalesce(eligible_count,0),
                   least(coalesce(eligible_count,0),3000),
                   coalesce(excluded_listing,0),coalesce(excluded_market_cap,0),
                   greatest(coalesce(eligible_count,0)-3000,0),y<year(CAST(? AS DATE)),
                   CASE WHEN y>=year(CAST(? AS DATE)) THEN 'incomplete_year'
                        WHEN s.ranking_date IS NULL THEN 'missing_session'
                        WHEN coalesce(eligible_count,0)<3000 THEN 'undersized'
                        ELSE 'complete' END,
                   greatest(s.ranking_date+INTERVAL 22 HOUR,input_available_at),?,?
            FROM range(?,?) years(y) LEFT JOIN sessions s ON s.fiscal_year=y
            LEFT JOIN stats c ON c.fiscal_year=y
            """,
            [
                options.market_source,
                options.as_of_date,
                cutoff,
                lower,
                upper,
                ANNUAL_COVERAGE_UNIVERSE_ID,
                options.as_of_date,
                COHORT_SOURCE,
                options.market_source,
                rules,
                options.as_of_date,
                options.as_of_date,
                options.run_id,
                cutoff,
                lower,
                upper + 1,
            ],
        )
        store.con.execute(
            """
            INSERT INTO item_coverage_annual_cohort
            SELECT ?,r.fiscal_year,r.security_id,r.ranking_date,r.market_cap_rank,r.market_cap,
                   r.market_daily_id,r.membership_id,r.market_available_at,r.listing_available_at,
                   y.available_at,?, ?,?,?
            FROM _coverage_ranked r JOIN item_coverage_cohort_years y
              ON y.universe_id=? AND y.fiscal_year=r.fiscal_year
            WHERE r.market_cap_rank<=3000
            """,
            [
                ANNUAL_COVERAGE_UNIVERSE_ID,
                options.as_of_date,
                COHORT_SOURCE,
                options.run_id,
                cutoff,
                ANNUAL_COVERAGE_UNIVERSE_ID,
            ],
        )
        store.con.execute("DROP TABLE _coverage_ranked")
        store.con.execute("DROP TABLE _coverage_candidates")
    years = store.con.execute(
        "SELECT fiscal_year,ranking_date,selected_count,status FROM item_coverage_cohort_years "
        "WHERE universe_id=? AND fiscal_year BETWEEN ? AND ? ORDER BY fiscal_year",
        [ANNUAL_COVERAGE_UNIVERSE_ID, lower, upper],
    ).fetchall()
    return {
        "universe_id": ANNUAL_COVERAGE_UNIVERSE_ID,
        "rows_written": sum(int(row[2]) for row in years),
        "years": [
            {"fiscal_year": row[0], "ranking_date": row[1], "selected_count": row[2], "status": row[3]} for row in years
        ],
    }
