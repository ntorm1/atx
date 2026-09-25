"""Point-in-time split epochs for share-basis rebasing in the derived engine.

A stock split changes every per-share value and share count reported after it.
A filing states all of its values on the share basis in force when it is
issued (ASC 260-10-55-12, SAB Topic 4C restate for splits before issuance), so
a derived state's basis is fixed by its availability clock. Two operands filed
at different clocks are compared on one basis by rebasing each to the frame's
clock with the splits that became known in between.

Detection (reconstructed basis: the vendor factor comes from a later snapshot,
but a split is applied only from its ``available_at``, never before):

- a day-over-day change k of ``adjusted_close / close`` on ``equity_daily_bars``
  (the cumulative split and dividend factor) outside [SPLIT_MIN_RATIO,
  SPLIT_MAX_RATIO] is a split of k new shares per old share, available at the
  ex-date bar's ``available_at``; dividends move the factor by a few percent;
- a factor step of at least STOCK_DIVIDEND_MIN_STEP inside that band (e.g. a
  6:5 split) is a split only when the archive share count moves by the same
  ratio within SHARE_CORROBORATION_TOLERANCE over the next _SHARES_WINDOW_BARS
  bars; it is available only once that whole share window has been observed;
- with no factor on the step, an archive share-count jump outside the band
  with a continuous market value (price moves inversely) is a split available
  at that bar.

``evidence`` labels each event for audit only and never gates a rebase:
``vendor_factor``, ``vendor_factor+shares`` (the share count corroborates the
factor; for out-of-band steps this label is assigned with share observations
up to _SHARES_WINDOW_BARS bars after the event's availability) and
``shares+price`` (no factor).

Coverage is a run of bars with a factor and no gap above COVERAGE_MAX_GAP_DAYS:
only inside it would a split have been seen. A basis is unknown when coverage
does not span from the operand's clock to the frame's clock (less
COVERAGE_GRACE_DAYS for the latest bar), or when a split known by the frame's
clock went ex within AMBIGUOUS_EPOCH_DAYS of the operand's clock (whether that
filing is restated is uncertain). Share-count evidence alone never proves
coverage.
"""

from __future__ import annotations

from typing import Any

__all__ = [
    "AMBIGUOUS_EPOCH_DAYS",
    "COVERAGE_GRACE_DAYS",
    "COVERAGE_MAX_GAP_DAYS",
    "SHARE_CORROBORATION_TOLERANCE",
    "SPLIT_MAX_RATIO",
    "SPLIT_MIN_RATIO",
    "STOCK_DIVIDEND_MIN_STEP",
    "basis_known_sql",
    "cleanup_split_epochs",
    "prepare_split_epochs",
    "rebase_factor_sql",
]

SPLIT_MIN_RATIO = 0.8
SPLIT_MAX_RATIO = 1.25
STOCK_DIVIDEND_MIN_STEP = 0.05
SHARE_CORROBORATION_TOLERANCE = 0.1
COVERAGE_MAX_GAP_DAYS = 10
COVERAGE_GRACE_DAYS = 4
AMBIGUOUS_EPOCH_DAYS = 5
_SHARES_WINDOW_BARS = 20
_EVENTS = "_pit_split_events"
_COVERAGE = "_pit_split_coverage"


def _bars_available(con: Any) -> bool:
    columns = {row[0] for row in con.execute(
        "SELECT column_name FROM duckdb_columns() WHERE table_name = 'equity_daily_bars' "
        "AND NOT internal").fetchall()}
    return {"security_id", "trade_date", "close", "adjusted_close"} <= columns


def _empty(con: Any) -> None:
    con.execute(f"""CREATE OR REPLACE TEMP TABLE {_EVENTS} (
        ex_date DATE, available_at TIMESTAMP, ratio DOUBLE, factor_ratio DOUBLE,
        share_ratio DOUBLE, evidence VARCHAR)""")
    con.execute(f"CREATE OR REPLACE TEMP TABLE {_COVERAGE} (first_at TIMESTAMP, last_at TIMESTAMP)")


