"""Stage F vintage access (S4.6): as-of views, first-reported vs latest vintages, catalog macros.

Every view reads the published stage (``events.parquet``, ``quarterly_history.parquet``) and is a pure function of
the files and the as-of instant, so the same instant always returns the same rows (Compustat-Snapshot-style
``as_of``):

* ``events_as_of_sql(stage, t)``: the events visible at ``t`` (``available_at < t``; ``available_at`` =
  max(filing clock, FX rate clock)); ``latest_only`` keeps the issuer's latest visible event (the state a
  consumer sees at ``t``: ordered by ``available_at``, then ``clock_utc``, ``period_end``, ``accession``);
* ``history_as_of_sql(stage, t)``: per ``(cik, item, period_end, currency)`` the latest vintage with
  ``available_at < t`` (the quarterly value as then known);
* ``vintages_sql(stage)``: per ``(cik, item, period_end, currency)`` the first-reported value (earliest vintage) and
  the latest value, their accessions and clocks, the vintage count and ``restated`` (latest differs from first by
  more than ``RESTATE_REL`` relative);
* ``session_cutoff(d)``: the visibility instant of decision session ``d`` = 22:00 UTC of the previous session
  (the lake calendar when given, else the previous weekday); a row is visible at ``d`` iff
  ``available_at < session_cutoff(d)``;
* ``macros_sql(stage)``: ``CREATE OR REPLACE MACRO`` statements (``fund_events_asof(ts)``,
  ``fund_events_latest_asof(ts)``, ``fund_history_asof(ts)``, ``fund_vintages()``) for the serving catalog.

The S4.7 cutoff rebuild (``fund_validate cutoff``) compares ``events_as_of_sql`` / ``history_as_of_sql`` at each
cutoff with the issuer rebuilt from the facts filed before it.
"""

from __future__ import annotations

import bisect
import datetime as dt
from pathlib import Path

from . import common

RESTATE_REL = 0.005


def session_cutoff(d: dt.date, sessions: list[dt.date] | None = None) -> dt.datetime:
    if sessions:
        i = bisect.bisect_left(sessions, d) - 1
        prev = sessions[i] if i >= 0 else d - dt.timedelta(days=1)
    else:
        prev = d - dt.timedelta(days=1)
        while prev.weekday() >= 5:
            prev -= dt.timedelta(days=1)
    return dt.datetime(prev.year, prev.month, prev.day, common.MARK_HOUR_UTC)


def _ts(t: dt.datetime) -> str:
    return f"TIMESTAMP '{t.isoformat(sep=' ')}'"


def _cik_filter(ciks) -> str:
    return f" AND cik IN ({', '.join(str(int(c)) for c in ciks)})" if ciks else ""


def events_as_of_sql(stage: Path, as_of: dt.datetime | str, latest_only: bool = False, ciks=None) -> str:
    t = as_of if isinstance(as_of, str) else _ts(as_of)
    ev = (Path(stage) / "events.parquet").as_posix()
    q = f"SELECT * FROM read_parquet('{ev}') WHERE available_at < {t}{_cik_filter(ciks)}"
    if latest_only:
        q += (" QUALIFY row_number() OVER (PARTITION BY cik ORDER BY available_at DESC, clock_utc DESC, "
              "period_end DESC, accession DESC) = 1")
    return q + " ORDER BY cik, available_at, clock_utc, accession"


def history_as_of_sql(stage: Path, as_of: dt.datetime | str, ciks=None) -> str:
    t = as_of if isinstance(as_of, str) else _ts(as_of)
    h = (Path(stage) / "quarterly_history.parquet").as_posix()
    return (f"SELECT cik, item, period_end, fiscal_period, accession, value, currency, available_at, clock_basis, "
            f"zero_filled FROM read_parquet('{h}') WHERE available_at < {t}{_cik_filter(ciks)} "
            "QUALIFY row_number() OVER (PARTITION BY cik, item, period_end, currency "
            "ORDER BY available_at DESC, accession DESC) = 1 ORDER BY cik, item, period_end, currency")


def vintages_sql(stage: Path, ciks=None) -> str:
    h = (Path(stage) / "quarterly_history.parquet").as_posix()
    where = f"WHERE true{_cik_filter(ciks)}"
    return f"""
        SELECT cik, item, period_end, currency,
               arg_min(value, (available_at, accession)) AS first_value,
               arg_min(accession, (available_at, accession)) AS first_accession,
               min(available_at) AS first_available_at,
               arg_max(value, (available_at, accession)) AS latest_value,
               arg_max(accession, (available_at, accession)) AS latest_accession,
               max(available_at) AS latest_available_at,
               count(*) AS n_vintages,
               abs(arg_max(value, (available_at, accession)) - arg_min(value, (available_at, accession)))
                   > {RESTATE_REL} * greatest(abs(arg_min(value, (available_at, accession))), 1e-9) AS restated
        FROM read_parquet('{h}') {where}
        GROUP BY cik, item, period_end, currency ORDER BY cik, item, period_end, currency"""


def macros_sql(stage: Path) -> list[str]:
    ev = (Path(stage) / "events.parquet").as_posix()
    h = (Path(stage) / "quarterly_history.parquet").as_posix()
    return [
        f"CREATE OR REPLACE MACRO fund_events_asof(ts) AS TABLE SELECT * FROM read_parquet('{ev}') "
        "WHERE available_at < ts",
        f"CREATE OR REPLACE MACRO fund_events_latest_asof(ts) AS TABLE SELECT * FROM read_parquet('{ev}') "
        "WHERE available_at < ts QUALIFY row_number() OVER (PARTITION BY cik ORDER BY available_at DESC, "
        "clock_utc DESC, period_end DESC, accession DESC) = 1",
        f"CREATE OR REPLACE MACRO fund_history_asof(ts) AS TABLE SELECT * FROM read_parquet('{h}') "
        "WHERE available_at < ts QUALIFY row_number() OVER (PARTITION BY cik, item, period_end, currency "
        "ORDER BY available_at DESC, accession DESC) = 1",
        f"CREATE OR REPLACE MACRO fund_vintages() AS TABLE {vintages_sql(stage)}",
    ]
