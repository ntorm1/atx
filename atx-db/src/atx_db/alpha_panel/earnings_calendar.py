"""Stage ``earnings_calendar`` (D2): earnings announcements from 8-K item 2.02, with timing and expected dates.

Input: the published ``sec_filings`` stage (``eight_k_items.parquet``, ``filings.parquet``) and the session
calendar (``calendar.parquet``). Validation uses SEC evidence only (``sec_validation``: 8-K event dates, 10-Q/10-K
report dates and filing order, EDGAR index pages in sec_filings). The vendor TickerHistory3 earnFlag is known
unreliable (owner ruling 2026-09-28): the manifest keeps a labelled ``vendor_flag_comparison`` (``prices/``
``earn_flag`` on ``identity/links_combined.parquet`` lines) for information only; it is not a validation and no rule
uses it.

One row per (cik, accession) of an original (non-amended) 8-K carrying item 2.02:

* ``announcement_utc`` = the 8-K's EDGAR acceptance (UTC); ``available_at`` = ``announcement_utc``.
* ``timing`` from the acceptance in America/New_York on the NYSE calendar: ``pre_market`` (< 09:30),
  ``intraday`` (09:30 to the close: 16:00, 13:00 on early-close days), ``post_market`` (>= the close), and
  ``closed_day`` (accepted on a day without a session, e.g. Good Friday, when EDGAR is open).
* ``session_date`` = the announcement's ET date when it is a session, else the next session.
  ``reaction_session`` = the first session whose close reflects the announcement: ``session_date`` for
  pre-market and intraday, the next session for post-market; for ``closed_day`` it is ``session_date``.
* ``fiscal_period_end`` = the latest original 10-Q/10-K/10-QT/10-KT ``report_date`` <= the announcement's ET date
  (any filing time: a label, not a signal); ``period_lag_days`` = ET date - period end. ``is_primary`` marks the
  first announcement per (cik, fiscal_period_end) with lag <= ``MAX_PERIOD_LAG_DAYS`` that is not a second
  2.02 on the same reaction session.
* Expected dates (``EXPECTED_RULE``), PIT by construction:
  - ``next_expected_date`` on a primary row = the first session on or after ``session_date + 364 days``
    (same fiscal quarter next year), known at the row's own ``available_at``.
  - ``expected_date`` on a primary row = the ``next_expected_date`` of the year-ago primary announcement
    (fiscal period end 345-385 days earlier, closest to 365), ``expected_available_at`` = that announcement's
    acceptance; fallback ``prev_primary_plus_91``: first session on or after the previous primary
    announcement's session + 91 days. ``expected_error_sessions`` / ``_days`` = actual ``session_date`` minus
    ``expected_date``.

Sessions come from ``calendar.parquet`` (vendor sessions, 2012-03-26..2026-09-18) and outside its range from the
NYSE rule calendar (``nyse_holidays``), validated against the vendor calendar where both exist
(``session_basis``). No statistic here uses returns.
"""

from __future__ import annotations

import argparse
import bisect
import datetime as dt
import json
import os
import sys
from collections.abc import Iterable
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo

import pyarrow as pa
import pyarrow.parquet as pq

from . import common as C
from . import sec_filings as SF

STAGE = "earnings_calendar"
SCHEMA = "atx.alpha-panel.earnings-calendar/v1"
ET = ZoneInfo("America/New_York")
UTC = dt.timezone.utc
PERIODIC_FORMS = ("10-Q", "10-K", "10-QT", "10-KT")
MAX_PERIOD_LAG_DAYS = 120
YOY_DAYS = 364
YOY_MATCH_DAYS = (345, 385)
QOQ_DAYS = 91
AGREE_WINDOW = 5
OPEN = dt.time(9, 30)
CLOSE = dt.time(16, 0)
EARLY_CLOSE = dt.time(13, 0)
RULE_START = dt.date(2007, 1, 1)
RULE_END = dt.date(2028, 12, 31)
DUCKDB_MEMORY = os.environ.get("ATX_EARN_DUCKDB_MEMORY", "400MB")
VENDOR_LABEL = ("vendor flag, known unreliable, not a validation: the TickerHistory3 earnFlag has bad logic "
                "(owner ruling 2026-09-28); it is not used as a target or reference and no rule here was tuned or "
                "accepted on it; outputs carry no vendor-derived column")
EXPECTED_RULE = ("yoy_364: first session on or after the year-ago primary announcement's session_date + 364 days "
                 "(year-ago = primary announcement whose fiscal_period_end is 345-385 days earlier, closest to "
                 "365); fallback prev_primary_plus_91: first session on or after the previous primary "
                 "announcement's session_date + 91 days")
# NYSE unscheduled closures inside the rule range.
SPECIAL_CLOSURES = frozenset({
    dt.date(2007, 1, 2),    # President Ford
    dt.date(2012, 10, 29), dt.date(2012, 10, 30),  # Hurricane Sandy
    dt.date(2018, 12, 5),   # President G.H.W. Bush
    dt.date(2025, 1, 9),    # President Carter
})

# ---------------------------------------------------------------------------------------------------------
# NYSE calendar (pure)
# ---------------------------------------------------------------------------------------------------------