def prepare_split_epochs(con: Any, security_id: str) -> int:
    """Stage one security's split events and factor coverage; return the event count.

    Without a bars table (or with one lacking the price pair) both relations are
    empty: every cross-clock share-basis comparison is then unproven.
    """
    if not _bars_available(con):
        _empty(con)
        return 0
    columns = {row[0] for row in con.execute(
        "SELECT column_name FROM duckdb_columns() WHERE table_name = 'equity_daily_bars'").fetchall()}
    shares = "shares_outstanding" if "shares_outstanding" in columns else "NULL::DOUBLE"
    clock = ("coalesce(available_at, trade_date::TIMESTAMP + INTERVAL 22 HOUR)" if "available_at" in columns
             else "trade_date::TIMESTAMP + INTERVAL 22 HOUR")
    order = "available_at DESC NULLS LAST, source" if {"available_at", "source"} <= columns else "trade_date"
    band = f"BETWEEN {SPLIT_MIN_RATIO} AND {SPLIT_MAX_RATIO}"
    near = f"abs(share_ratio / factor_ratio - 1) <= {SHARE_CORROBORATION_TOLERANCE}"
    window = f"ORDER BY trade_date ROWS BETWEEN CURRENT ROW AND {_SHARES_WINDOW_BARS} FOLLOWING"
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _pit_split_bars AS
        SELECT trade_date, CAST(close AS DOUBLE) AS close, CAST({shares} AS DOUBLE) AS shares,
               {clock} AS available_at,
               CASE WHEN close > 0 AND adjusted_close > 0 AND isfinite(close) AND isfinite(adjusted_close)
                    THEN adjusted_close / close END AS factor
        FROM equity_daily_bars
        WHERE security_id = ? AND trade_date IS NOT NULL
        QUALIFY row_number() OVER (PARTITION BY trade_date ORDER BY {order}) = 1
    """, [security_id])
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE {_EVENTS} AS
        WITH steps AS (
            SELECT trade_date, available_at, factor, close, shares,
                   lag(trade_date) OVER w AS prior_date, lag(factor) OVER w AS prior_factor,
                   lag(close) OVER w AS prior_close, lag(shares) OVER w AS prior_shares,
                   last_value(shares IGNORE NULLS) OVER (ORDER BY trade_date
                       ROWS BETWEEN {_SHARES_WINDOW_BARS} PRECEDING AND 1 PRECEDING) AS shares_before,
                   last_value(shares IGNORE NULLS) OVER ({window}) AS shares_after,
                   last_value(CASE WHEN shares IS NOT NULL THEN available_at END IGNORE NULLS)
                       OVER ({window}) AS shares_after_at
            FROM _pit_split_bars WINDOW w AS (ORDER BY trade_date)
        ), candidates AS (
            SELECT trade_date AS ex_date, available_at, shares_after_at,
                   CASE WHEN prior_factor IS NOT NULL AND factor IS NOT NULL THEN factor / prior_factor END
                       AS factor_ratio,
                   CASE WHEN shares_before > 0 AND shares_after > 0 THEN shares_after / shares_before END
                       AS share_ratio,
                   CASE WHEN prior_shares > 0 AND shares > 0 THEN shares / prior_shares END AS step_share_ratio,
                   CASE WHEN prior_close > 0 AND close > 0 THEN close / prior_close END AS price_ratio
            FROM steps
            WHERE prior_date IS NOT NULL AND date_diff('day', prior_date, trade_date) <= {COVERAGE_MAX_GAP_DAYS}
        )
        SELECT ex_date,
               CASE WHEN NOT (factor_ratio {band}) THEN available_at
                    WHEN factor_ratio IS NOT NULL THEN greatest(available_at, shares_after_at)
                    ELSE available_at END AS available_at,
               coalesce(factor_ratio, step_share_ratio) AS ratio, factor_ratio,
               coalesce(share_ratio, step_share_ratio) AS share_ratio,
               CASE WHEN factor_ratio IS NULL THEN 'shares+price'
                    WHEN share_ratio IS NOT NULL AND {near} THEN 'vendor_factor+shares'
                    ELSE 'vendor_factor' END AS evidence
        FROM candidates
        WHERE NOT (factor_ratio {band})
           OR (abs(factor_ratio - 1) >= {STOCK_DIVIDEND_MIN_STEP} AND abs(share_ratio - 1) >= {STOCK_DIVIDEND_MIN_STEP}
               AND {near} AND shares_after_at IS NOT NULL)
           OR (factor_ratio IS NULL AND NOT (step_share_ratio {band})
               AND step_share_ratio * price_ratio {band})
    """)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE {_COVERAGE} AS
        WITH factored AS (
            SELECT trade_date, available_at,
                   CASE WHEN date_diff('day', lag(trade_date) OVER (ORDER BY trade_date), trade_date)
                             <= {COVERAGE_MAX_GAP_DAYS} THEN 0 ELSE 1 END AS starts_run
            FROM _pit_split_bars WHERE factor IS NOT NULL
        ), runs AS (
            SELECT available_at, sum(starts_run) OVER (ORDER BY trade_date) AS run FROM factored
        )
        SELECT min(available_at) AS first_at, max(available_at) AS last_at FROM runs GROUP BY run
    """)
    con.execute("DROP TABLE IF EXISTS _pit_split_bars")
    return int(con.execute(f"SELECT count(*) FROM {_EVENTS}").fetchone()[0])


def cleanup_split_epochs(con: Any) -> None:
    for table in (_EVENTS, _COVERAGE, "_pit_split_bars"):
        con.execute(f"DROP TABLE IF EXISTS {table}")


def rebase_factor_sql(clock: str, frame_clock: str, exponent: int) -> str:
    """Multiplier that restates a value filed at ``clock`` on the basis at ``frame_clock``.

    A split of ratio k (new shares per old share) divides per-share values
    (exponent -1) and multiplies share counts (exponent +1). Only splits known
    by the frame's clock are applied.
    """
    return (f"exp({exponent} * coalesce((SELECT sum(ln(s.ratio)) FROM {_EVENTS} s "
            f"WHERE s.available_at > ({clock}) AND s.available_at <= ({frame_clock})), 0))")


def basis_known_sql(clock: str, frame_clock: str) -> str:
    """Whether every split between ``clock`` and ``frame_clock`` would have been seen."""
    return (f"(EXISTS (SELECT 1 FROM {_COVERAGE} c WHERE c.first_at <= ({clock}) "
            f"AND c.last_at >= ({frame_clock}) - INTERVAL {COVERAGE_GRACE_DAYS} DAY) "
            f"AND NOT EXISTS (SELECT 1 FROM {_EVENTS} s WHERE s.available_at <= ({frame_clock}) "
            f"AND abs(date_diff('day', s.ex_date, CAST(({clock}) AS DATE))) <= {AMBIGUOUS_EPOCH_DAYS}))")
