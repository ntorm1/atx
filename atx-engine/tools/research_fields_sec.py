"""SEC-derived point-in-time research fields (platform v7, lane W5a): earnings calendar (D2), Form 4 (D11), 8-K (D12).

An opt-in field module of ``prepare_research_fields.py``: the builder's ``FIELD_MODULES`` hook binds it to the builder's
own namespace (``bind(globals())``), appends ``FIELDS`` to its registry after every existing field, and calls
``check`` before any output and ``compute`` after the built-in groups. Nothing else in the builder changes, so every
other field's bytes and manifest entry are identical whether or not these fields are requested.

Inputs (read-only; every file is verified against its stage manifest before parsing; every manifest SHA-256 is pinned
on the command line and recorded per field):
* ``--sec-stages``: the atx-db alpha-panel build root (``C:/atx/atx-db/data/alpha_panel/v1``), stages
  ``earnings_calendar`` (``announcements.parquet``, schema atx.alpha-panel.earnings-calendar/v1), ``insider``
  (``transactions/year=YYYY/YYYYqN.parquet``, atx.alpha-panel.insider/v1) and ``sec_filings``
  (``eight_k_items.parquet``, atx.alpha-panel.sec-filings/v1). See atx-db/docs/ALPHA_PANEL_SEC.md.
* ``--sec-identity-bridge``: an ``atx.identity-bridge/v1`` export (atx-db ``export/identity-bridge-v2-pit``), read with
  the builder's own ``load_bridge`` / ``resolve_links`` (``LINK_RULE``: values on primary P lines only). It is separate
  from ``--identity-bridge`` so the fundamental fields keep their own pinned bridge.

Clock (``SEC_CLOCK``): a source row is usable at role session t only if its ``available_at`` (UTC; EDGAR acceptance)
is strictly before 22:00 UTC of session t-1 on the session calendar (role sessions inside the role, the NYSE rule
calendar outside it). A filing accepted at 22:30 UTC on d-1 is therefore first usable at d+1. Rows available on or
after 2025-01-01 are dropped (seal). Sessions are counted on the same calendar.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

GROUP = "sec"
OPTIONS = ("sec_stages", "sec_identity_bridge", "sec_identity_bridge_sha256", "earnings_calendar_sha256",
           "insider_sha256", "sec_filings_sha256")
STAGES = {  # stage key -> (directory, manifest schema)
    "earnings_calendar": ("earnings_calendar", "atx.alpha-panel.earnings-calendar/v1"),
    "insider": ("insider", "atx.alpha-panel.insider/v1"),
    "sec_filings": ("sec_filings", "atx.alpha-panel.sec-filings/v1"),
}
SEC_LAG_SESSIONS = 1
CAL_PREFIX_DAYS = 400            # session calendar reaches 400 days before the role (every lookback below is shorter)
CAL_SUFFIX_DAYS = 400            # and 400 days after it (expected dates are at most ~370 days ahead)
# Earnings calendar (D2)
EA_STALE_DAYS = 200              # atx-db declared earnings staleness (ALPHA_PANEL_REQUEST_V7_RESPONSE.md section 0)
EA_CONSUME_DAYS = 45             # an expected date within 45 days after the latest announcement is taken as fulfilled
EA_FALLBACK_DAYS = 91            # no year-ago expectation for the next quarter: latest session + 91 days
EA_FALLBACK_TRIGGER_DAYS = 136   # ... when the earliest pending year-ago expectation is later than latest + 136 days
EA_OVERDUE_MAX_SESSIONS = 63     # an expectation overdue by more than 63 sessions -> NaN (calendar broken)
EA_LOOKBACK_ROWS = 16            # prior primary rows scanned for pending expectations (exactness counted)
EA_TIMING_CODES = {"pre_market": 0.0, "intraday": 1.0, "post_market": 2.0, "closed_day": 3.0}
# Form 4 (D11)
INS_WINDOW = 126
INS_CLUSTER_WINDOW = 21
INS_CLUSTER_MIN = 3
INS_PRESENCE_DAYS = 365          # atx-db declared Form 4 staleness
INS_ROUTINE_YEARS = 3
INS_SHARE_SCALE = 1000           # shares accumulated exactly as integer milli-shares
INS_RATIO_DOMAIN = (-1.0, 1.0)   # |net shares| cannot exceed shares outstanding
INS_SKIP_MARGIN_DAYS = 31        # a filing quarter starting > 31 days after the role's last session is not read
# 8-K metadata (D12)
K8_COUNT_WINDOW = 63
K8_MATERIAL_WINDOW = 21
K8_MATERIAL_ITEMS = ("1.01", "2.01", "2.05", "2.06", "4.02", "5.02")
K8_PRESENCE_DAYS = 365
NYSE_SPECIAL_CLOSURES = ("2001-09-11", "2001-09-12", "2001-09-13", "2001-09-14", "2004-06-11", "2007-01-02",
                         "2012-10-29", "2012-10-30", "2018-12-05", "2025-01-09")
ACCESSION_PATTERN = r"^[0-9]{10}-[0-9]{2}-[0-9]{6}$"

SEC_CLOCK = ("sec-acceptance-lag1-v1: a source row is usable at role session t iff available_at (UTC, the EDGAR "
             "acceptance resolved by atx-db acceptance-per-file-clock-v1) < 22:00 UTC of session t-1 on the session "
             "calendar (role sessions inside the role range, the NYSE rule calendar nyse-rule-v1 outside it); rows "
             "available on or after 2025-01-01 are dropped; sessions are counted on the same calendar")
LINK_NOTE = ("line -> CIK through the pinned --sec-identity-bridge (atx.identity-bridge/v1, atx-db "
             "export/identity-bridge-v2-pit) with the builder's LINK_RULE; values on primary (P) lines only")
EA_CAVEATS = [
    "announcement = atx-db primary 8-K item 2.02 row (is_primary). is_primary and the expected_date match use the "
    "stage's fiscal_period_end label (the latest 10-Q/10-K report_date at or before the announcement, from filings of "
    "any date: atx-db calls it a label, not a signal); whether a 2.02 row is primary can therefore depend on a "
    "periodic report filed after the announcement (about 9.5% of 2017-2022 2.02 rows are non-primary)",
    "8-K acceptance can follow the wire release by up to a session (atx-db: 19-21% of intraday 2.02 8-Ks report a "
    "prior-day event): the calendar is conservative, never early",
    "foreign private issuers announce on 6-K without item codes and are absent (NaN); REITs and trusts that do not "
    "furnish 2.02 are absent",
    LINK_NOTE]
INS_CAVEATS = [
    "trades = non-derivative open-market purchases (P) and sales (S) on original Form 4 (no 4/A, no Form 5) filed by "
    "a director or officer (any reporting owner on the filing); pure 10% owners and 'other' owners are excluded, and "
    "so are joint filings (more than one reporting owner) that include a 10% owner: sponsor / fund groups whose "
    "filing lists a deputized director would otherwise count block sales of the fund's shares; the insider is the "
    "filing's primary reporting owner (smallest owner CIK, atx-db rule)",
    "Form 4 share counts are in the issuer's share units at the trade date and are not restated for later splits, "
    "while shares_out is restated to the session basis: a split inside the window distorts the ratio",
    "foreign private issuers are exempt from Section 16 and have no Form 4 (NaN by the presence rule); an ADR line "
    "counts ADS, not ordinary shares",
    LINK_NOTE]
K8_CAVEATS = [
    "original 8-K forms (8-K, 8-K12B, 8-K12G3, 8-K15D5; no amendments); co-registrant 8-Ks count for each registrant",
    "foreign private issuers file 6-K, not 8-K: NaN by the presence rule",
    LINK_NOTE]

EA_NAMES = ("ea_days_to_expected", "ea_days_since", "ea_window_pre5", "ea_window_post3", "ea_delay_days",
            "ea_time_of_day")
INS_NAMES = ("ins_net_buy_ratio", "ins_n_buyers", "ins_n_sellers", "ins_opportunistic_net", "ins_cluster_buy")
K8_NAMES = ("k8_count_63", "k8_item_material_21", "k8_days_since_any")
EA_FRESH = f"no visible primary announcement, or the latest one's session_date is more than {EA_STALE_DAYS} days before the session -> NaN"
INS_PRESENT = (f"NaN unless the issuer has a visible insider transaction row (any form, code or owner) with available_at "
               f"within {INS_PRESENCE_DAYS} days before the 22:00 UTC mark of t-1 (Section 16 presence; else 0 would "
               "mean 'no trades' for an issuer that files no Form 4); NaN while the window or the presence lookback "
               "starts before the stage's first filing quarter")
INS_RATIO_NAN = ("shares_out not finite and positive -> NaN; |net shares| > shares_out (outside the declared domain "
                 "[-1, 1]: a shares_out units defect or a split the Form 4 counts do not restate) -> NaN, counted")
K8_PRESENT = (f"NaN unless the CIK has a visible original 8-K with available_at within {K8_PRESENCE_DAYS} days before "
              "the 22:00 UTC mark of t-1 (8-K filer presence)")

FIELDS = {}


def _spec(name, stages, units, definition, staleness, caveats, formula, lag_note, min_history, source_columns,
          requires=None, domain=None):
    spec = {"group": GROUP, "point_in_time": True, "lagged": False, "stages": stages, "units": units,
            "clock": SEC_CLOCK, "staleness": staleness, "caveats": caveats, "definition": definition,
            "source_columns": source_columns, "formula_id": formula, "lag": lag_note, "min_history": min_history}
    if requires:
        spec["requires"] = requires
    if domain:
        spec["domain"] = domain
    FIELDS[name] = spec


_EA_COLS = ["cik", "accession", "available_at", "is_primary", "session_date", "reaction_session", "timing",
            "next_expected_date", "expected_rule", "expected_error_days"]
_EA_EXPECT = (f"next expected announcement as of t: the latest visible primary row A (max available_at, tie by "
              f"accession); pending = next_expected_date E_r (atx-db yoy_364: first session >= session_date_r + 364) "
              f"of the CIK's visible primary rows r with E_r > session_date_A + {EA_CONSUME_DAYS} days (an E within "
              f"{EA_CONSUME_DAYS} days after the latest announcement is taken as fulfilled); E* = min pending; when "
              f"none or E* > session_date_A + {EA_FALLBACK_TRIGGER_DAYS} days, E* = first session >= session_date_A + "
              f"{EA_FALLBACK_DAYS} days (fallback)")
_EA_LAG = "announcement usable from the session after the first session whose 22:00 UTC mark follows its acceptance"
_EA_MIN = "one visible primary 2.02 announcement (stage from 2009)"
_spec("ea_days_to_expected", ["earnings_calendar"], "sessions (signed): expected announcement session minus t",
      f"{_EA_EXPECT}; value = sessions from t to E* (0 = expected today, negative = overdue)",
      f"{EA_FRESH}; overdue by more than {EA_OVERDUE_MAX_SESSIONS} sessions -> NaN", EA_CAVEATS,
      "sec-ea-days-to-expected-yoy364-consume45-fb91-v1", _EA_LAG, _EA_MIN, _EA_COLS)
_spec("ea_days_since", ["earnings_calendar"], "sessions since the latest announcement's reaction session (>= 0)",
      "t minus the reaction_session (atx-db: session_date for pre-market, intraday and closed-day releases, the next "
      "session for post-market) of the latest visible primary announcement A", EA_FRESH, EA_CAVEATS,
      "sec-ea-days-since-reaction-v1", _EA_LAG, _EA_MIN, _EA_COLS)
_spec("ea_window_pre5", ["earnings_calendar"], "indicator: 1 when the expected announcement is 1..5 sessions ahead",
      "1 if 1 <= ea_days_to_expected <= 5 else 0", f"NaN wherever ea_days_to_expected is NaN", EA_CAVEATS,
      "sec-ea-window-pre5-v1", _EA_LAG, _EA_MIN, _EA_COLS)
_spec("ea_window_post3", ["earnings_calendar"], "indicator: 1 when t is 0..3 sessions after the latest reaction session",
      "1 if 0 <= ea_days_since <= 3 else 0", "NaN wherever ea_days_since is NaN", EA_CAVEATS,
      "sec-ea-window-post3-v1", _EA_LAG, _EA_MIN, _EA_COLS)
_spec("ea_delay_days", ["earnings_calendar"], "calendar days (signed): announced session_date minus expected date",
      "expected_error_days of the latest visible primary announcement A (Johnson-So 2018 delay: A's session_date "
      "minus its expected_date, the year-ago same-quarter announcement's next_expected_date); only expected_rule "
      "yoy_364 (a prev_primary_plus_91 fallback expectation is not a same-quarter schedule -> NaN)",
      f"{EA_FRESH}; A without a yoy_364 expected date -> NaN", EA_CAVEATS, "sec-ea-delay-days-yoy364-v1", _EA_LAG,
      "one visible primary announcement with a year-ago same-quarter primary announcement", _EA_COLS)
_spec("ea_time_of_day", ["earnings_calendar"], "categorical code: 0 pre_market (BMO), 1 intraday, 2 post_market (AMC), "
      "3 closed_day, stored as f64", "atx-db timing of the latest visible primary announcement A (America/New_York "
      "acceptance time against the NYSE session)", EA_FRESH, EA_CAVEATS, "sec-ea-time-of-day-v1", _EA_LAG, _EA_MIN,
      _EA_COLS)
_INS_COLS = ["issuer_cik", "owner_cik", "form", "is_amendment", "table_type", "transaction_code", "acquired_disposed",
             "shares", "transaction_date", "any_director", "any_officer", "any_ten_percent_owner",
             "n_reporting_owners", "available_at"]
_INS_TRADES = (f"trades visible at t (available_at < mark(t-1)) whose transaction_date is on or after session t-{INS_WINDOW} "
               f"(the {INS_WINDOW} sessions t-{INS_WINDOW}..t-1; a late filing counts once it is visible)")
_INS_LAG = "trade usable from the session after the first session whose 22:00 UTC mark follows the Form 4 acceptance"
_INS_MIN = "window and presence lookback on or after the stage's first filing quarter (2015q1)"
_spec("ins_net_buy_ratio", ["insider"], "ratio (signed): net open-market shares bought / shares outstanding",
      f"(sum of P shares - sum of S shares) over {_INS_TRADES} / shares_out[t] of the line (this run's shares_out)",
      f"{INS_PRESENT}; {INS_RATIO_NAN}", INS_CAVEATS, "sec-ins-net-buy-ratio126-v1",
      _INS_LAG, _INS_MIN, _INS_COLS + ["shares_out"], requires=["shares_out"], domain=INS_RATIO_DOMAIN)
_spec("ins_n_buyers", ["insider"], "count of distinct insiders with an open-market purchase",
      f"distinct primary reporting owners (owner_cik) with a P trade among {_INS_TRADES}", INS_PRESENT, INS_CAVEATS,
      "sec-ins-n-buyers126-v1", _INS_LAG, _INS_MIN, _INS_COLS)
_spec("ins_n_sellers", ["insider"], "count of distinct insiders with an open-market sale",
      f"distinct primary reporting owners (owner_cik) with an S trade among {_INS_TRADES}", INS_PRESENT, INS_CAVEATS,
      "sec-ins-n-sellers126-v1", _INS_LAG, _INS_MIN, _INS_COLS)
_spec("ins_opportunistic_net", ["insider"], "ratio (signed): opportunistic insiders' net shares bought / shares out",
      f"(P - S shares) of the {_INS_TRADES} made by insiders classified opportunistic for the trade's calendar year Y "
      f"(Cohen-Malloy-Pomorski 2012): from the owner's P/S trades in the issuer with transaction_date in Y-3..Y-1 and "
      f"available_at before Y-01-01 00:00 UTC, the owner is routine if some calendar month has a trade in each of "
      f"Y-1, Y-2 and Y-3, opportunistic if each of those years has a trade but no month repeats in all three, else "
      f"unclassified (excluded); divided by shares_out[t]",
      f"{INS_PRESENT}; the window starting before 2018-01-01 (a trade year without {INS_ROUTINE_YEARS} prior years of "
      f"stage history) -> NaN; {INS_RATIO_NAN}", INS_CAVEATS,
      "sec-ins-opportunistic-net126-cmp3y-v1", _INS_LAG,
      f"{_INS_MIN}; classification needs {INS_ROUTINE_YEARS} full prior calendar years (first classifiable trade year "
      "2018)", _INS_COLS + ["shares_out"], requires=["shares_out"], domain=INS_RATIO_DOMAIN)
_spec("ins_cluster_buy", ["insider"], f"indicator: 1 when >= {INS_CLUSTER_MIN} distinct insiders bought",
      f"1 if at least {INS_CLUSTER_MIN} distinct primary reporting owners have a P trade visible at t with "
      f"transaction_date on or after session t-{INS_CLUSTER_WINDOW} ({INS_CLUSTER_WINDOW} sessions), else 0",
      INS_PRESENT, INS_CAVEATS, "sec-ins-cluster-buy21-min3-v1", _INS_LAG, _INS_MIN, _INS_COLS)
_K8_COLS = ["cik", "accession", "form", "is_amendment", "item", "available_at"]
_K8_LAG = ("filing usable from the session after its event session, the first session whose 22:00 UTC mark follows "
           "its acceptance")
_spec("k8_count_63", ["sec_filings"], "count of original 8-K filings",
      f"distinct original 8-K accessions of the CIK that became usable within the last {K8_COUNT_WINDOW} sessions "
      f"(usable from session e: counted at t for e <= t < e + {K8_COUNT_WINDOW})", K8_PRESENT, K8_CAVEATS,
      "sec-k8-count63-v1", _K8_LAG, "stage from 2009", _K8_COLS)
_spec("k8_item_material_21", ["sec_filings"], "indicator: 1 when a material-item 8-K became usable in 21 sessions",
      f"1 if an original 8-K of the CIK with any item in {list(K8_MATERIAL_ITEMS)} became usable within the last "
      f"{K8_MATERIAL_WINDOW} sessions, else 0", K8_PRESENT, K8_CAVEATS, "sec-k8-item-material21-v1", _K8_LAG,
      "stage from 2009", _K8_COLS)
_spec("k8_days_since_any", ["sec_filings"], "sessions since the latest original 8-K's event session (>= 1)",
      "t minus the event session (first session whose 22:00 UTC mark follows the acceptance) of the CIK's latest "
      "visible original 8-K", K8_PRESENT, K8_CAVEATS, "sec-k8-days-since-any-v1", _K8_LAG, "stage from 2009", _K8_COLS)
del _spec


# ---------------------------------------------------------------------------------------------------------------
# Session calendar: role sessions inside the role, the NYSE rule calendar outside
# ---------------------------------------------------------------------------------------------------------------

def _nth_weekday(year, month, weekday, n):
    d = dt.date(year, month, 1)
    return d + dt.timedelta(days=(weekday - d.weekday()) % 7 + 7 * (n - 1))


def _last_weekday(year, month, weekday):
    d = (dt.date(year + month // 12, month % 12 + 1, 1)) - dt.timedelta(days=1)
    return d - dt.timedelta(days=(d.weekday() - weekday) % 7)


def _easter(year):
    """Gregorian Easter Sunday (anonymous / Meeus-Jones-Butcher algorithm)."""
    a, (b, c) = year % 19, divmod(year, 100)
    d, e = divmod(b, 4)
    g = (b - (b + 8) // 25 + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i, k = divmod(c, 4)
    l = (32 + 2 * e + 2 * i - h - k) % 7
    m = (a + 11 * h + 22 * l) // 451
    month, day = divmod(h + l - 7 * m + 114, 31)
    return dt.date(year, month, day + 1)


def _observed(d):
    """Saturday holidays are observed on Friday, Sunday holidays on Monday."""
    return d + dt.timedelta(days={5: -1, 6: 1}.get(d.weekday(), 0))


def nyse_holidays(year: int) -> set:
    """NYSE full-day closures of a year (rule nyse-rule-v1): New Year (Sunday -> Monday; a Saturday New Year is not
    observed), MLK and Presidents (3rd Monday of Jan / Feb), Good Friday, Memorial (last Monday of May), Juneteenth
    (from 2022), Independence, Labor (1st Monday of Sep), Thanksgiving (4th Thursday of Nov), Christmas, plus the
    special closures in NYSE_SPECIAL_CLOSURES."""
    out = {_nth_weekday(year, 1, 0, 3), _nth_weekday(year, 2, 0, 3), _easter(year) - dt.timedelta(days=2),
           _last_weekday(year, 5, 0), _observed(dt.date(year, 7, 4)), _nth_weekday(year, 9, 0, 1),
           _nth_weekday(year, 11, 3, 4), _observed(dt.date(year, 12, 25))}
    new_year = dt.date(year, 1, 1)
    if new_year.weekday() < 5 or new_year.weekday() == 6:
        out.add(_observed(new_year))
    if year >= 2022:
        out.add(_observed(dt.date(year, 6, 19)))
    out |= {x for x in map(dt.date.fromisoformat, NYSE_SPECIAL_CLOSURES) if x.year == year}
    return out


def nyse_sessions(first: dt.date, last: dt.date) -> np.ndarray:
    """Epoch days of the NYSE rule sessions in [first, last]."""
    holidays = set().union(*(nyse_holidays(y) for y in range(first.year, last.year + 1)))
    days = (first + dt.timedelta(days=i) for i in range((last - first).days + 1))
    epoch = dt.date(1970, 1, 1)
    return np.array([(d - epoch).days for d in days if d.weekday() < 5 and d not in holidays], dtype=np.int64)


class Calendar:
    """Extended session axis: the NYSE rule sessions within CAL_PREFIX_DAYS before the role, the role's own sessions,
    then the rule sessions within CAL_SUFFIX_DAYS after it. Role session t is extended index t + prefix."""

    def __init__(self, role, day_ns: int, mark_ns: int):
        epoch = dt.date(1970, 1, 1)
        first, last = int(role.days[0]), int(role.days[-1])
        lo = epoch + dt.timedelta(days=first - CAL_PREFIX_DAYS)
        hi = epoch + dt.timedelta(days=last + CAL_SUFFIX_DAYS)
        rule = nyse_sessions(lo, hi)
        inside = rule[(rule >= first) & (rule <= last)]
        self.stats = {"rule": "nyse-rule-v1 outside the role range, the role's sessions inside it",
                      "prefix_days": CAL_PREFIX_DAYS, "suffix_days": CAL_SUFFIX_DAYS,
                      "rule_sessions_not_in_role": int(len(np.setdiff1d(inside, role.days))),
                      "role_sessions_not_in_rule": int(len(np.setdiff1d(role.days, inside)))}
        prefix, suffix = rule[rule < first], rule[rule > last]
        self.days = np.concatenate((prefix, role.days, suffix)).astype(np.int64)
        self.prefix = len(prefix)
        self.marks = self.days * day_ns + mark_ns
        self.day_ns = day_ns
        self.stats.update(first=str(epoch + dt.timedelta(days=int(self.days[0]))),
                          last=str(epoch + dt.timedelta(days=int(self.days[-1]))), prefix_sessions=self.prefix,
                          suffix_sessions=len(suffix))

    def usable_from(self, instants_ns: np.ndarray) -> np.ndarray:
        """First extended index u at which a row is usable: available_at < mark(u-1). The event session s is the
        first session whose mark follows the instant (mark(s) > available_at); u = s + 1."""
        return np.searchsorted(self.marks, instants_ns, side="right").astype(np.int64) + 1

    def at_or_after(self, days: np.ndarray) -> np.ndarray:
        """Extended index of the first session on or after each epoch day."""
        return np.searchsorted(self.days, days, side="left").astype(np.int64)

    def count_through(self, days: np.ndarray) -> np.ndarray:
        """Number of extended sessions on or before each epoch day."""
        return np.searchsorted(self.days, days, side="right").astype(np.int64)


# ---------------------------------------------------------------------------------------------------------------
# Streaming helpers
# ---------------------------------------------------------------------------------------------------------------

class Latest:
    """Clock-ordered rows (usable-from index non-decreasing): latest[c] = the last row of CIK c usable at u."""

    def __init__(self, usable: np.ndarray, cidx: np.ndarray, n_cik: int):
        if len(usable) and np.any(np.diff(usable) < 0):
            raise ValueError("internal: rows are not in clock order")
        self.usable, self.cidx, self.p = usable, cidx, 0
        self.latest = np.full(max(n_cik, 1), -1, dtype=np.int64)

    def at(self, u: int) -> np.ndarray:
        p2 = int(np.searchsorted(self.usable, u, side="right"))
        if p2 > self.p:
            np.maximum.at(self.latest, self.cidx[self.p:p2], np.arange(self.p, p2, dtype=np.int64))
            self.p = p2
        return self.latest


class Windowed:
    """Per-CIK sums over rows active on [enter, leave) extended indices, advanced session by session."""

    def __init__(self, enter, leave, cidx, value, n_cik: int, dtype):
        keep = enter < leave
        idx = np.concatenate((enter[keep], leave[keep]))
        o = np.argsort(idx, kind="stable")
        self.idx = idx[o]
        self.cidx = np.concatenate((cidx[keep], cidx[keep]))[o]
        self.delta = np.concatenate((value[keep], -value[keep])).astype(dtype)[o]
        self.state = np.zeros(max(n_cik, 1), dtype=dtype)
        self.p = 0

    def at(self, u: int) -> np.ndarray:
        p2 = int(np.searchsorted(self.idx, u, side="right"))
        if p2 > self.p:
            np.add.at(self.state, self.cidx[self.p:p2], self.delta[self.p:p2])
            self.p = p2
        return self.state


def merge_intervals(keys: list, enter: np.ndarray, leave: np.ndarray):
    """Disjoint union of [enter, leave) per key tuple (overlapping or touching intervals merge); returns the merged
    (row index of each run's first interval, enter, leave). Distinct-owner counts then add 1 per merged interval."""
    keep = np.flatnonzero(enter < leave)
    if not len(keep):
        return keep, enter[keep], leave[keep]
    o = keep[np.lexsort((enter[keep],) + tuple(k[keep] for k in reversed(keys)))]
    a, b = enter[o], leave[o]
    group = np.zeros(len(o), dtype=bool)
    group[0] = True
    for k in keys:
        group[1:] |= k[o][1:] != k[o][:-1]
    rank = np.cumsum(group) - 1
    scale = int(b.max()) + 1
    running = np.maximum.accumulate(rank * scale + b) - rank * scale   # running max of leave within the key
    start = group.copy()
    start[1:] |= a[1:] > running[:-1]
    first = np.flatnonzero(start)
    return o[first], a[first], np.maximum.reduceat(b, first)


def accession_keys(col) -> np.ndarray:
    """EDGAR accession 'NNNNNNNNNN-YY-NNNNNN' -> its 18 digits as int64 (same order as the text)."""
    text = pc.cast(col, pa.string())
    if text.null_count or not pc.all(pc.match_substring_regex(text, ACCESSION_PATTERN)).as_py():
        raise ValueError("accession is not NNNNNNNNNN-YY-NNNNNN")
    return pc.cast(pc.replace_substring(text, "-", ""), pa.int64()).to_numpy(zero_copy_only=False)


def cik_positions(ciks: np.ndarray, values: np.ndarray):
    pos = np.minimum(np.searchsorted(ciks, values), max(len(ciks) - 1, 0))
    return pos.astype(np.int64), (ciks[pos] == values) if len(ciks) else np.zeros(len(values), dtype=bool)


# ---------------------------------------------------------------------------------------------------------------
# The module object the builder binds
# ---------------------------------------------------------------------------------------------------------------

class _Host:
    """Late-bound view of the builder's namespace (its globals dict), so helpers defined after the hook resolve."""

    def __init__(self, namespace: dict):
        self._ns = namespace

    def __getattr__(self, name):
        return self._ns[name]


def bind(host_namespace: dict) -> "SecFieldModule":
    return SecFieldModule(host_namespace)


class SecFieldModule:
    GROUP, FIELDS, OPTIONS = GROUP, FIELDS, OPTIONS

    def __init__(self, host_namespace: dict):
        self.h = _Host(host_namespace)

    # -- command line --------------------------------------------------------------------------------------------
    @staticmethod
    def add_arguments(parser):
        g = parser.add_argument_group("SEC-derived fields (research_fields_sec.py: " + ",".join(FIELDS) + ")")
        g.add_argument("--sec-stages", type=Path, help="atx-db alpha-panel build root holding earnings_calendar/, "
                                                       "insider/ and sec_filings/")
        g.add_argument("--sec-identity-bridge", type=Path, help="atx.identity-bridge/v1 directory for the SEC fields "
                                                                "(atx-db export/identity-bridge-v2-pit)")
        g.add_argument("--sec-identity-bridge-sha256", help="SHA-256 of that bridge's manifest.json")
        for stage in STAGES:
            g.add_argument(f"--{stage.replace('_', '-')}-sha256", help=f"SHA-256 of {stage}/manifest.json")

    @staticmethod
    def check(selected, options: dict):
        names = [f for f in selected if f in FIELDS]
        if not names:
            return
        if options.get("sec_stages") is None:
            raise ValueError("--fields: SEC fields need --sec-stages (the atx-db alpha-panel build root)")
        if options.get("sec_identity_bridge") is None or not options.get("sec_identity_bridge_sha256"):
            raise ValueError("--fields: SEC fields need --sec-identity-bridge and --sec-identity-bridge-sha256")
        for name in names:
            for stage in FIELDS[name]["stages"]:
                if not options.get(f"{stage}_sha256"):
                    raise ValueError(f"--fields: {name} needs --{stage.replace('_', '-')}-sha256")

    # -- inputs --------------------------------------------------------------------------------------------------
    def _stage(self, options, stage):
        directory = Path(options["sec_stages"]) / STAGES[stage][0]
        m, src = self.h.pinned_manifest(directory, options[f"{stage}_sha256"], stage.replace("_", "-"), STAGES[stage][1])
        return directory, m, src

    def _verified(self, directory: Path, manifest: dict, rel: str, budget):
        entry = (manifest.get("files") or {}).get(rel)
        if not isinstance(entry, dict):
            raise ValueError(f"{directory.name}: manifest does not list {rel}")
        blob = (directory / rel).read_bytes()
        digest = hashlib.sha256(blob).hexdigest()
        if len(blob) != int(entry.get("bytes", -1)) or digest != entry.get("sha256"):
            raise ValueError(f"{directory.name}: {rel} does not match its manifest entry")
        budget.check(f"sec-read-{directory.name}")
        return blob, {"path": str((directory / rel).resolve()), "bytes": len(blob), "sha256": digest}

    def _batches(self, blob: bytes, columns, what: str):
        pf = pq.ParquetFile(pa.BufferReader(blob))
        missing = [c for c in columns if c not in pf.schema_arrow.names]
        if missing:
            raise ValueError(f"{what} lacks contract column(s) {missing}")
        return pf.iter_batches(batch_size=1 << 18, columns=list(columns), use_threads=False)

    # -- D2 earnings calendar ------------------------------------------------------------------------------------
    def _earnings(self, options, ciks, cal: Calendar, end_ns: int, budget):
        h = self.h
        directory, m, man_src = self._stage(options, "earnings_calendar")
        blob, src = self._verified(directory, m, "announcements.parquet", budget)
        dtypes = {"cidx": np.int64, "avail": np.int64, "acc": np.int64, "sdate": np.int64, "react": np.int64,
                  "nexp": np.int64, "timing": np.float64, "yoy": np.bool_, "err": np.float64}
        parts = {k: [] for k in dtypes}
        st = {"rows_total": 0, "rows_non_primary": 0, "rows_sealed": 0, "rows_after_role": 0, "rows_unlinked_cik": 0}
        timing_names = pa.array(list(EA_TIMING_CODES))
        timing_codes = np.append(np.array(list(EA_TIMING_CODES.values())), np.nan)   # unknown timing -> NaN
        for batch in self._batches(blob, _EA_COLS, "announcements.parquet"):
            tb = pa.Table.from_batches([batch])
            st["rows_total"] += tb.num_rows
            primary = pc.fill_null(h.column_of(tb, "is_primary"), False).to_numpy(zero_copy_only=False)
            avail = h.as_instants_ns(h.column_of(tb, "available_at"), "announcements available_at")
            cik = h.as_ids(h.column_of(tb, "cik"), "announcements cik")
            pos, linked = cik_positions(ciks, cik)
            sealed = avail >= h.SEAL_NS
            late = ~sealed & (avail >= end_ns)
            st["rows_non_primary"] += int(np.count_nonzero(~primary))
            st["rows_sealed"] += int(np.count_nonzero(primary & sealed))
            st["rows_after_role"] += int(np.count_nonzero(primary & late))
            st["rows_unlinked_cik"] += int(np.count_nonzero(primary & ~sealed & ~late & ~linked))
            keep = np.flatnonzero(primary & ~sealed & ~late & linked)
            if not len(keep):
                continue
            tb = tb.take(pa.array(keep))
            slot = pc.fill_null(pc.index_in(h.as_text(h.column_of(tb, "timing")), value_set=timing_names),
                                len(EA_TIMING_CODES)).to_numpy(zero_copy_only=False)
            for k, v in (("cidx", pos[keep]), ("avail", avail[keep]),
                         ("acc", accession_keys(h.column_of(tb, "accession"))),
                         ("sdate", h.as_days(h.column_of(tb, "session_date"), "announcements session_date")),
                         ("react", h.as_days(h.column_of(tb, "reaction_session"), "announcements reaction_session")),
                         ("nexp", h.as_days(h.column_of(tb, "next_expected_date"), "announcements next_expected_date")),
                         ("timing", timing_codes[slot]),
                         ("yoy", pc.equal(h.as_text(h.column_of(tb, "expected_rule")), "yoy_364")
                          .to_numpy(zero_copy_only=False)),
                         ("err", h.as_f64(h.column_of(tb, "expected_error_days"), "announcements expected_error_days"))):
                parts[k].append(v)
            budget.check("sec-earnings-batch")
        del blob
        cat = {k: (np.concatenate(v) if v else np.empty(0)).astype(dtypes[k]) for k, v in parts.items()}
        codes = cat["timing"]
        st["rows_used"] = int(len(cat["cidx"]))
        st["rows_used_unknown_timing"] = int(np.count_nonzero(np.isnan(codes)))
        # per-CIK clock order (available_at, accession): pending expectations of each row over its CIK's prior rows
        o = np.lexsort((cat["acc"], cat["avail"], cat["cidx"]))
        c, s, e = cat["cidx"][o], cat["sdate"][o], cat["nexp"][o]
        best = np.full(len(o), np.iinfo(np.int64).max, dtype=np.int64)
        for j in range(min(EA_LOOKBACK_ROWS + 1, len(o))):
            same = np.zeros(len(o), dtype=bool)
            same[j:] = c[j:] == c[:len(o) - j]
            cand = np.full(len(o), np.iinfo(np.int64).max, dtype=np.int64)
            cand[j:] = e[:len(o) - j]
            best = np.where(same & (cand > s + EA_CONSUME_DAYS), np.minimum(best, cand), best)
        same = np.zeros(len(o), dtype=bool)
        k = EA_LOOKBACK_ROWS + 1
        if len(o) > k:
            same[k:] = (c[k:] == c[:len(o) - k]) & (e[:len(o) - k] > s[k:] + EA_CONSUME_DAYS)
        st["lookback_truncated_rows"] = int(np.count_nonzero(same))   # 0: the 16-row lookback is exact
        fallback = best > s + EA_FALLBACK_TRIGGER_DAYS
        expected = np.where(fallback, s + EA_FALLBACK_DAYS, best)
        st["rows_expectation_fallback"] = int(np.count_nonzero(fallback))
        inv = np.empty_like(o)
        inv[o] = np.arange(len(o))
        expected = expected[inv]
        # global clock order for the Latest pointer
        g = np.lexsort((cat["acc"], cat["avail"]))
        exp_idx = cal.at_or_after(expected[g])
        on_cal = lambda idx, days: (cal.days[np.minimum(idx, len(cal.days) - 1)] == days)
        in_range = lambda days: (days >= cal.days[0]) & (days <= cal.days[-1])
        st["expected_not_on_calendar_rows"] = int(np.count_nonzero(
            in_range(expected[g]) & ~on_cal(exp_idx, expected[g]) & ~fallback[inv][g]))
        react_idx = cal.at_or_after(cat["react"][g])
        st["reaction_not_on_calendar_rows"] = int(np.count_nonzero(
            in_range(cat["react"][g]) & ~on_cal(react_idx, cat["react"][g])))
        ea = {"usable": cal.usable_from(cat["avail"][g]), "cidx": cat["cidx"][g], "sdate": cat["sdate"][g],
              "react_idx": react_idx, "exp_idx": exp_idx, "timing": codes[g],
              "delay": np.where(cat["yoy"][g], cat["err"][g], np.nan)}
        st["rows_used_with_yoy_delay"] = int(np.count_nonzero(np.isfinite(ea["delay"])))
        return ea, [man_src, src], st, {"earnings_calendar": man_src["sha256"]}

    # -- D11 Form 4 ----------------------------------------------------------------------------------------------
    def _insider(self, options, ciks, cal: Calendar, end_ns: int, budget):
        h = self.h
        directory, m, man_src = self._stage(options, "insider")
        rels = sorted(k for k in (m.get("files") or {}) if re.match(r"^transactions/year=\d{4}/\d{4}q[1-4]\.parquet$", k))
        quarters = [re.search(r"(\d{4})q([1-4])", r).groups() for r in rels]
        qdays = [dt.date(int(y), 3 * int(q) - 2, 1) for y, q in quarters]
        if not rels:
            raise ValueError("insider: manifest lists no transactions/year=YYYY/YYYYqN.parquet")
        for a, b in zip(qdays, qdays[1:]):
            if (b.year * 12 + b.month) - (a.year * 12 + a.month) != 3:
                raise ValueError(f"insider: filing quarters are not contiguous ({a} -> {b})")
        epoch = dt.date(1970, 1, 1)
        start_day = (qdays[0] - epoch).days
        end_day = end_ns // cal.day_ns
        st = {"quarters_listed": f"{quarters[0][0]}q{quarters[0][1]}..{quarters[-1][0]}q{quarters[-1][1]}",
              "first_filing_day": str(qdays[0]), "files_read": 0, "files_not_read_after_role": 0, "rows_total": 0,
              "rows_sealed": 0, "rows_after_role": 0, "rows_unlinked_issuer": 0, "presence_rows": 0,
              "trade_rows": 0, "dropped_not_ps_or_derivative": 0, "dropped_form_not_original_4": 0,
              "dropped_not_director_or_officer": 0, "dropped_joint_filing_with_ten_percent_owner": 0,
              "dropped_shares_not_positive": 0,
              "dropped_code_direction_mismatch": 0}
        files, pres_c, pres_a, tr = [], [], [], {k: [] for k in ("c", "o", "a", "d", "sh", "buy")}
        for rel, qday in zip(rels, qdays):
            if (qday - epoch).days > end_day + INS_SKIP_MARGIN_DAYS:
                st["files_not_read_after_role"] += 1   # filed from a quarter start well after the role's last mark
                continue
            blob, src = self._verified(directory, m, rel, budget)
            files.append([rel, src["bytes"], src["sha256"]])
            st["files_read"] += 1
            for batch in self._batches(blob, _INS_COLS, rel):
                tb = pa.Table.from_batches([batch])
                st["rows_total"] += tb.num_rows
                avail = h.as_instants_ns(h.column_of(tb, "available_at"), "insider available_at")
                cik = h.as_ids(h.column_of(tb, "issuer_cik"), "insider issuer_cik")
                pos, linked = cik_positions(ciks, cik)
                sealed = avail >= h.SEAL_NS
                late = ~sealed & (avail >= end_ns)
                st["rows_sealed"] += int(np.count_nonzero(sealed))
                st["rows_after_role"] += int(np.count_nonzero(late))
                st["rows_unlinked_issuer"] += int(np.count_nonzero(~sealed & ~late & ~linked))
                use = ~sealed & ~late & linked
                pair = np.unique(np.stack((avail[use], pos[use])), axis=1)   # one row per (filing clock, CIK)
                pres_a.append(pair[0])
                pres_c.append(pair[1])
                st["presence_rows"] += int(np.count_nonzero(use))
                code = h.as_text(h.column_of(tb, "transaction_code"))
                ps = (pc.equal(h.as_text(h.column_of(tb, "table_type")), "non_derivative").to_numpy(zero_copy_only=False)
                      & pc.is_in(code, value_set=pa.array(["P", "S"])).to_numpy(zero_copy_only=False))
                orig4 = (pc.equal(h.as_text(h.column_of(tb, "form")), "4").to_numpy(zero_copy_only=False)
                         & ~pc.fill_null(h.column_of(tb, "is_amendment"), True).to_numpy(zero_copy_only=False))
                insider = (pc.fill_null(h.column_of(tb, "any_director"), False).to_numpy(zero_copy_only=False)
                           | pc.fill_null(h.column_of(tb, "any_officer"), False).to_numpy(zero_copy_only=False))
                joint10 = ((pc.fill_null(h.column_of(tb, "n_reporting_owners"), 1).to_numpy(zero_copy_only=False) > 1)
                           & pc.fill_null(h.column_of(tb, "any_ten_percent_owner"), False).to_numpy(zero_copy_only=False))
                shares = h.as_f64(h.column_of(tb, "shares"), "insider shares")
                buy = pc.equal(code, "P").to_numpy(zero_copy_only=False)
                direction = h.as_text(h.column_of(tb, "acquired_disposed"))
                agree = np.where(buy, pc.equal(direction, "A").to_numpy(zero_copy_only=False),
                                 pc.equal(direction, "D").to_numpy(zero_copy_only=False))
                owner = pc.fill_null(h.column_of(tb, "owner_cik"), 0).to_numpy(zero_copy_only=False).astype(np.int64)
                tdate = h.column_of(tb, "transaction_date")
                has_date = tdate.is_valid().to_numpy(zero_copy_only=False)
                st["dropped_not_ps_or_derivative"] += int(np.count_nonzero(use & ~ps))
                st["dropped_form_not_original_4"] += int(np.count_nonzero(use & ps & ~orig4))
                st["dropped_not_director_or_officer"] += int(np.count_nonzero(use & ps & orig4 & ~insider))
                st["dropped_joint_filing_with_ten_percent_owner"] += int(np.count_nonzero(use & ps & orig4 & insider
                                                                                          & joint10))
                ok = use & ps & orig4 & insider & ~joint10
                good = np.isfinite(shares) & (shares > 0) & (owner > 0) & has_date
                st["dropped_shares_not_positive"] += int(np.count_nonzero(ok & ~good))
                st["dropped_code_direction_mismatch"] += int(np.count_nonzero(ok & good & ~agree))
                t = np.flatnonzero(ok & good & agree)
                tr["c"].append(pos[t])
                tr["o"].append(owner[t])
                tr["a"].append(avail[t])
                tr["d"].append(h.as_days(tdate.take(pa.array(t)), "insider transaction_date"))
                tr["sh"].append(np.rint(shares[t] * INS_SHARE_SCALE).astype(np.int64))
                tr["buy"].append(buy[t])
            del blob
            budget.check("sec-insider-file")
        cat = lambda xs, dtype: np.concatenate(xs).astype(dtype) if xs else np.empty(0, dtype)
        pc_, pa_ = cat(pres_c, np.int64), cat(pres_a, np.int64)
        o = np.argsort(pa_, kind="stable")
        presence = {"usable": cal.usable_from(pa_[o]), "cidx": pc_[o], "avail": pa_[o]}
        T = {k: cat(v, np.bool_ if k == "buy" else np.int64) for k, v in tr.items()}
        st["trade_rows"] = int(len(T["c"]))
        st["trade_rows_buy"] = int(np.count_nonzero(T["buy"]))
        # Cohen-Malloy-Pomorski classification per (issuer, owner, trade year Y) from trades filed before Y-01-01
        years = (T["d"].astype("datetime64[D]").astype("datetime64[Y]").astype(np.int64) + 1970)
        months = T["d"].astype("datetime64[D]").astype("datetime64[M]").astype(np.int64) % 12
        opp = self._opportunistic(T["c"], T["o"], years, months, T["a"])
        budget.check("sec-insider-classify")
        st["trade_rows_opportunistic"] = int(np.count_nonzero(opp))
        first_full_year = qdays[0].year if (qdays[0].month, qdays[0].day) == (1, 1) else qdays[0].year + 1
        opp_first_day = (dt.date(first_full_year + INS_ROUTINE_YEARS, 1, 1) - epoch).days
        usable = cal.usable_from(T["a"])
        leave126 = cal.count_through(T["d"]) + INS_WINDOW
        leave21 = cal.count_through(T["d"]) + INS_CLUSTER_WINDOW
        signed = np.where(T["buy"], T["sh"], -T["sh"])
        n = len(ciks)
        ins = {"presence": presence, "start_day": start_day, "opp_first_day": opp_first_day,
               "net": Windowed(usable, leave126, T["c"], signed, n, np.int64),
               "opp": Windowed(usable, leave126, T["c"], np.where(opp, signed, 0), n, np.int64)}
        for key, side, leave in (("buyers", T["buy"], leave126), ("sellers", ~T["buy"], leave126),
                                 ("cluster", T["buy"], leave21)):
            rows = np.flatnonzero(side)
            first, a, b = merge_intervals([T["c"][rows], T["o"][rows]], usable[rows], leave[rows])
            ins[key] = Windowed(a, b, T["c"][rows][first], np.ones(len(first), dtype=np.int64), n, np.int64)
        files.sort()
        src = {"path": str((directory / "transactions").resolve()), "files_read": len(files),
               "files_sha256": hashlib.sha256(json.dumps(files, separators=(",", ":")).encode()).hexdigest(),
               "files_list_rule": "sha256 of compact JSON of the sorted [relative path, bytes, SHA-256] of every "
                                  "transactions file read, each verified against the insider manifest"}
        return ins, [man_src, src], st, {"insider": man_src["sha256"]}

    @staticmethod
    def _opportunistic(c, o, years, months, avail) -> np.ndarray:
        """Per trade row: True when its owner is opportunistic in the issuer (dense CIK index c) for the trade's
        calendar year Y (Cohen-Malloy-Pomorski): the owner's trades in Y-3..Y-1 filed before Y-01-01 00:00 UTC give
        one 12-bit month mask per year; eligible = all three masks non-empty; routine = a month common to all three;
        opportunistic = eligible and not routine."""
        valid = (years >= 1995) & (years <= 2100)
        if len(c) and (int(c.max()) >= (1 << 21) or int(o.max()) >= (1 << 34)):
            raise ValueError("insider: CIK index or owner CIK beyond the classification key width")
        key = lambda cc, oo, yy: (cc << 42) | (oo << 8) | (np.clip(yy, 1990, 2245) - 1990)
        year_start_ns = lambda yy: (yy - 1970).astype("datetime64[Y]").astype("datetime64[ns]").astype(np.int64)
        hk, hj, hm = [], [], []
        for j in range(1, INS_ROUTINE_YEARS + 1):
            ok = valid & (avail < year_start_ns(years + j))
            hk.append(key(c[ok], o[ok], years[ok] + j))
            hj.append(np.full(int(np.count_nonzero(ok)), j - 1, dtype=np.int64))
            hm.append(np.left_shift(np.int64(1), months[ok]).astype(np.int64))
        hk, hj, hm = np.concatenate(hk), np.concatenate(hj), np.concatenate(hm)
        if not len(hk):
            return np.zeros(len(c), dtype=bool)
        groups, inv = np.unique(hk, return_inverse=True)
        masks = np.zeros((len(groups), INS_ROUTINE_YEARS), dtype=np.int64)
        np.bitwise_or.at(masks, (inv, hj), hm)
        eligible = np.all(masks != 0, axis=1)
        routine = np.bitwise_and.reduce(masks, axis=1) != 0
        return valid & np.isin(key(c, o, years), groups[eligible & ~routine])

    # -- D12 8-K -------------------------------------------------------------------------------------------------
    def _eightk(self, options, ciks, cal: Calendar, end_ns: int, budget):
        h = self.h
        directory, m, man_src = self._stage(options, "sec_filings")
        blob, src = self._verified(directory, m, "eight_k_items.parquet", budget)
        st = {"rows_total": 0, "rows_amendment": 0, "rows_not_8k_form": 0, "rows_sealed": 0, "rows_after_role": 0,
              "rows_unlinked_cik": 0}
        parts = {k: [] for k in ("c", "acc", "a", "mat")}
        for batch in self._batches(blob, _K8_COLS, "eight_k_items.parquet"):
            tb = pa.Table.from_batches([batch])
            st["rows_total"] += tb.num_rows
            form = h.as_text(h.column_of(tb, "form"))
            is8k = pc.starts_with(form, "8-K").to_numpy(zero_copy_only=False)
            amend = pc.fill_null(h.column_of(tb, "is_amendment"), True).to_numpy(zero_copy_only=False)
            avail = h.as_instants_ns(h.column_of(tb, "available_at"), "eight_k_items available_at")
            cik = h.as_ids(h.column_of(tb, "cik"), "eight_k_items cik")
            pos, linked = cik_positions(ciks, cik)
            sealed = avail >= h.SEAL_NS
            late = ~sealed & (avail >= end_ns)
            st["rows_not_8k_form"] += int(np.count_nonzero(~is8k))
            st["rows_amendment"] += int(np.count_nonzero(is8k & amend))
            base = is8k & ~amend
            st["rows_sealed"] += int(np.count_nonzero(base & sealed))
            st["rows_after_role"] += int(np.count_nonzero(base & late))
            st["rows_unlinked_cik"] += int(np.count_nonzero(base & ~sealed & ~late & ~linked))
            keep = np.flatnonzero(base & ~sealed & ~late & linked)
            if not len(keep):
                continue
            tb = tb.take(pa.array(keep))
            parts["c"].append(pos[keep])
            parts["acc"].append(accession_keys(h.column_of(tb, "accession")))
            parts["a"].append(avail[keep])
            parts["mat"].append(pc.is_in(h.as_text(h.column_of(tb, "item")),
                                         value_set=pa.array(list(K8_MATERIAL_ITEMS))).to_numpy(zero_copy_only=False))
            budget.check("sec-8k-batch")
        del blob
        cat = lambda xs, dtype: np.concatenate(xs).astype(dtype) if xs else np.empty(0, dtype)
        c, acc, a, mat = cat(parts["c"], np.int64), cat(parts["acc"], np.int64), cat(parts["a"], np.int64), \
            cat(parts["mat"], np.bool_)
        # one row per (cik, accession): items share the filing's clock
        o = np.lexsort((acc, c))
        c, acc, a, mat = c[o], acc[o], a[o], mat[o]
        new = np.ones(len(c), dtype=bool)
        new[1:] = (c[1:] != c[:-1]) | (acc[1:] != acc[:-1])
        first = np.flatnonzero(new)
        if len(first):
            clock_ok = np.minimum.reduceat(a, first) == np.maximum.reduceat(a, first)
            st["accessions_with_item_clock_disagreement"] = int(np.count_nonzero(~clock_ok))
            a_f = np.maximum.reduceat(a, first)   # the later clock if items disagree (conservative)
            mat_f = np.logical_or.reduceat(mat, first)
        else:
            a_f, mat_f = np.empty(0, np.int64), np.empty(0, bool)
            st["accessions_with_item_clock_disagreement"] = 0
        c_f, acc_f = c[first], acc[first]
        st["accessions_used"] = int(len(first))
        st["accessions_used_material"] = int(np.count_nonzero(mat_f))
        g = np.lexsort((acc_f, a_f))
        c_f, a_f, mat_f = c_f[g], a_f[g], mat_f[g]
        usable = cal.usable_from(a_f)
        n = len(ciks)
        ones = np.ones(len(c_f), dtype=np.int64)
        k8 = {"latest": Latest(usable, c_f, n), "event": usable - 1, "avail": a_f,
              "count": Windowed(usable, usable + K8_COUNT_WINDOW, c_f, ones, n, np.int64),
              "material": Windowed(usable[mat_f], usable[mat_f] + K8_MATERIAL_WINDOW, c_f[mat_f], ones[mat_f], n,
                                   np.int64)}
        return k8, [man_src, src], st, {"sec_filings": man_src["sha256"]}

    # -- orchestration -------------------------------------------------------------------------------------------
    def compute(self, names, role, output: Path, budget, options: dict, outcome: dict, source_checks: dict,
                field_extras: dict):
        if not names:
            return
        h = self.h
        nd, n = role.n_dates, role.n
        budget.admit(nd * n * 5 + (128 << 20), "sec-link-matrix")
        links, bridge_sources, bridge_st = h.load_bridge(Path(options["sec_identity_bridge"]),
                                                         options["sec_identity_bridge_sha256"], role, budget)
        marks = role.days * h.DAY_NS + h.MARK_NS
        ciks, link, primary, link_st = h.resolve_links(links, role, marks, budget)
        del links
        cal = Calendar(role, h.DAY_NS, h.MARK_NS)
        end_ns = int(marks[-1])   # a row at or after the last role mark is never usable in the role
        want = set(names)
        st = {"lag_sessions": SEC_LAG_SESSIONS, "clock": SEC_CLOCK, "calendar": cal.stats,
              "identity_bridge": {**bridge_st, **link_st}}
        sources = {x: list(bridge_sources) for x in names}
        pins = {}
        ea = ins = k8 = None
        for group, fn, key in ((EA_NAMES, self._earnings, "earnings_calendar"), (INS_NAMES, self._insider, "insider"),
                               (K8_NAMES, self._eightk, "sec_filings")):
            if want & set(group):
                data, src, s, pin = fn(options, ciks, cal, end_ns, budget)
                st[key] = s
                pins.update(pin)
                for x in names:
                    if x in group:
                        sources[x] += src
                if key == "earnings_calendar":
                    ea = data
                elif key == "insider":
                    ins = data
                else:
                    k8 = data
                pa.default_memory_pool().release_unused()   # decoded Arrow buffers are dead once the arrays are built
                budget.report(f"sec-{key}-loaded", **{k: v for k, v in s.items() if k.endswith("used")
                                                      or k in ("trade_rows", "accessions_used", "files_read")})
        need_so = bool(want & {"ins_net_buy_ratio", "ins_opportunistic_net"})
        reasons = {x: {"not_primary_link": 0, "absent_or_stale": 0, "out_of_rule": 0} for x in names}
        domain_nan = {"ins_net_buy_ratio": 0, "ins_opportunistic_net": 0}
        per_year = {}
        writers, so_file = {}, None
        try:
            for x in names:
                writers[x] = h.FieldWriter(output, x, role)
            if need_so:
                so_path = output / "shares_out.f64"
                if not so_path.is_file() or so_path.stat().st_size != nd * n * 8:
                    raise ValueError("SEC insider ratios: this run's shares_out.f64 is missing or has the wrong size")
                so_file = so_path.open("rb")
            ea_latest = Latest(ea["usable"], ea["cidx"], len(ciks)) if ea is not None else None
            ins_latest = Latest(ins["presence"]["usable"], ins["presence"]["cidx"], len(ciks)) if ins is not None else None
            for t in range(nd):
                u = t + cal.prefix
                day = int(cal.days[u])
                member = role.member[t] != 0
                lk = link[t]
                safe = np.maximum(lk, 0)
                pline = (lk >= 0) & primary[t]
                y = per_year.setdefault(str(int(role.years[t])), {"member_primary_cells": 0,
                                                                  **{x: 0 for x in names}})
                y["member_primary_cells"] += int(np.count_nonzero(member & pline))
                rows = {}
                if ea is not None:
                    latest = ea_latest.at(u)
                    k = np.where(pline, latest[safe] if len(ciks) else -1, -1)
                    has = k >= 0
                    ks = np.maximum(k, 0)
                    if len(ea["cidx"]):
                        fresh = has & ((day - ea["sdate"][ks]) <= EA_STALE_DAYS)
                        since = (u - ea["react_idx"][ks]).astype(np.float64)
                        to = (ea["exp_idx"][ks] - u).astype(np.float64)
                        timing, delay = ea["timing"][ks], ea["delay"][ks]
                    else:
                        fresh = np.zeros(n, dtype=bool)
                        since = to = timing = delay = np.full(n, np.nan)
                    live = fresh & (to >= -EA_OVERDUE_MAX_SESSIONS)
                    rows["ea_days_to_expected"] = (np.where(live, to, np.nan), fresh)
                    rows["ea_window_pre5"] = (np.where(live, ((to >= 1) & (to <= 5)).astype(np.float64), np.nan), fresh)
                    rows["ea_days_since"] = (np.where(fresh, since, np.nan), fresh)
                    rows["ea_window_post3"] = (np.where(fresh, ((since >= 0) & (since <= 3)).astype(np.float64),
                                                        np.nan), fresh)
                    rows["ea_delay_days"] = (np.where(fresh, delay, np.nan), fresh)
                    rows["ea_time_of_day"] = (np.where(fresh, timing, np.nan), fresh)
                if ins is not None:
                    latest = ins_latest.at(u)
                    k = np.where(pline, latest[safe] if len(ciks) else -1, -1)
                    last_avail = np.where(k >= 0, ins["presence"]["avail"][np.maximum(k, 0)]
                                          if len(ins["presence"]["avail"]) else 0, np.iinfo(np.int64).min)
                    cutoff = int(cal.marks[u - 1]) - INS_PRESENCE_DAYS * h.DAY_NS
                    history = (int(cal.days[u - INS_WINDOW]) >= ins["start_day"]
                               and cutoff >= ins["start_day"] * h.DAY_NS)
                    present = pline & (last_avail >= cutoff) & history
                    opp_ok = int(cal.days[u - INS_WINDOW]) >= ins["opp_first_day"]
                    net, opp = ins["net"].at(u)[safe], ins["opp"].at(u)[safe]
                    nb, ns_, cl = ins["buyers"].at(u)[safe], ins["sellers"].at(u)[safe], ins["cluster"].at(u)[safe]
                    if so_file is not None:
                        blob = so_file.read(n * 8)
                        if len(blob) != n * 8:
                            raise ValueError("SEC insider ratios: this run's shares_out.f64 is truncated")
                        so = np.frombuffer(blob, dtype="<f8")
                        sok = np.isfinite(so) & (so > 0)
                        with np.errstate(invalid="ignore", divide="ignore"):
                            ratio = net / INS_SHARE_SCALE / np.where(sok, so, 1.0)
                            oratio = opp / INS_SHARE_SCALE / np.where(sok, so, 1.0)
                        lo, hi = INS_RATIO_DOMAIN
                        rows["ins_net_buy_ratio"] = (np.where(present & sok & (ratio >= lo) & (ratio <= hi), ratio,
                                                              np.nan), present)
                        rows["ins_opportunistic_net"] = (np.where(present & sok & opp_ok & (oratio >= lo)
                                                                  & (oratio <= hi), oratio, np.nan), present)
                        for x, r in (("ins_net_buy_ratio", ratio), ("ins_opportunistic_net", oratio)):
                            domain_nan[x] += int(np.count_nonzero(member & present & sok & ((r < lo) | (r > hi))))
                    rows["ins_n_buyers"] = (np.where(present, nb.astype(np.float64), np.nan), present)
                    rows["ins_n_sellers"] = (np.where(present, ns_.astype(np.float64), np.nan), present)
                    rows["ins_cluster_buy"] = (np.where(present, (cl >= INS_CLUSTER_MIN).astype(np.float64), np.nan),
                                               present)
                if k8 is not None:
                    latest = k8["latest"].at(u)
                    k = np.where(pline, latest[safe] if len(ciks) else -1, -1)
                    ks = np.maximum(k, 0)
                    if len(k8["avail"]):
                        cutoff = int(cal.marks[u - 1]) - K8_PRESENCE_DAYS * h.DAY_NS
                        present = (k >= 0) & (k8["avail"][ks] >= cutoff)
                        since = (u - k8["event"][ks]).astype(np.float64)
                    else:
                        present, since = np.zeros(n, dtype=bool), np.full(n, np.nan)
                    cnt, mat = k8["count"].at(u)[safe], k8["material"].at(u)[safe]
                    rows["k8_count_63"] = (np.where(present, cnt.astype(np.float64), np.nan), present)
                    rows["k8_item_material_21"] = (np.where(present, (mat > 0).astype(np.float64), np.nan), present)
                    rows["k8_days_since_any"] = (np.where(present, since, np.nan), present)
                for x in names:
                    row, base = rows[x]
                    writers[x].write(row)
                    finite = np.isfinite(row)
                    r = reasons[x]
                    r["not_primary_link"] += int(np.count_nonzero(member & ~pline))
                    r["absent_or_stale"] += int(np.count_nonzero(member & pline & ~base))
                    r["out_of_rule"] += int(np.count_nonzero(member & pline & base & ~finite))
                    y[x] += int(np.count_nonzero(member & finite))
                if t % 256 == 0:
                    budget.check("sec-write")
            if so_file is not None and so_file.read(1):
                raise ValueError("SEC insider ratios: this run's shares_out.f64 is longer than the role shape")
        except BaseException:
            for w in writers.values():
                w.f.close()   # refused: partial files stay unpublished (no manifest), handles released
            raise
        finally:
            if so_file is not None:
                so_file.close()
        for x in names:
            writers[x].close()
        source_checks[GROUP] = st
        for x in names:
            spec = FIELDS[x]
            stages = {s: {"path": str((Path(options["sec_stages"]) / STAGES[s][0] / "manifest.json").resolve()),
                          "sha256": pins[s]} for s in spec["stages"]}
            field_extras[x] = {
                "formula_id": spec["formula_id"],
                "formula_sha256": h.formula_id(x, h.spec_definition(x, SEC_LAG_SESSIONS)),
                "lag_sessions": SEC_LAG_SESSIONS, "lag": spec["lag"], "min_history": spec["min_history"],
                "nan_rule": spec["staleness"], "stage_manifests": stages,
                "identity_bridge_manifest_sha256": options["sec_identity_bridge_sha256"].lower(),
                "nan_reasons_member_cells": reasons[x],
                "coverage_linked_primary": {yr: {"member_primary_cells": v["member_primary_cells"],
                                                 "finite_member_cells": v[x],
                                                 "finite_frac": round(v[x] / v["member_primary_cells"], 6)
                                                 if v["member_primary_cells"] else None}
                                            for yr, v in sorted(per_year.items())}}
            if "domain" in spec:
                field_extras[x]["domain"] = list(spec["domain"])
                field_extras[x]["outside_domain_member_cells"] = domain_nan[x]
            outcome[x] = (writers[x], sources[x], writers[x].coverage())
        budget.report("sec-complete", fields=len(names))