def easter(year: int) -> dt.date:
    """Gregorian Easter Sunday (anonymous algorithm)."""
    a = year % 19
    b, c = divmod(year, 100)
    d, e = divmod(b, 4)
    f = (b + 8) // 25
    g = (b - f + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i, k = divmod(c, 4)
    l_ = (32 + 2 * e + 2 * i - h - k) % 7
    m = (a + 11 * h + 22 * l_) // 451
    month, day = divmod(h + l_ - 7 * m + 114, 31)
    return dt.date(year, month, day + 1)


def _nth_weekday(year: int, month: int, weekday: int, n: int) -> dt.date:
    d = dt.date(year, month, 1)
    d += dt.timedelta(days=(weekday - d.weekday()) % 7)
    return d + dt.timedelta(weeks=n - 1)


def _last_weekday(year: int, month: int, weekday: int) -> dt.date:
    d = dt.date(year, month + 1, 1) - dt.timedelta(days=1) if month < 12 else dt.date(year, 12, 31)
    return d - dt.timedelta(days=(d.weekday() - weekday) % 7)


def _observed(d: dt.date, saturday_to_friday: bool = True) -> dt.date | None:
    if d.weekday() == 5:
        return d - dt.timedelta(days=1) if saturday_to_friday else None
    if d.weekday() == 6:
        return d + dt.timedelta(days=1)
    return d


def nyse_holidays(year: int) -> set[dt.date]:
    """NYSE full-day holidays of ``year`` (weekend observance rules, Juneteenth from 2022, special closures)."""
    out: set[dt.date] = set()
    ny = _observed(dt.date(year, 1, 1), saturday_to_friday=False)  # Sat New Year: no Friday closure
    if ny:
        out.add(ny)
    out.add(_nth_weekday(year, 1, 0, 3))   # Martin Luther King Jr. Day
    out.add(_nth_weekday(year, 2, 0, 3))   # Washington's Birthday
    out.add(easter(year) - dt.timedelta(days=2))  # Good Friday
    out.add(_last_weekday(year, 5, 0))     # Memorial Day
    if year >= 2022:
        out.add(_observed(dt.date(year, 6, 19)))  # Juneteenth
    out.add(_observed(dt.date(year, 7, 4)))
    out.add(_nth_weekday(year, 9, 0, 1))   # Labor Day
    out.add(_nth_weekday(year, 11, 3, 4))  # Thanksgiving
    out.add(_observed(dt.date(year, 12, 25)))
    out |= {d for d in SPECIAL_CLOSURES if d.year == year}
    return {d for d in out if d is not None}


def nyse_early_closes(year: int) -> set[dt.date]:
    """13:00 ET closes: the day after Thanksgiving, and July 3 / December 24 when they fall Monday-Thursday."""
    hol = nyse_holidays(year)
    out = {_nth_weekday(year, 11, 3, 4) + dt.timedelta(days=1)}
    for d in (dt.date(year, 7, 3), dt.date(year, 12, 24)):
        if d.weekday() <= 3 and d not in hol:
            out.add(d)
    return out


def nyse_rule_sessions(start: dt.date, end: dt.date) -> list[dt.date]:
    hol: set[dt.date] = set()
    for y in range(start.year, end.year + 1):
        hol |= nyse_holidays(y)
    out = []
    d = start
    while d <= end:
        if d.weekday() < 5 and d not in hol:
            out.append(d)
        d += dt.timedelta(days=1)
    return out


class SessionCalendar:
    """Sorted sessions with index lookups; early closes from the NYSE rule."""

    def __init__(self, sessions: Iterable[dt.date], basis: dict[dt.date, str] | None = None) -> None:
        self.sessions = sorted(set(sessions))
        self.index = {d: i for i, d in enumerate(self.sessions)}
        self.basis = basis or {}
        years = {d.year for d in self.sessions}
        self.early = set().union(*(nyse_early_closes(y) for y in years)) if years else set()

    def is_session(self, d: dt.date) -> bool:
        return d in self.index

    def on_or_after(self, d: dt.date) -> dt.date | None:
        i = bisect.bisect_left(self.sessions, d)
        return self.sessions[i] if i < len(self.sessions) else None

    def after(self, d: dt.date) -> dt.date | None:
        i = bisect.bisect_right(self.sessions, d)
        return self.sessions[i] if i < len(self.sessions) else None

    def close_time(self, d: dt.date) -> dt.time:
        return EARLY_CLOSE if d in self.early else CLOSE


def build_calendar(vendor_sessions: list[dt.date], start: dt.date = RULE_START,
                   end: dt.date = RULE_END) -> tuple[SessionCalendar, dict[str, Any]]:
    """Vendor sessions inside their range, the NYSE rule outside; plus the rule-vs-vendor audit."""
    rule = nyse_rule_sessions(start, end)
    if not vendor_sessions:
        return SessionCalendar(rule, {d: "nyse_rule" for d in rule}), {"vendor_sessions": 0}
    lo, hi = min(vendor_sessions), max(vendor_sessions)
    vend = set(vendor_sessions)
    rule_in = {d for d in rule if lo <= d <= hi}
    sessions = [d for d in rule if d < lo or d > hi] + sorted(vend)
    basis = {d: ("vendor_calendar" if lo <= d <= hi else "nyse_rule") for d in sessions}
    audit = {"vendor_range": [lo.isoformat(), hi.isoformat()], "vendor_sessions": len(vend),
             "rule_sessions_in_range": len(rule_in),
             "vendor_not_rule": sorted(d.isoformat() for d in vend - rule_in),
             "rule_not_vendor": sorted(d.isoformat() for d in rule_in - vend)}
    return SessionCalendar(sessions, basis), audit


def classify_timing(et: dt.datetime, cal: SessionCalendar) -> str:
    """pre_market / intraday / post_market against the day's NYSE session; closed_day when there is none."""
    d = et.date()
    if not cal.is_session(d):
        return "closed_day"
    t = et.time()
    if t < OPEN:
        return "pre_market"
    if t < cal.close_time(d):
        return "intraday"
    return "post_market"


def sessions_for(et: dt.datetime, timing: str, cal: SessionCalendar) -> tuple[dt.date | None, dt.date | None]:
    """(session_date, reaction_session) of an announcement accepted at ``et`` (America/New_York)."""
    d = et.date()
    if timing == "closed_day":
        s = cal.on_or_after(d)
        return s, s
    if timing == "post_market":
        return d, cal.after(d)
    return d, d


def roll_forward(d: dt.date, cal: SessionCalendar) -> dt.date | None:
    return cal.on_or_after(d)


def match_year_ago(period_end: dt.date, prior: list[tuple[dt.date, int]]) -> int | None:
    """Index of the prior primary announcement whose period end is 345-385 days before ``period_end``
    (closest to 365; ties to the earlier period), else None."""
    best: tuple[int, dt.date, int] | None = None
    for pe, idx in prior:
        gap = (period_end - pe).days
        if YOY_MATCH_DAYS[0] <= gap <= YOY_MATCH_DAYS[1]:
            key = (abs(gap - 365), pe, idx)
            if best is None or key < best:
                best = key
    return None if best is None else best[2]


def session_gap(a: dt.date | None, b: dt.date | None, cal: SessionCalendar) -> int | None:
    """Sessions from ``b`` to ``a`` (a - b) when both are sessions."""
    if a is None or b is None or a not in cal.index or b not in cal.index:
        return None
    return cal.index[a] - cal.index[b]


def process_cik(rows: list[dict[str, Any]], cal: SessionCalendar) -> list[dict[str, Any]]:
    """Timing, sessions, primary flag and expected dates for one CIK's announcements (sorted by acceptance)."""
    out: list[dict[str, Any]] = []
    seen_reaction: set[dt.date] = set()
    seen_period: set[dt.date] = set()
    primaries: list[dict[str, Any]] = []
    for r in sorted(rows, key=lambda x: (x["announcement_utc"] or dt.datetime.max, x["accession"])):
        o = dict(r)
        acc = r["announcement_utc"]
        if acc is None:
            o.update(announcement_et=None, timing="unknown", session_date=None, reaction_session=None,
                     session_basis=None)
        else:
            et = acc.replace(tzinfo=UTC).astimezone(ET).replace(tzinfo=None)
            timing = classify_timing(et, cal)
            sd, rs = sessions_for(et, timing, cal)
            o.update(announcement_et=et, timing=timing, session_date=sd, reaction_session=rs,
                     session_basis=cal.basis.get(sd) if sd else None)
        pe = r.get("fiscal_period_end")
        lag = (o["announcement_et"].date() - pe).days if (pe is not None and o["announcement_et"]) else None
        o["period_lag_days"] = lag
        dup = o["reaction_session"] is not None and o["reaction_session"] in seen_reaction
        o["same_session_dup"] = dup
        if o["reaction_session"] is not None:
            seen_reaction.add(o["reaction_session"])
        primary = (not dup and pe is not None and lag is not None and 0 <= lag <= MAX_PERIOD_LAG_DAYS
                   and pe not in seen_period and o["session_date"] is not None)
        o["is_primary"] = primary
        o.update(next_expected_date=None, expected_date=None, expected_rule=None, expected_available_at=None,
                 expected_source_accession=None, expected_error_sessions=None, expected_error_days=None)
        if primary:
            seen_period.add(pe)
            o["next_expected_date"] = roll_forward(o["session_date"] + dt.timedelta(days=YOY_DAYS), cal)
            j = match_year_ago(pe, [(p["fiscal_period_end"], i) for i, p in enumerate(primaries)])
            src, rule, exp = None, None, None
            if j is not None:
                src, rule = primaries[j], "yoy_364"
                exp = src["next_expected_date"]
            elif primaries:
                src, rule = primaries[-1], "prev_primary_plus_91"
                exp = roll_forward(src["session_date"] + dt.timedelta(days=QOQ_DAYS), cal)
            if src is not None and exp is not None:
                o.update(expected_date=exp, expected_rule=rule, expected_available_at=src["available_at"],
                         expected_source_accession=src["accession"],
                         expected_error_sessions=session_gap(o["session_date"], exp, cal),
                         expected_error_days=(o["session_date"] - exp).days)
            primaries.append(o)
        out.append(o)
    return out


OUT_SCHEMA = pa.schema([
    ("cik", pa.int64()), ("accession", pa.string()), ("form", pa.string()), ("items", pa.string()),
    ("filing_date", pa.date32()), ("event_date", pa.date32()),
    ("announcement_utc", pa.timestamp("us")), ("announcement_et", pa.timestamp("us")),
    ("available_at", pa.timestamp("us")), ("acceptance_clock", pa.string()), ("vintage_risk", pa.string()),
    ("timing", pa.string()), ("session_date", pa.date32()),
    ("reaction_session", pa.date32()), ("session_basis", pa.string()),
    ("fiscal_period_end", pa.date32()), ("fiscal_period_form", pa.string()), ("period_lag_days", pa.int32()),
    ("same_session_dup", pa.bool_()), ("is_primary", pa.bool_()),
    ("next_expected_date", pa.date32()),
    ("expected_date", pa.date32()), ("expected_rule", pa.string()), ("expected_available_at", pa.timestamp("us")),
    ("expected_source_accession", pa.string()), ("expected_error_sessions", pa.int32()),
    ("expected_error_days", pa.int32()),
])


def _sec_stage() -> Path:
    d = C.build_root() / SF.STAGE
    man = d / "manifest.json"
    if not man.exists() or C.read_json(man).get("status") != "complete":
        raise SystemExit(f"{man}: sec_filings stage is not published")
    return d


def build(con, dest: Path, cal: SessionCalendar) -> int:
    sec = _sec_stage()
    items = (sec / "eight_k_items.parquet").as_posix()
    filings = (sec / "filings.parquet").as_posix()
    forms = ", ".join(f"'{f}'" for f in PERIODIC_FORMS)
    reader = con.execute(f"""
        WITH a AS (
            SELECT DISTINCT cik, accession, form, filing_date, report_date AS event_date,
                   acceptance_utc AS announcement_utc, available_at, acceptance_clock, vintage_risk,
                   CAST(timezone('America/New_York', CAST(coalesce(acceptance_utc, available_at) AS TIMESTAMPTZ))
                        AS DATE) AS et_date
            FROM read_parquet('{items}') WHERE item = '2.02' AND NOT is_amendment
        ), it AS (
            SELECT cik, accession, string_agg(item, ',' ORDER BY item) AS items
            FROM read_parquet('{items}') WHERE accession IN (SELECT accession FROM a) GROUP BY 1, 2
        ), p AS (
            SELECT cik, report_date, arg_max(form, CASE WHEN form LIKE '10-K%' THEN 1 ELSE 0 END) AS form
            FROM read_parquet('{filings}')
            WHERE form IN ({forms}) AND report_date IS NOT NULL GROUP BY 1, 2
        )
        SELECT a.cik, a.accession, a.form, it.items, a.filing_date, a.event_date, a.announcement_utc,
               a.available_at, a.acceptance_clock, a.vintage_risk, p.report_date AS fiscal_period_end,
               p.form AS fiscal_period_form
        FROM a LEFT JOIN it USING (cik, accession)
        ASOF LEFT JOIN p ON a.cik = p.cik AND a.et_date >= p.report_date
        ORDER BY a.cik, a.announcement_utc, a.accession
    """).fetch_record_batch(100_000)
    tmp = dest.with_name(dest.name + ".partial")
    writer = pq.ParquetWriter(tmp, OUT_SCHEMA, compression="zstd")
    total = 0
    cur: int | None = None
    rows: list[dict[str, Any]] = []
    pending: list[dict[str, Any]] = []

    def flush() -> int:
        if not pending:
            return 0
        t = pa.Table.from_pylist(pending, schema=OUT_SCHEMA)
        writer.write_table(t)
        pending.clear()
        return t.num_rows

    for batch in reader:
        for r in batch.to_pylist():
            if r["cik"] != cur:
                if cur is not None:
                    pending.extend(process_cik(rows, cal))
                cur, rows = r["cik"], []
            rows.append(r)
        if len(pending) >= 100_000:
            total += flush()
    if cur is not None:
        pending.extend(process_cik(rows, cal))
    total += flush()
    writer.close()
    os.replace(tmp, dest)
    return total


def coverage(con, ann: Path) -> dict[str, Any]:
    sec = _sec_stage()
    a = ann.as_posix()
    f = (sec / "filings.parquet").as_posix()
    per_year = con.execute(f"""
        WITH q AS (
            SELECT year(filing_date) AS y, cik FROM read_parquet('{f}') WHERE form = '10-Q' GROUP BY 1, 2
        ), n AS (
            SELECT year(session_date) AS y, cik, count(*) AS k FROM read_parquet('{a}')
            WHERE NOT same_session_dup AND session_date IS NOT NULL GROUP BY 1, 2
        ), cov AS (
            SELECT q.y, count(*) AS tenq_ciks, count(*) FILTER (WHERE coalesce(n.k, 0) >= 3) AS with3,
                   count(*) FILTER (WHERE coalesce(n.k, 0) >= 1) AS with1
            FROM q LEFT JOIN n USING (y, cik) GROUP BY 1
        ), t AS (
            SELECT year(session_date) AS y, count(*) AS announcements, count(DISTINCT cik) AS ciks,
                   count(*) FILTER (WHERE is_primary) AS primary_n,
                   count(*) FILTER (WHERE timing = 'pre_market') AS pre_market,
                   count(*) FILTER (WHERE timing = 'intraday') AS intraday,
                   count(*) FILTER (WHERE timing = 'post_market') AS post_market,
                   count(*) FILTER (WHERE timing = 'closed_day') AS closed_day,
                   count(*) FILTER (WHERE same_session_dup) AS dups,
                   count(*) FILTER (WHERE vintage_risk = 'acceptance_clock_unresolved') AS clock_unresolved,
                   count(*) FILTER (WHERE fiscal_period_end IS NOT NULL AND period_lag_days <= {MAX_PERIOD_LAG_DAYS})
                       AS period_matched,
                   count(*) FILTER (WHERE expected_rule = 'yoy_364') AS exp_yoy,
                   count(*) FILTER (WHERE expected_rule = 'prev_primary_plus_91') AS exp_qoq,
                   count(*) FILTER (WHERE expected_rule = 'yoy_364' AND expected_error_sessions = 0) AS yoy_exact,
                   count(*) FILTER (WHERE expected_rule = 'yoy_364' AND abs(expected_error_sessions) <= 1) AS yoy_1,
                   count(*) FILTER (WHERE expected_rule = 'yoy_364' AND abs(expected_error_sessions) <= 5) AS yoy_5,
                   median(abs(expected_error_sessions)) FILTER (WHERE expected_rule = 'yoy_364') AS yoy_med_abs,
                   count(*) FILTER (WHERE expected_rule = 'prev_primary_plus_91' AND abs(expected_error_sessions) <= 5)
                       AS qoq_5
            FROM read_parquet('{a}') WHERE session_date IS NOT NULL GROUP BY 1
        )
        SELECT t.*, cov.tenq_ciks, cov.with3, cov.with1 FROM t LEFT JOIN cov USING (y) ORDER BY y
    """)
    cols = [d[0] for d in per_year.description]
    table = {}
    for row in per_year.fetchall():
        rec = dict(zip(cols, row))
        y = str(rec.pop("y"))
        n = rec["announcements"] or 1
        for k in ("pre_market", "intraday", "post_market", "closed_day"):
            rec[f"{k}_share"] = round(rec[k] / n, 4)
        if rec.get("tenq_ciks"):
            rec["share_10q_ciks_ge3"] = round(rec["with3"] / rec["tenq_ciks"], 4)
            rec["share_10q_ciks_ge1"] = round(rec["with1"] / rec["tenq_ciks"], 4)
        if rec["exp_yoy"]:
            for k in ("yoy_exact", "yoy_1", "yoy_5"):
                rec[f"{k}_share"] = round(rec[k] / rec["exp_yoy"], 4)
        if rec["exp_qoq"]:
            rec["qoq_5_share"] = round(rec["qoq_5"] / rec["exp_qoq"], 4)
        rec["yoy_med_abs"] = None if rec["yoy_med_abs"] is None else float(rec["yoy_med_abs"])
        table[y] = rec
    return table


def member_coverage(con, ann: Path) -> dict[str, Any]:
    """Per year over panel ``member_equity`` cells: linked share and the share whose linked CIK has a primary 8-K
    2.02 announcement visible at the cell (available_at < 22:00 UTC of session d-1) within 120 / 364 days (364:
    a ``next_expected_date`` >= d is known). Metadata only; no returns."""
    panel = (C.build_root() / "panel" / "*" / "*.parquet").as_posix()
    if not list((C.build_root() / "panel").glob("*/*.parquet")):
        return {}
    a = ann.as_posix()
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE prev_sess AS
        SELECT session_date, lag(session_date) OVER (ORDER BY session_date) AS prev_session
        FROM read_parquet('{C.calendar_path().as_posix()}')""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE prim AS
        SELECT cik, available_at, session_date AS ann_session FROM read_parquet('{a}') WHERE is_primary""")
    out: dict[str, Any] = {}
    years = [r[0] for r in con.execute(f"SELECT DISTINCT year FROM read_parquet('{panel}') ORDER BY 1").fetchall()]
    for y in years:
        rows = con.execute(f"""
            WITH m AS (
                SELECT p.session_date, p.security_id, p.cik, coalesce(p.link_tier, 'unlinked') AS link_tier,
                       CAST(s.prev_session AS TIMESTAMP) + INTERVAL {C.MARK_HOUR_UTC} HOUR AS mark_utc
                FROM read_parquet('{panel}') p JOIN prev_sess s USING (session_date)
                WHERE p.year = {y} AND p.member_equity
            ), j AS (
                SELECT m.*, pr.ann_session FROM m ASOF LEFT JOIN prim pr
                  ON m.cik = pr.cik AND m.mark_utc > pr.available_at
            )
            SELECT link_tier, count(*) AS cells,
                   count(*) FILTER (WHERE ann_session >= session_date - 120) AS ann120,
                   count(*) FILTER (WHERE ann_session >= session_date - 364) AS ann364
            FROM j GROUP BY 1 ORDER BY 1""").fetchall()
        tot = sum(r[1] for r in rows)
        linked = sum(r[1] for r in rows if r[0] != "unlinked")
        a120 = sum(r[2] for r in rows)
        a364 = sum(r[3] for r in rows)
        out[str(y)] = {
            "member_equity_cells": tot, "linked_share": round(linked / tot, 4) if tot else None,
            "ann120_share_of_all": round(a120 / tot, 4) if tot else None,
            "ann364_share_of_all": round(a364 / tot, 4) if tot else None,
            "ann120_share_of_linked": round(a120 / linked, 4) if linked else None,
            "ann364_share_of_linked": round(a364 / linked, 4) if linked else None,
            "by_link_tier": {t: {"cells": int(n), "ann120_share": round(x / n, 4) if n else None,
                                 "ann364_share": round(z / n, 4) if n else None} for t, n, x, z in rows},
        }
    return out


def sec_validation(con, ann: Path) -> dict[str, Any]:
    """Validation from SEC evidence only (the vendor earnFlag is not a reference; owner ruling 2026-09-28).

    * ``event_date_vs_acceptance``: the 8-K's own event date (SEC reportDate, the earliest event reported) against
      the ET date of the resolved acceptance, per ``acceptance_clock``. An earnings 8-K is normally filed the day
      of the release, so a wrong clock shows up as event date = ET date - 1 on evening filings.
    * ``period_lag``: ET announcement date minus the matched fiscal period end, primary rows.
    * ``periodic_report_order``: whether the matched 10-Q/10-K was filed on or after the announcement's ET date.
    """
    sec = _sec_stage()
    a = ann.as_posix()
    f = (sec / "filings.parquet").as_posix()
    forms = ", ".join(f"'{x}'" for x in PERIODIC_FORMS)
    ev = con.execute(f"""
        SELECT acceptance_clock, timing, count(*) AS n,
               count(*) FILTER (WHERE event_date = CAST(announcement_et AS DATE)) AS same_day,
               count(*) FILTER (WHERE event_date = CAST(announcement_et AS DATE) - 1) AS minus1,
               count(*) FILTER (WHERE event_date < CAST(announcement_et AS DATE) - 1) AS earlier,
               count(*) FILTER (WHERE event_date > CAST(announcement_et AS DATE)) AS later
        FROM read_parquet('{a}') WHERE event_date IS NOT NULL AND announcement_et IS NOT NULL
          AND year(session_date) >= 2012
        GROUP BY ALL ORDER BY ALL""").fetchall()
    ev_out: dict[str, Any] = {}
    for clock, timing, n, same, m1, earlier, later in ev:
        ev_out.setdefault(str(clock), {})[timing] = {
            "n": int(n), "same_day": round(same / n, 4), "event_one_day_before": round(m1 / n, 4),
            "event_earlier": round(earlier / n, 4), "event_after_acceptance": round(later / n, 4)}
    lag = con.execute(f"""
        SELECT year(session_date) AS y, count(*), quantile_cont(period_lag_days, 0.1), median(period_lag_days),
               quantile_cont(period_lag_days, 0.9), avg(CASE WHEN period_lag_days <= 45 THEN 1.0 ELSE 0.0 END),
               avg(CASE WHEN period_lag_days <= 90 THEN 1.0 ELSE 0.0 END)
        FROM read_parquet('{a}') WHERE is_primary GROUP BY 1 ORDER BY 1""").fetchall()
    order = con.execute(f"""
        WITH rep AS (
            SELECT cik, report_date, min(filing_date) AS rep_filed FROM read_parquet('{f}')
            WHERE form IN ({forms}) GROUP BY 1, 2
        )
        SELECT year(a.session_date) AS y, count(*) AS n,
               count(*) FILTER (WHERE r.rep_filed >= CAST(a.announcement_et AS DATE)) AS report_on_or_after,
               count(*) FILTER (WHERE r.rep_filed = CAST(a.announcement_et AS DATE)) AS same_day,
               median(r.rep_filed - CAST(a.announcement_et AS DATE)) AS median_days_to_report
        FROM read_parquet('{a}') a JOIN rep r ON r.cik = a.cik AND r.report_date = a.fiscal_period_end
        WHERE a.is_primary GROUP BY 1 ORDER BY 1""").fetchall()
    return {
        "rule": "SEC evidence only; the vendor earnFlag is not used as a reference",
        "event_date_vs_acceptance": ev_out,
        "period_lag": {str(y): {"n": int(n), "p10": float(p10), "p50": float(p50), "p90": float(p90),
                                "share_le45": round(float(s45), 4), "share_le90": round(float(s90), 4)}
                       for y, n, p10, p50, p90, s45, s90 in lag},
        "periodic_report_order": {str(y): {"n": int(n), "report_filed_on_or_after_release": round(o / n, 4),
                                           "report_same_day": round(sd / n, 4),
                                           "median_days_release_to_report": float(md)}
                                  for y, n, o, sd, md in order},
    }


def spot_checks(con, ann: Path) -> dict[str, Any]:
    a = ann.as_posix()
    out: dict[str, Any] = {}
    for name, cik in (("AAPL", 320193), ("MSFT", 789019), ("JPM", 19617), ("GS", 886982), ("WMT", 104169),
                      ("NVDA", 1045810)):
        rows = con.execute(f"""
            SELECT timing, count(*), min(strftime(announcement_et, '%H:%M')), median(hour(announcement_et) * 60
                   + minute(announcement_et)), max(strftime(announcement_et, '%H:%M'))
            FROM read_parquet('{a}') WHERE cik = {cik} AND NOT same_session_dup GROUP BY 1 ORDER BY 2 DESC
        """).fetchall()
        last = con.execute(f"""
            SELECT strftime(announcement_et, '%Y-%m-%d %H:%M'), timing, session_date, reaction_session,
                   fiscal_period_end, expected_date, expected_error_sessions
            FROM read_parquet('{a}') WHERE cik = {cik} AND is_primary ORDER BY announcement_utc DESC LIMIT 4
        """).fetchall()
        out[name] = {
            "cik": cik,
            "timing": [{"timing": t, "n": int(n), "earliest_et": lo,
                        "median_et": f"{int(m) // 60:02d}:{int(m) % 60:02d}", "latest_et": hi}
                       for t, n, lo, m, hi in rows],
            "last_primary": [[str(v) for v in r] for r in last],
        }
    return out


def vendor_agreement(con, ann: Path) -> dict[str, Any]:
    """Informational only (``VENDOR_LABEL``): 8-K reaction sessions vs the vendor earnFlag '0' days on linked lines.
    The vendor flag is known unreliable; this is not a validation and nothing is tuned or accepted on it."""
    a = ann.as_posix()
    prices = (C.build_root() / "prices" / "*" / "*.parquet").as_posix()
    links = (C.build_root() / "identity" / "links_combined.parquet").as_posix()
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE sess AS
        SELECT session_date, row_number() OVER (ORDER BY session_date) AS sidx
        FROM read_parquet('{C.calendar_path().as_posix()}')""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE vend AS
        SELECT p.security_id, p.session_date, s.sidx
        FROM read_parquet('{prices}', hive_partitioning = false) p JOIN sess s USING (session_date)
        WHERE p.earn_flag = '0'""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE lk AS
        SELECT security_id, cik, start, end_incl,
               CASE WHEN tier IN ('high', 'medium') THEN 'strict' ELSE tier END AS link_tier
        FROM read_parquet('{links}')""")
    lo, hi = con.execute(f"SELECT min(session_date), max(session_date) FROM read_parquet('{prices}', "
                         "hive_partitioning = false)").fetchone()
    # 8-K -> vendor: each (announcement, linked line present on the reaction session).
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE pr AS
        WITH an AS (
            SELECT a.cik, a.accession, a.timing, a.reaction_session, s.sidx AS r_sidx
            FROM read_parquet('{a}') a JOIN sess s ON s.session_date = a.reaction_session
            WHERE NOT a.same_session_dup AND a.reaction_session BETWEEN DATE '{lo}' AND DATE '{hi}'
        )
        SELECT an.cik, an.accession, an.timing, an.reaction_session, an.r_sidx, lk.security_id,
               arg_min(lk.link_tier, CASE lk.link_tier WHEN 'strict' THEN 0 WHEN 'backfill' THEN 1 ELSE 2 END)
                   AS link_tier
        FROM an JOIN lk ON an.cik = lk.cik AND an.reaction_session BETWEEN lk.start AND lk.end_incl
        GROUP BY ALL""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE present AS
        SELECT pr.* FROM pr JOIN (SELECT security_id, session_date FROM read_parquet('{prices}', hive_partitioning = false)
                                  WHERE security_id IN (SELECT DISTINCT security_id FROM pr)) p
          ON p.security_id = pr.security_id AND p.session_date = pr.reaction_session""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE pairs AS
        SELECT present.*, (
            SELECT arg_min(v.sidx - present.r_sidx, abs(v.sidx - present.r_sidx) * 2 + (v.sidx < present.r_sidx)::INT)
            FROM vend v WHERE v.security_id = present.security_id
              AND v.sidx BETWEEN present.r_sidx - {AGREE_WINDOW} AND present.r_sidx + {AGREE_WINDOW}
        ) AS vendor_offset
        FROM present""")
    fwd = con.execute("""
        SELECT year(reaction_session) AS y, link_tier, count(*) AS n,
               count(*) FILTER (WHERE vendor_offset = 0) AS exact,
               count(*) FILTER (WHERE abs(vendor_offset) <= 1) AS within1,
               count(*) FILTER (WHERE vendor_offset IS NOT NULL) AS within5,
               count(*) FILTER (WHERE vendor_offset = -1) AS vendor_one_before,
               count(*) FILTER (WHERE vendor_offset = 1) AS vendor_one_after
        FROM pairs GROUP BY 1, 2 ORDER BY 1, 2""").fetchall()
    by_timing = con.execute("""
        SELECT timing, count(*) AS n, count(*) FILTER (WHERE vendor_offset = 0) AS exact,
               count(*) FILTER (WHERE vendor_offset = -1) AS m1, count(*) FILTER (WHERE vendor_offset = 1) AS p1,
               count(*) FILTER (WHERE vendor_offset IS NULL) AS none5
        FROM pairs GROUP BY 1 ORDER BY 1""").fetchall()
    # vendor -> 8-K: each vendor reaction day on a line linked that day.
    rev = con.execute(f"""
        WITH vl AS (
            SELECT v.security_id, v.session_date, v.sidx, lk.cik,
                   arg_min(lk.link_tier, CASE lk.link_tier WHEN 'strict' THEN 0 WHEN 'backfill' THEN 1 ELSE 2 END)
                       AS link_tier
            FROM vend v JOIN lk ON v.security_id = lk.security_id AND v.session_date BETWEEN lk.start AND lk.end_incl
            GROUP BY ALL
        ), an AS (
            SELECT a.cik, s.sidx AS r_sidx FROM read_parquet('{a}') a JOIN sess s ON s.session_date = a.reaction_session
            WHERE NOT a.same_session_dup
        )
        SELECT year(vl.session_date) AS y, vl.link_tier, count(*) AS n,
               count(*) FILTER (WHERE EXISTS (SELECT 1 FROM an WHERE an.cik = vl.cik AND an.r_sidx = vl.sidx)) AS exact,
               count(*) FILTER (WHERE EXISTS (SELECT 1 FROM an WHERE an.cik = vl.cik
                                              AND an.r_sidx BETWEEN vl.sidx - 1 AND vl.sidx + 1)) AS within1
        FROM vl GROUP BY 1, 2 ORDER BY 1, 2""").fetchall()

    def share(x: int, n: int) -> float | None:
        return round(x / n, 4) if n else None

    out: dict[str, Any] = {
        "window": [str(lo), str(hi)],
        "rule": ("pairs = (8-K 2.02 announcement, line linked to its CIK on the reaction session and present in "
                 "prices that day); vendor_offset = nearest vendor earnFlag '0' session minus the 8-K reaction session "
                 f"within +-{AGREE_WINDOW} sessions (ties to the later); reverse = vendor '0' days on a line "
                 "linked that day with an 8-K reaction session for the CIK"),
        "eightk_to_vendor": {}, "by_timing": {}, "vendor_to_eightk": {},
    }
    for y, tier, n, ex, w1, w5, m1, p1 in fwd:
        out["eightk_to_vendor"].setdefault(str(y), {})[tier] = {
            "pairs": int(n), "exact": share(ex, n), "within1": share(w1, n), "within5": share(w5, n),
            "vendor_one_session_before": share(m1, n), "vendor_one_session_after": share(p1, n)}
    for t, n, ex, m1, p1, none5 in by_timing:
        out["by_timing"][t] = {"pairs": int(n), "exact": share(ex, n), "vendor_one_before": share(m1, n),
                               "vendor_one_after": share(p1, n), "no_vendor_within5": share(none5, n)}
    for y, tier, n, ex, w1 in rev:
        out["vendor_to_eightk"].setdefault(str(y), {})[tier] = {
            "vendor_days": int(n), "exact": share(ex, n), "within1": share(w1, n)}
    tot = con.execute("""SELECT count(*), count(*) FILTER (WHERE vendor_offset = 0),
                         count(*) FILTER (WHERE abs(vendor_offset) <= 1) FROM pairs""").fetchone()
    out["overall"] = {"pairs": int(tot[0]), "exact": share(tot[1], tot[0]), "within1": share(tot[2], tot[0])}
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.parse_args(argv)
    stage = C.stage_dir(STAGE)
    con = C.connect(memory=DUCKDB_MEMORY)
    receipt: dict[str, Any] = {}
    vendor_sessions = C.load_calendar(con)
    cal, cal_audit = build_calendar(vendor_sessions)
    receipt["calendar_audit"] = cal_audit
    dest = stage / "announcements.parquet"
    with C.timed(receipt, "build"):
        receipt["rows"] = {"announcements": build(con, dest, cal)}
    with C.timed(receipt, "coverage"):
        receipt["coverage_per_year"] = coverage(con, dest)
    with C.timed(receipt, "member_coverage"):
        receipt["member_equity_coverage"] = member_coverage(con, dest)
    with C.timed(receipt, "spot_checks"):
        receipt["spot_checks"] = spot_checks(con, dest)
    with C.timed(receipt, "sec_validation"):
        receipt["sec_validation"] = sec_validation(con, dest)
    with C.timed(receipt, "vendor_flag_comparison"):
        receipt["vendor_flag_comparison"] = {"label": VENDOR_LABEL, **vendor_agreement(con, dest)}
    con.close()
    sec_manifest = C.build_root() / SF.STAGE / "manifest.json"
    payload = {
        "rules": {
            "announcement": "original 8-K (not /A) with item 2.02; one row per (cik, accession)",
            "available_at": "announcement_utc = EDGAR acceptance of the 8-K (sec_filings acceptance rule)",
            "timing": "America/New_York acceptance time vs the NYSE session: pre_market < 09:30 <= intraday < close "
                      "(16:00; 13:00 on early-close days) <= post_market; closed_day = no session that ET date",
            "session_date": "ET date if a session, else the next session",
            "reaction_session": "session_date for pre_market/intraday/closed_day; next session for post_market",
            "fiscal_period": f"latest original {'/'.join(PERIODIC_FORMS)} report_date <= announcement ET date "
                             f"(label, uses later filings); primary = first per (cik, period) with lag <= "
                             f"{MAX_PERIOD_LAG_DAYS} days and not a same-reaction-session duplicate",
            "expected": EXPECTED_RULE,
            "next_expected_date": "primary rows: first session on or after session_date + 364 days, known at the "
                                  "row's available_at",
            "calendar": "calendar.parquet sessions inside 2012-03-26..2026-09-18, NYSE rule calendar outside",
        },
        "staleness": "event data, no staleness",
        "sources": {"sec_filings_manifest": {"path": str(sec_manifest), "sha256": C.sha256_file(sec_manifest)},
                    "calendar": {"path": str(C.calendar_path()), "sha256": C.sha256_file(C.calendar_path())},
                    "links_combined": {"sha256": C.sha256_file(C.build_root() / "identity" / "links_combined.parquet")},
                    "prices_manifest": {"sha256": C.sha256_file(C.build_root() / "prices" / "manifest.json")},
                    "panel_manifest": {"sha256": C.sha256_file(C.build_root() / "panel" / "manifest.json")
                                       if (C.build_root() / "panel" / "manifest.json").exists() else None}},
        **receipt,
    }
    print(json.dumps({"rows": receipt["rows"], "event_date_vs_acceptance": receipt["sec_validation"]["event_date_vs_acceptance"]}), flush=True)
    print(C.write_stage_manifest(STAGE, SCHEMA, ("common", "sec_filings", "earnings_calendar"), payload), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
