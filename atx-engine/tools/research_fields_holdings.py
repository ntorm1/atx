"""Point-in-time research fields from the atx-db holdings and short-side stages (platform v7 lane W5b).

Registered into ``prepare_research_fields.py`` as a late ``FIELD_MODULES`` module (``FIELD_MODULES.append(_holdings.
bind(globals()))``, lane A1 of P9; until then a wrap of the builder's ``run`` and ``main``). When none of
``HOLD_FIELDS`` is requested nothing of this module runs (every byte of every other field and manifest entry is the
same). When some are, ``check`` pins the stages before any output, the builder computes the other fields and assembles
its manifest, and ``finish`` writes this module's ``<field>.f64`` payloads into the same exclusive output directory and
appends its manifest entries (registry order, after the builder's) and ``source_checks.holdings`` before the manifest
is published. A run may request holdings fields only.
With ``--reuse`` (v8 C-3) a holdings field is copied from the prior directory when its producing code (``PRODUCERS``
kind closure plus the builder code it reads through ``ns``), stage manifest pins, research seal (review N-1: each
entry records ``seal_date``), formula and dependencies are unchanged (prepare_research_fields.REUSE_MODULE_RULE); every
other holdings field is computed.

Sources (atx-db alpha panel v1 stages, read-only; each pinned by the SHA-256 of its ``manifest.json``, which must be a
complete manifest of the declared schema with the declared clock and staleness rules; every file read is hash-checked
against that manifest from the exact bytes parsed; nothing available on or after the research seal, ``research_window.py``
``SEAL_DATE``, is used):
* ``thirteenf/`` (D1): ``filings.parquet``, ``filing_checks.parquet``, ``cusip_map_pit.parquet``, ``agg_asof45.parquet``
  and the ``parts/source=*/holdings.parquet`` data sets of the effective filings.
* ``ftd/`` (D3a): ``year=YYYY/ftd.parquet``.
* ``regsho_threshold/`` (D3b): ``lists.parquet`` and ``year=YYYY/threshold.parquet``; the listing market of a line comes
  from ``security_master/finra_names.parquet`` (FINRA short-interest rows, PIT at their dissemination).
* ``short_volume_ext/`` (D3c): ``year=YYYY/short_volume_ext.parquet``; the role's own ``volume.f64``/``present.u8``.
* ``security_master/finra_names.parquet`` also feeds ``exch_up_365d`` (platform v8 LIB2, kind ``xsw``): a recent move of
  the line's listing up to NYSE or NYSE American (Dharan and Ikenberry 1995), decided row by row as the name rows
  become visible (``XSW_RULE``). Opt-in: no other kind's producing code reaches it.

Visibility (the v7 data-request house rule, ``VISIBILITY_RULE``): a source row is usable at role session t iff its
``available_at < date(t-1) 22:00 UTC`` (t-1 = the previous role session), so session 0 is NaN in every field. Field
definitions, clocks, staleness and NaN rules are in ``HOLD_FIELDS``; the declared constants below were fixed before any
field value was read. No field reads a return, and no statistic conditioned on returns is computed.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import sys

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

import research_window as rw  # same directory: the research window (the seal)

GROUP = "holdings"
EPOCH = dt.date(1970, 1, 1)
DAY_NS = 86_400_000_000_000
MARK_NS = 22 * 3_600_000_000_000
SEAL = rw.SEAL  # first sealed date (research_window.py)
SEAL_NS = (SEAL - EPOCH).days * DAY_NS
NEVER = np.iinfo(np.int64).max
BEFORE_ALL = np.iinfo(np.int64).min

# Stage contracts (kwarg / CLI name -> schema and the declared rules the stage manifest must carry).
STAGES = {
    "thirteenf": {"schema": "atx.alpha-panel.thirteenf/v1", "clock_rule": "13f-filed-plus-46h-v1", "staleness_days": 150},
    "ftd": {"schema": "atx.alpha-panel.ftd/v1", "clock_rule": "ftd-publication-halfmonth-v1", "staleness_days": 60},
    "regsho_threshold": {"schema": "atx.alpha-panel.regsho-threshold/v1", "clock_rule": "regsho-publication-v1",
                         "staleness_days": 10},
    "security_master": {"schema": "atx.alpha-panel.security-master/v1", "rule": "security-master-v1"},
    "short_volume_ext": {"schema": "atx.alpha-panel.short-volume-ext/v1", "clock_rule": "sv-ext-next-day-v1",
                         "staleness_days": 5},
}
KIND_STAGES = {"13f": ("thirteenf",), "ftd": ("ftd",), "regsho": ("regsho_threshold", "security_master"),
               "svx": ("short_volume_ext",), "xsw": ("security_master",)}
# v8 C-3 --reuse (prepare_research_fields.reuse_module_fields): each kind's producing code is its builder function and
# every module-level definition it reaches, plus the builder code it reads through ``ns`` / ``ctx.ns``.
PRODUCERS = {"13f": ("build_13f",), "ftd": ("build_ftd",), "regsho": ("build_regsho",), "svx": ("build_svx",),
             "xsw": ("build_xsw",)}
HOST_HANDLES = ("ns",)
KIND_CHECK_KEY = {"13f": "thirteenf", "ftd": "ftd", "regsho": "regsho_threshold", "svx": "short_volume_ext",
                  "xsw": "listing_switch"}
STAGE_KWARGS = tuple(k for s in STAGES for k in (s, s + "_sha256"))

VISIBILITY_RULE = ("a source row is usable at role session t iff available_at < date(t-1) 22:00 UTC, t-1 = the previous "
                   "role session (v7 data request section 0.1); session 0 -> NaN")
# 13F (D1)
F13_STALE_DAYS = 150            # stage staleness: NaN when date(t) - period_of_report > 150 days
F13_PRICE_OUTLIER = 10.0        # row implied price outside [1/10, 10] x the cross-filer median of its security: dropped
F13_IO_MAX = 2.0                # declared plausibility: an institutional ownership share above 2 -> NaN (counted)
F13_BASE_FORMS = ("13F-HR",)
F13_FORMS = ("13F-HR", "13F-HR/A")
F13_EFFECTIVE_RULE = (
    "13f-asof45-effective-v1: per (filer CIK, period_q) among 13F-HR / 13F-HR/A filings with filing_date <= the stage "
    "deadline (first SEC business day >= period + 45 d): base = the latest (filing_date, accession) 13F-HR or "
    "amendment_type RESTATEMENT; plus every NEW HOLDINGS amendment after the base; a 13F-HR/A without an amendment "
    "type and every filing after the deadline are not used (counted)")
F13_ROWS_RULE = (
    "holdings rows of the effective filings with sshprnamt_type SH, no put/call, shares > 0 and a finite value_usd >= 0; "
    "value_usd x filing_checks.unit_factor of the accession (1 when unlisted); CUSIP -> security_id by "
    "cusip_map_pit.parquet at the same period_q; a mapped row with value > 0 whose implied price value/shares lies "
    "outside [1/10, 10] x the median implied price of its security's rows that quarter is dropped (declared screen "
    "for share / value unit errors)")
F13_CLOCK = (
    "13f-quarter-asof45-v1: the quarter P becomes visible at V(P) = max available_at (stage 13f-filed-plus-46h-v1: "
    "filing_date 00:00 UTC + 46 h) over every 13F filing of P filed by its deadline (effective or not, notices "
    "included), uniform across securities; session t reads the latest quarter P with V(P) < date(t-1) 22:00 UTC and "
    "date(t) - P <= 150 days in which the security has an agg_asof45 row (forward fill across a quarter without a row); "
    "every 13F field of the session reads that same anchor quarter (a NaN there stays NaN, no per-field skip-back)")
F13_STALENESS = ("date(t) - period_of_report > 150 days -> NaN (stage staleness_days 150); no visible fresh quarter with "
                 "an agg_asof45 row for the security -> NaN; session 0 -> NaN")
F13_CAVEATS = [
    "the 13F clock is the filing date + 46 h (the data sets carry no acceptance time); quarters are published only "
    "after their 45-day deadline, so a value is 47-150 days old",
    "filer type is not classified (hedge / mutual fund): every 13F filer counts",
    "13F-HR/A amendments filed after the deadline (late confidential-treatment disclosures) are not used",
    "a quarter in which a security has no 13F row (no mapped holder) is bridged by the prior quarter until the "
    "150-day staleness; it is not read as zero holders",
]
# FTD (D3a)
FTD_WINDOW = 21
FTD_STALE_DAYS = 60
# Reg SHO threshold lists (D3b)
REGSHO_WINDOW = 63
REGSHO_MIN_LISTS = 60           # of the 63 window sessions, lists of the line's listing market that must be visible
REGSHO_NAME_MAX_AGE_DAYS = 45   # listing market from the latest visible FINRA name row no older than this (stage U3)
REGSHO_MARKETS = ("nasdaq", "nyse", "cboe_bzx", "finra_otc")
# FINRA marketClassCode -> the threshold list that covers the listing (NYSE's combined file covers NYSE, NYSE
# American and NYSE Arca; Nasdaq's covers the Global/Global Select (NNM) and Capital (SC) markets).
REGSHO_MARKET_OF_CLASS = {"NNM": "nasdaq", "SC": "nasdaq", "NYSE": "nyse", "AMEX": "nyse", "ARCA": "nyse",
                          "BZX": "cboe_bzx", "OTC": "finra_otc"}
REGSHO_LIST_STATUSES = ("list", "empty_list")
# Short volume with the off-exchange split (D3c)
SVX_WINDOW = 126
SVX_MIN_SESSIONS = 63
SVX_DOMAIN = (0.0, 1.0)
# Exchange listing up-switches (v8 LIB2; declared blind, before any field value was read)
XSW_WINDOW_DAYS = 365       # an up-switch counts while date(t) - its dissemination date <= 365 days ("past year")
XSW_MAX_GAP_DAYS = 45       # rows of a line more than 45 days apart are not compared; a latest row older than 45 days
                            # leaves the listing market unknown (NaN), as REGSHO_NAME_MAX_AGE_DAYS
XSW_VENUES = ("nasdaq", "amex", "nyse")
XSW_VENUE_OF_CLASS = {"NNM": "nasdaq", "SC": "nasdaq", "AMEX": "amex", "NYSE": "nyse"}   # any other class: other (-1)
XSW_UP = (("nasdaq", "nyse"), ("amex", "nyse"), ("nasdaq", "amex"))   # Dharan-Ikenberry's moves (CZ ExchSwitch)
XSW_NO_DAY = -(1 << 40)     # "no up-switch yet": far below any epoch day, so date(t) - it never fits the window
XSW_RULE = (
    "finra-listing-up-switch-v1: the listing venue of a FINRA name row is its market_class mapped NNM/SC -> nasdaq, "
    "AMEX -> amex (NYSE American), NYSE -> nyse, any other class (ARCA, BZX, OTC, IEX, null) -> other. The rows of "
    "the role's lines are taken in visibility order (available_at, then dissemination_date, then venue, then file "
    "order). Per line: "
    "the first row sets the current (dissemination date, venue); a row with a later dissemination date advances it and "
    "is an up-switch when current venue -> row venue is nasdaq -> nyse, amex -> nyse or nasdaq -> amex and the two "
    "dissemination dates are at most 45 days apart; a row on the current date with another venue makes the current "
    "venue other (conflict: the next row cannot switch from it); a row dated before the current one is ignored (out of "
    "order). An event is decided when its row becomes visible and is never revised by a later row")

_SESSION_T2 = "the window ends at session t-2, the newest session whose next-day file is visible before the t-1 mark"


def _spec(kind, formula_id, units, definition, clock, staleness, source_columns, caveats, requires=()):
    spec = {"group": GROUP, "kind": kind, "formula_id": formula_id, "point_in_time": True, "units": units,
            "definition": f"{definition} Formula id {formula_id}.", "clock": clock, "staleness": staleness,
            "source_columns": list(source_columns), "caveats": list(caveats)}
    if requires:
        spec["requires"] = list(requires)
    return spec


F13_COLUMNS = ["filings.accession", "filings.filer_cik", "filings.period_q", "filings.filing_date",
               "filings.submission_type", "filings.amendment_type", "filings.available_at", "filings.deadline",
               "agg_asof45.inst_shares", "agg_asof45.n_holders", "agg_asof45.available_at"]
F13_HOLD_COLUMNS = ["holdings.accession", "holdings.cusip", "holdings.shares", "holdings.sshprnamt_type",
                    "holdings.put_call", "holdings.value_usd", "filing_checks.unit_factor", "cusip_map_pit.security_id"]
HOLD_FIELDS = {
    "inst_own_share": _spec(
        "13f", "13f-asof45-io-share-qe-v1", "fraction of shares outstanding (decimal)",
        "13F institutional ownership at the anchor quarter P: agg_asof45 inst_shares of the security / this run's "
        "shares_out at the last role session on or before P (both in P's share basis); NaN when that session is before "
        "the role, shares_out there is not finite and positive, or the ratio exceeds 2 (declared plausibility, counted).",
        F13_CLOCK, F13_STALENESS, F13_COLUMNS + ["shares_out"],
        F13_CAVEATS + ["13F double counting (sub-advisers, multiple managers) can lift the share above 1",
                       "shares_out is the house A8 90-day lagged vendor count (one line, not the issuer total)"],
        requires=("shares_out",)),
    "inst_breadth_chg": _spec(
        "13f", "13f-asof45-breadth-chs-v1", "fraction of continuing 13F filers (decimal, signed)",
        "Chen-Hong-Stein breadth change at the anchor quarter P: (number of filers holding the security at P - number "
        "holding it at P-1) / number of filers, counting only filers with at least one screened holding row in both P "
        "and the preceding calendar quarter P-1; NaN when P-1 has no filings, or the security has no screened mapped "
        "row in P or in P-1 (unmapped then).",
        F13_CLOCK, F13_STALENESS, F13_COLUMNS + F13_HOLD_COLUMNS, F13_CAVEATS + [F13_ROWS_RULE]),
    "inst_own_chg_q": _spec(
        "13f", "13f-asof45-io-chg-q-v1", "change of the ownership fraction (decimal, signed)",
        "inst_own_share(P) - inst_own_share(P-1) for the anchor quarter P and the preceding calendar quarter P-1, each "
        "at its own quarter-end session (share-basis invariant); NaN when either is NaN.",
        F13_CLOCK, F13_STALENESS, F13_COLUMNS + ["shares_out"], F13_CAVEATS, requires=("shares_out",)),
    "inst_best_ideas": _spec(
        "13f", "13f-asof45-best-ideas-cps-v1", "sum of portfolio-weight excesses (decimal)",
        "Cohen-Polk-Silli conviction at the anchor quarter P: sum over filers m holding the security of max(0, w_m - "
        "mw), w_m = the security's screened value in m's filings / m's total screened long value (mapped or not), mw = "
        "the security's share of the total screened mapped 13F value of P (the 13F-universe market weight; no free-float "
        "market cap exists for the whole 13F universe at quarter end); NaN when the security has no screened mapped row "
        "in P.",
        F13_CLOCK, F13_STALENESS, F13_COLUMNS + F13_HOLD_COLUMNS,
        F13_CAVEATS + [F13_ROWS_RULE, "the market weight is the 13F-universe value weight, not the CRSP weight: a "
                       "low-IO name has a lower benchmark weight"]),
    "inst_n_holders": _spec(
        "13f", "13f-asof45-n-holders-v1", "count of 13F filers",
        "agg_asof45 n_holders of the security at the anchor quarter P (the stage aggregate: SH rows, no put/call, "
        "shares > 0, mapped, price-outlier rows excluded by the stage).",
        F13_CLOCK, F13_STALENESS, F13_COLUMNS, F13_CAVEATS),
    "ftd_shares_ratio21": _spec(
        "ftd", "sec-ftd-sum21-over-shares-v1", "sum of daily fail balances / shares outstanding (decimal)",
        "sum over the 21 settlement dates ending at S*(t) of the SEC CNS fails-to-deliver quantity of the line (a "
        "settlement date without a row of the line counts 0: the SEC file lists every non-zero fail) / this run's "
        "shares_out at the last role session on or before S*(t); S*(t) = the latest settlement date such that every "
        "settlement date up to it is visible; settlement calendar = the distinct settlement dates of the stage.",
        "sec-ftd-latest-visible-21-v1: rows visible by available_at (stage ftd-publication-halfmonth-v1: nominal "
        "half-month publication + 7 d, or a later HTTP Last-Modified) < date(t-1) 22:00 UTC; the window is the 21 "
        "settlement dates ending at the latest prefix-visible settlement date S*(t)",
        "NaN when date(t) - S*(t) > 60 days (stage staleness), fewer than 21 settlement dates exist up to S*(t), or "
        "shares_out at the window's session is not finite and positive; session 0 -> NaN",
        ["settlement_date", "security_id", "quantity", "available_at", "map_basis", "shares_out"],
        ["files are re-posted without revision history (vintage_risk on every row)",
         "rows the stage could not map (OTC and delisted symbols, collisions) count as no fail: 96-98% of fail value "
         "maps", "two CUSIPs of one line on one date (CUSIP change) are summed"],
        requires=("shares_out",)),
    "regsho_threshold_days63": _spec(
        "regsho", "regsho-threshold-days63-listing-market-v1", "count of sessions (0-63)",
        "number of the 63 sessions ending at t-2 (role calendar, extended before the role by the list dates) on which a "
        "visible Reg SHO threshold list carries the line on_list; finite only when the line's listing market (FINRA "
        "marketClassCode of its latest visible short-interest row no older than 45 days: NNM/SC -> nasdaq, NYSE/AMEX/"
        "ARCA -> nyse, BZX -> cboe_bzx, OTC -> finra_otc) has a visible list (status list or empty_list) on at least 60 "
        "of those 63 sessions.",
        "regsho-days63-t2-v1: list rows and list statuses visible by available_at (stage regsho-publication-v1) < "
        "date(t-1) 22:00 UTC; " + _SESSION_T2 + "; listing-market rows by their dissemination available_at under the "
        "same rule",
        "NaN when the listing market is unknown (no visible FINRA name row within 45 days, or a market without a list "
        "source such as IEX) or fewer than 60 of the market's 63 window lists are visible (this also enforces the stage's "
        "10-day list staleness); session 0 -> NaN",
        ["lists.market", "lists.list_date", "lists.status", "lists.available_at", "threshold.list_date",
         "threshold.market", "threshold.security_id", "threshold.on_list", "threshold.available_at",
         "finra_names.security_id", "finra_names.available_at", "finra_names.dissemination_date",
         "finra_names.market_class"],
        ["the NYSE-family lists were still landing at stage build time (51 days of 2018 only): NYSE-listed lines are NaN "
         "by the coverage rule, never a false zero; a rebuild after the stage lands them fills them unchanged",
         "lists are re-posted without revision history (vintage_risk)",
         "symbols the stage could not map are absent (OTC symbols outside the vendor file)"]),
    "sv_offexchange_share126": _spec(
        "svx", "finra-offexchange-short-over-consolidated126-v1", "ratio (decimal, [0, 1])",
        "sum over S of FINRA CNMS short_volume (every FINRA-reported trade is off-exchange: TRFs and ADF) / sum over S of "
        "the role's consolidated vendor volume; S = the sessions among the 126 ending at t-2 with a visible "
        "short_volume_ext row of the line and a present role volume > 0; NaN when |S| < 63 or the ratio is outside "
        "[0, 1] (declared plausibility, counted). The FINRA files carry facility flags, not per-venue volumes: the only "
        "off-exchange / exchange split is FINRA versus consolidated volume.",
        "finra-svx-126-t2-v1: short_volume_ext rows visible by available_at (stage sv-ext-next-day-v1: trade date + 1 "
        "day 00:00 UTC) < date(t-1) 22:00 UTC; " + _SESSION_T2 + "; role volume of session s is known at its s mark",
        "NaN when fewer than 63 sessions contribute, the first 127 role sessions (no prefix: the role volume starts "
        "with the role), or outside [0, 1]; session 0 -> NaN",
        ["security_id", "trade_date", "short_volume", "total_volume", "available_at", "volume.f64", "present.u8"],
        ["FINRA daily short volume counts market-maker short sales that hedge customer buying",
         "files as landed long after the trade date (vintage_risk)",
         "vendor volume is taken as consolidated volume; the ratio of FINRA total volume to it is reported per year in "
         "source_checks.holdings.short_volume_ext"]),
    "exch_up_365d": _spec(
        "xsw", "finra-listing-up-switch365-v1", "indicator (0/1)",
        "1 when the line's latest visible up-switch of its listing exchange (Nasdaq -> NYSE, NYSE American -> NYSE or "
        "Nasdaq -> NYSE American; Dharan and Ikenberry 1995, the Chen-Zimmermann ExchSwitch moves) has a dissemination "
        "date d with date(t) - d <= 365 days, else 0. " + XSW_RULE + ".",
        "finra-names-up-switch-asof-v1: FINRA name rows (security_master finra_names, PIT at their dissemination "
        "available_at) are usable at role session t iff available_at < date(t-1) 22:00 UTC; the line's state and its "
        "events at t come from those rows only",
        "NaN when the line's latest visible name row is more than 45 days older than date(t) (listing market unknown, "
        "as regsho_threshold_days63), or when the visible name rows (any line of the role) do not reach back 410 days "
        "(365 + 45) "
        "before date(t) (short history: an earlier move could not be seen); session 0 -> NaN",
        ["finra_names.security_id", "finra_names.available_at", "finra_names.dissemination_date",
         "finra_names.market_class"],
        ["a listing move shows at the line's first short-interest row after it (semi-monthly: up to about 15 days "
         "late); the event date is that row's dissemination date, not the listing date",
         "FINRA short interest before June 2021 is FINRA's later republication: its market class is as republished "
         "(vintage risk)",
         "a move that coincides with a new vendor security_id is not seen; a de-SPAC that keeps its vendor line and "
         "moves from Nasdaq to NYSE is an up-switch",
         "moves to or from NYSE Arca, Cboe BZX or OTC are not events (Dharan-Ikenberry use CRSP exchange codes)"]),
}
FORMULA_IDS = {k: v["formula_id"] for k, v in HOLD_FIELDS.items()}


# ---------------------------------------------------------------------------------------------------------------------
# Pure helpers (unit-tested directly)
# ---------------------------------------------------------------------------------------------------------------------

def day_of(value: dt.date) -> int:
    return (value - EPOCH).days


def date_of(day: int) -> dt.date:
    return EPOCH + dt.timedelta(days=int(day))


def quarter_end(d: dt.date) -> dt.date:
    """The calendar quarter end on or after ``d``."""
    month = ((d.month - 1) // 3) * 3 + 3
    nxt = dt.date(d.year + (month == 12), month % 12 + 1, 1)
    return nxt - dt.timedelta(days=1)


def source_part_is_sealed(part: str) -> bool:
    """True for a 13F ``parts/source=YYYYqN`` data set (the filings of calendar quarter N of YYYY) that begins on or
    after the research seal; a part name of another shape is not dated and is not refused here."""
    m = re.fullmatch(r"(\d{4})q([1-4])", part)
    return m is not None and rw.partition_is_sealed(int(m[1]), int(m[2]))


def prev_quarter_end(q: dt.date) -> dt.date:
    return quarter_end(q.replace(day=1) - dt.timedelta(days=80))


def needed_quarters(first: dt.date, last: dt.date) -> list:
    """Quarter ends P that can be fresh inside [first, last] (P >= first - 150 d, P < last) plus the quarter before
    the earliest (changes)."""
    q = quarter_end(first - dt.timedelta(days=F13_STALE_DAYS))
    out = [prev_quarter_end(q)]
    while q < last:
        out.append(q)
        q = quarter_end(q + dt.timedelta(days=1))
    return out


def prev_marks(days: np.ndarray) -> np.ndarray:
    """Per role session t: the mark of session t-1 (date 22:00 UTC, ns); session 0 sees nothing."""
    marks = days.astype(np.int64) * DAY_NS + MARK_NS
    out = np.empty_like(marks)
    out[0] = BEFORE_ALL
    out[1:] = marks[:-1]
    return out


def effective_filings(period, filer, fdate, accession, subtype, amend):
    """Boolean mask of the ``F13_EFFECTIVE_RULE`` effective filings (inputs: already filtered by the deadline)."""
    period, filer, fdate = (np.asarray(x, dtype=np.int64) for x in (period, filer, fdate))
    n = len(period)
    if not n:
        return np.zeros(0, dtype=bool)
    subtype, amend = np.asarray(subtype, dtype=object), np.asarray(amend, dtype=object)
    newh = amend == "NEW HOLDINGS"
    base = ~newh & (np.isin(subtype, F13_BASE_FORMS) | (amend == "RESTATEMENT"))
    acc_rank = np.unique(np.asarray(accession, dtype=str), return_inverse=True)[1]
    order = np.lexsort((acc_rank, fdate, filer, period))
    p, f = period[order], filer[order]
    group = np.cumsum(np.r_[True, (p[1:] != p[:-1]) | (f[1:] != f[:-1])]) - 1
    pos = np.arange(n)
    last_base = np.full(int(group[-1]) + 1, -1, dtype=np.int64)
    b = base[order]
    np.maximum.at(last_base, group[b], pos[b])
    lb = last_base[group]
    eff_sorted = (pos == lb) | (newh[order] & (lb >= 0) & (pos > lb))
    eff = np.zeros(n, dtype=bool)
    eff[order] = eff_sorted
    return eff


def group_median(keys: np.ndarray, values: np.ndarray):
    """(unique keys, median of values per key)."""
    uk, inv = np.unique(keys, return_inverse=True)
    if not len(uk):
        return uk, np.zeros(0)
    order = np.lexsort((values, inv))
    v = values[order]
    counts = np.bincount(inv, minlength=len(uk))
    start = np.r_[0, np.cumsum(counts)[:-1]]
    med = (v[start + (counts - 1) // 2] + v[start + counts // 2]) / 2
    return uk, med


def quarter_holdings(filer, sid, shares, value, outlier_factor=F13_PRICE_OUTLIER):
    """Per-quarter reduction of the screened holdings rows (``F13_ROWS_RULE``): the price-outlier screen, filers,
    (filer, security) pairs and per-security best-ideas sums and holder counts."""
    filer, sid = np.asarray(filer, dtype=np.int64), np.asarray(sid, dtype=np.int64)
    shares, value = np.asarray(shares, dtype=np.float64), np.asarray(value, dtype=np.float64)
    mapped = sid >= 0
    priced = mapped & (value > 0)
    price = np.where(priced, value / np.where(priced, shares, 1.0), np.nan)
    us, med = group_median(sid[priced], price[priced])
    ref = np.full(len(sid), np.nan)
    ref[priced] = med[np.searchsorted(us, sid[priced])]
    with np.errstate(invalid="ignore", divide="ignore"):
        ratio = price / ref
    outlier = priced & ((ratio > outlier_factor) | (ratio < 1.0 / outlier_factor))
    keep = ~outlier
    filers, finv = np.unique(filer[keep], return_inverse=True)
    total_m = np.bincount(finv, weights=value[keep], minlength=len(filers))
    km = keep & mapped
    fi = np.searchsorted(filers, filer[km])
    sids, si = np.unique(sid[km], return_inverse=True)
    nf = max(len(filers), 1)
    key = si.astype(np.int64) * nf + fi
    pairs, pinv = np.unique(key, return_inverse=True)
    v_pair = np.bincount(pinv, weights=value[km], minlength=len(pairs))
    pair_s, pair_f = pairs // nf, pairs % nf
    inst_value = np.bincount(pair_s, weights=v_pair, minlength=len(sids))
    total = float(inst_value.sum())
    mw = inst_value / total if total > 0 else np.zeros(len(sids))
    tm = total_m[pair_f]
    with np.errstate(invalid="ignore", divide="ignore"):
        w = np.where(tm > 0, v_pair / np.where(tm > 0, tm, 1.0), np.nan)
    tilt = np.where(np.isfinite(w), np.maximum(0.0, w - mw[pair_s]), 0.0)
    return {"sids": sids, "best": np.bincount(pair_s, weights=tilt, minlength=len(sids)),
            "holders": np.bincount(pair_s, minlength=len(sids)), "filers": filers,
            "pair_filer": filers[pair_f], "pair_sid": sids[pair_s],
            "rows": int(len(sid)), "rows_mapped": int(np.count_nonzero(mapped)),
            "rows_price_outlier": int(np.count_nonzero(outlier)), "filers_zero_total": int(np.count_nonzero(total_m <= 0))}


def breadth_change(cur: dict, prev: dict | None) -> np.ndarray:
    """Chen-Hong-Stein breadth change per ``cur['sids']`` (NaN without a previous quarter or a previous row)."""
    out = np.full(len(cur["sids"]), np.nan)
    if prev is None:
        return out
    both = np.intersect1d(cur["filers"], prev["filers"])
    if not len(both):
        return out
    c_now = np.isin(cur["pair_filer"], both)
    c_old = np.isin(prev["pair_filer"], both)
    now = np.bincount(np.searchsorted(cur["sids"], cur["pair_sid"][c_now]), minlength=len(cur["sids"]))
    in_old = np.isin(cur["sids"], prev["sids"])
    old_sids, old_counts = np.unique(prev["pair_sid"][c_old], return_counts=True)
    old = np.zeros(len(cur["sids"]))
    at = np.searchsorted(old_sids, cur["sids"])
    hit = (at < len(old_sids)) & (old_sids[np.minimum(at, max(len(old_sids) - 1, 0))] == cur["sids"]) if len(old_sids) \
        else np.zeros(len(cur["sids"]), dtype=bool)
    old[hit] = old_counts[at[hit]]
    out[in_old] = (now[in_old] - old[in_old]) / len(both)
    return out


def anchor_quarters(pdays: np.ndarray, vis_ns: np.ndarray, has: np.ndarray, day: int, prev_mark: int,
                    stale_days=F13_STALE_DAYS) -> np.ndarray:
    """Per column: the latest quarter index q with V(q) < prev_mark, day - P(q) <= stale_days and a row (``has[q]``);
    -1 when none."""
    cand = np.flatnonzero((vis_ns < prev_mark) & (day - pdays <= stale_days) & (pdays <= day))
    anchor = np.full(has.shape[1], -1, dtype=np.int64)
    for q in cand:  # ascending: a later quarter with a row wins
        anchor = np.where(has[q], q, anchor)
    return anchor


def window_sums(cum: np.ndarray, end: int, width: int) -> np.ndarray:
    """Sum of rows end-width+1..end of the matrix whose inclusive prefix sums are ``cum`` (cum[0] = 0 row)."""
    return cum[end + 1] - cum[end + 1 - width]


# ---------------------------------------------------------------------------------------------------------------------
# Stage pins
# ---------------------------------------------------------------------------------------------------------------------

def _sha(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def _identity(path: Path):
    st = Path(path).stat()
    return (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns)


def _sha_file(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    return h.hexdigest()


def pin_stage(directory: Path, expected_sha256: str, key: str):
    blob = (Path(directory) / "manifest.json").read_bytes()
    digest = _sha(blob)
    flag = "--" + key.replace("_", "-")
    if digest != str(expected_sha256).lower():
        raise ValueError(f"{key} stage manifest SHA-256 does not match {flag}-sha256")
    m = json.loads(blob)
    spec = STAGES[key]
    if m.get("schema") != spec["schema"] or m.get("status") != "complete":
        raise ValueError(f"{key} stage manifest is not a complete {spec['schema']} manifest")
    for k in ("clock_rule", "staleness_days", "rule"):
        if k in spec and m.get(k) != spec[k]:
            raise ValueError(f"{key} stage {k} {m.get(k)!r} differs from the declared {spec[k]!r}")
    if not isinstance(m.get("files"), dict):
        raise ValueError(f"{key} stage manifest lists no files")
    return m, {"path": str((Path(directory) / "manifest.json").resolve()), "bytes": len(blob), "sha256": digest}


class Stage:
    """A pinned stage directory: every file is read once as bytes and checked against the stage manifest."""

    def __init__(self, key: str, directory: Path, expected_sha256: str):
        self.key, self.dir = key, Path(directory)
        self.manifest, self.manifest_source = pin_stage(self.dir, expected_sha256, key)
        self.read = []

    def blob(self, rel: str) -> bytes:
        entry = self.manifest["files"].get(rel)
        if not isinstance(entry, dict):
            raise ValueError(f"{self.key}: stage manifest does not list {rel}")
        blob = (self.dir / rel).read_bytes()
        if len(blob) != int(entry.get("bytes", -1)) or _sha(blob) != entry.get("sha256"):
            raise ValueError(f"{self.key}: {rel} does not match its stage manifest entry")
        src = {"path": str((self.dir / rel).resolve()), "bytes": len(blob), "sha256": entry["sha256"]}
        if src not in self.read:
            self.read.append(src)
        return blob

    def verified_path(self, rel: str):
        """A large file hashed by streaming (not held in memory) and pinned by its file identity: the caller parses
        it from the path and calls ``unchanged`` afterwards (the identity must not move while it is read)."""
        entry = self.manifest["files"].get(rel)
        if not isinstance(entry, dict):
            raise ValueError(f"{self.key}: stage manifest does not list {rel}")
        path = self.dir / rel
        before = _identity(path)
        if before[2] != int(entry.get("bytes", -1)) or _sha_file(path) != entry.get("sha256") or _identity(path) != before:
            raise ValueError(f"{self.key}: {rel} does not match its stage manifest entry")
        src = {"path": str(path.resolve()), "bytes": before[2], "sha256": entry["sha256"]}
        if src not in self.read:
            self.read.append(src)
        return path, before

    def unchanged(self, path: Path, before):
        if _identity(path) != before:
            raise ValueError(f"{self.key}: {path} changed while it was read")

    def table(self, rel: str, columns) -> pa.Table:
        return pq.read_table(pa.BufferReader(self.blob(rel)), columns=list(columns))

    def years(self, pattern: str, lo: int, hi: int) -> list:
        """Manifest-listed ``pattern.format(year)`` files for lo..hi that exist in the stage. A year that begins on or
        after the research seal holds only sealed rows: its file is never listed, so it is never opened."""
        return [pattern.format(y) for y in range(lo, hi + 1)
                if pattern.format(y) in self.manifest["files"] and not rw.partition_is_sealed(y)]

    def sources(self) -> list:
        return [self.manifest_source] + sorted(self.read, key=lambda x: x["path"])

    def check(self) -> dict:
        m = self.manifest
        return {"manifest_sha256": self.manifest_source["sha256"], "schema": m.get("schema"),
                "clock_rule": m.get("clock_rule"), "staleness_days": m.get("staleness_days"), "rule": m.get("rule"),
                "code_git_head": (m.get("code") or {}).get("git_head"), "files_read": len(self.read)}


def _col(table, name):
    col = table.column(name)
    return col.combine_chunks() if isinstance(col, pa.ChunkedArray) else col


def _instants(col) -> np.ndarray:
    if not pa.types.is_timestamp(col.type):
        raise ValueError(f"expected a timestamp column, got {col.type}")
    ns = pc.cast(pc.cast(col, pa.timestamp("ns", tz=col.type.tz)), pa.int64())
    return pc.fill_null(ns, NEVER).to_numpy(zero_copy_only=False).astype(np.int64)


def _days(col) -> np.ndarray:
    if not pa.types.is_date32(col.type):
        raise ValueError(f"expected a date32 column, got {col.type}")
    return pc.fill_null(pc.cast(col, pa.int32()), -(1 << 30)).to_numpy(zero_copy_only=False).astype(np.int64)


def _ids(col) -> np.ndarray:
    return pc.fill_null(pc.cast(col, pa.int64()), -1).to_numpy(zero_copy_only=False).astype(np.int64)


def _f64(col) -> np.ndarray:
    return pc.fill_null(pc.cast(col, pa.float64()), np.nan).to_numpy(zero_copy_only=False).astype(np.float64)


def _text(col) -> np.ndarray:
    return np.asarray(pc.fill_null(pc.cast(col, pa.string()), "").to_pylist(), dtype=object)


def _codes(col, values) -> np.ndarray:
    """Index of each string in ``values`` (-1: null or not listed), computed in Arrow (no Python strings)."""
    return pc.fill_null(pc.index_in(pc.cast(col, pa.string()), value_set=pa.array(list(values), pa.string())),
                        -1).to_numpy(zero_copy_only=False).astype(np.int64)


def _release():
    import gc
    gc.collect()
    pa.default_memory_pool().release_unused()


# ---------------------------------------------------------------------------------------------------------------------
# Field builders: each returns ({name: row matrix producer}, stats) by streaming rows into FieldWriters
# ---------------------------------------------------------------------------------------------------------------------

class Ctx:
    """What every builder needs: the builder namespace, the role, the output directory, the budget, marks."""

    def __init__(self, ns, role, output: Path, budget, shares_out):
        self.ns, self.role, self.output, self.budget = ns, role, Path(output), budget
        self.days = role.days.astype(np.int64)
        self.pm = prev_marks(self.days)
        self.so = shares_out           # (nd, n) f64 or None
        self.member = role.member != 0

    def session_on_or_before(self, day: int) -> int:
        return int(np.searchsorted(self.days, day, side="right")) - 1


def _reasons(names, keys):
    return {x: dict.fromkeys(keys, 0) for x in names}


def build_13f(ctx: Ctx, names, stage: Stage):
    role, ns, n = ctx.role, ctx.ns, ctx.role.n
    first, last = date_of(ctx.days[0]), date_of(ctx.days[-1])
    quarters = needed_quarters(first, last)
    qdays = np.array([day_of(q) for q in quarters], dtype=np.int64)
    nq = len(quarters)
    f = stage.table("filings.parquet", ["accession", "filer_cik", "period_q", "filing_date", "submission_type",
                                        "amendment_type", "available_at", "deadline", "source_period"])
    f = f.filter(pa.array(np.isin(_days(_col(f, "period_q")), qdays)))   # the needed quarters only
    period, fdate, deadline = _days(_col(f, "period_q")), _days(_col(f, "filing_date")), _days(_col(f, "deadline"))
    avail = _instants(_col(f, "available_at"))
    subtype, amend = _text(_col(f, "submission_type")), _text(_col(f, "amendment_type"))
    in_q = fdate <= deadline
    st = {"quarters": [q.isoformat() for q in quarters], "effective_rule": F13_EFFECTIVE_RULE, "rows_rule": F13_ROWS_RULE,
          "filings_in_quarters_by_deadline": int(np.count_nonzero(in_q)),
          "filings_after_deadline_ignored": int(np.count_nonzero(~in_q))}
    qidx = np.searchsorted(qdays, period)
    has_filing = np.zeros(nq, dtype=bool)
    has_filing[qidx[in_q]] = True
    vq = np.full(nq, BEFORE_ALL, dtype=np.int64)
    np.maximum.at(vq, qidx[in_q], avail[in_q])      # V(P): every filing of P by its deadline, notices included
    vis = np.where(has_filing, vq, NEVER)
    sealed = vis >= SEAL_NS
    vis[sealed] = NEVER
    st["quarters_without_filings"] = [quarters[i].isoformat() for i in np.flatnonzero(~has_filing)]
    st["quarters_sealed"] = int(np.count_nonzero(sealed & has_filing))
    st["quarter_visible_at"] = {quarters[i].isoformat(): (None if vis[i] == NEVER else
                                                          str(np.datetime64(int(vis[i]), "ns")) + "Z") for i in range(nq)}
    sel = in_q & np.isin(subtype, F13_FORMS)
    st["hr_a_without_amendment_type_ignored"] = int(np.count_nonzero(sel & (subtype == "13F-HR/A") & (amend == "")))
    filer_all = _ids(pc.cast(pc.fill_null(_col(f, "filer_cik"), "-1"), pa.int64()))
    acc_all = _text(_col(f, "accession"))
    src_all = _text(_col(f, "source_period"))
    idx = np.flatnonzero(sel)
    eff = effective_filings(period[idx], filer_all[idx], fdate[idx], acc_all[idx], subtype[idx], amend[idx])
    eidx = idx[eff]
    st["effective_filings"] = int(len(eidx))
    del f
    checks = stage.table("filing_checks.parquet", ["accession", "unit_factor"])
    uf_acc = _col(checks, "accession")
    uf_val = _f64(_col(checks, "unit_factor"))
    del checks
    cmap = stage.table("cusip_map_pit.parquet", ["period_q", "cusip", "security_id"])
    cm_q, cm_cusip, cm_sid = _days(_col(cmap, "period_q")), _col(cmap, "cusip"), _ids(_col(cmap, "security_id"))
    del cmap
    agg = stage.table("agg_asof45.parquet", ["period_of_report", "security_id", "inst_shares", "n_holders",
                                             "available_at", "version"])
    a_q, a_sid = _days(_col(agg, "period_of_report")), _ids(_col(agg, "security_id"))
    a_sh, a_nh, a_av = _f64(_col(agg, "inst_shares")), _f64(_col(agg, "n_holders")), _instants(_col(agg, "available_at"))
    if set(_text(_col(agg, "version")).tolist()) - {"asof45"}:
        raise ValueError("thirteenf: agg_asof45.parquet carries a version other than asof45")
    del agg
    ctx.budget.check("13f-inputs")
    pos, on = role.columns_of(a_sid)
    aq = np.searchsorted(qdays, a_q)
    use = on & (aq < nq) & (qdays[np.minimum(aq, nq - 1)] == a_q)
    late = use & (a_av > vis[np.minimum(aq, nq - 1)])
    st["agg_rows_on_role"] = int(np.count_nonzero(use))
    st["agg_rows_available_after_quarter_clock_dropped"] = int(np.count_nonzero(late))
    use &= ~late
    has = np.zeros((nq, n), dtype=bool)
    inst_sh = np.full((nq, n), np.nan)
    n_hold = np.full((nq, n), np.nan)
    has[aq[use], pos[use]] = True
    inst_sh[aq[use], pos[use]] = a_sh[use]
    n_hold[aq[use], pos[use]] = a_nh[use]
    has &= ~(vis[:, None] == NEVER)
    best = np.full((nq, n), np.nan)
    breadth = np.full((nq, n), np.nan)
    need_holdings = any(x in names for x in ("inst_best_ideas", "inst_breadth_chg"))
    per_q = {}
    if need_holdings:
        prev = None
        agree = [0, 0]
        for q in range(nq):
            if not has_filing[q]:
                prev = None
                continue
            mine = eidx[np.searchsorted(qdays, period[eidx]) == q]
            order = np.argsort(acc_all[mine].astype(str))
            acc_q = pa.array(acc_all[mine][order].tolist(), pa.string())
            filer_q = filer_all[mine][order]
            ufp = np.ones(len(mine))
            hi = pc.fill_null(pc.index_in(acc_q, value_set=uf_acc), -1).to_numpy(zero_copy_only=False)
            ufp[hi >= 0] = uf_val[hi[hi >= 0]]
            ufp = np.where(np.isfinite(ufp) & (ufp > 0), ufp, 1.0)
            cm = cm_q == qdays[q]
            keys = cm_cusip.filter(pa.array(cm))
            ksid = cm_sid[cm]
            rows = {"filer": [], "sid": [], "shares": [], "value": []}
            for part in sorted(set(src_all[mine].tolist())):
                if source_part_is_sealed(part):  # filed on or after the seal: never opened
                    st["holdings_parts_not_read_sealed"] = st.get("holdings_parts_not_read_sealed", 0) + 1
                    continue
                path, ident = stage.verified_path(f"parts/source={part}/holdings.parquet")
                pf = pq.ParquetFile(path)
                for batch in pf.iter_batches(batch_size=65_536, columns=["accession", "cusip", "shares", "sshprnamt_type",
                                                                         "put_call", "value_usd"], use_threads=False):
                    ai = pc.fill_null(pc.index_in(batch.column("accession"), value_set=acc_q), -1).to_numpy(
                        zero_copy_only=False)
                    ok = ai >= 0
                    ok &= pc.fill_null(pc.equal(batch.column("sshprnamt_type"), "SH"), False).to_numpy(zero_copy_only=False)
                    pcall = batch.column("put_call")
                    ok &= pc.fill_null(pc.or_kleene(pc.is_null(pcall), pc.equal(pcall, "")), True).to_numpy(
                        zero_copy_only=False)
                    sh = _f64(batch.column("shares"))
                    val = _f64(batch.column("value_usd"))
                    ok &= np.isfinite(sh) & (sh > 0) & np.isfinite(val) & (val >= 0)
                    if not ok.any():
                        continue
                    k = np.flatnonzero(ok)
                    ci = pc.fill_null(pc.index_in(batch.column("cusip").take(pa.array(k)), value_set=keys), -1).to_numpy(
                        zero_copy_only=False)
                    rows["filer"].append(filer_q[ai[k]])
                    rows["sid"].append(np.where(ci >= 0, ksid[np.maximum(ci, 0)], -1))
                    rows["shares"].append(sh[k])
                    rows["value"].append(val[k] * ufp[ai[k]])
                pf.close()
                stage.unchanged(path, ident)
                del pf
                _release()
                ctx.budget.check("13f-holdings")
            cat = {k: np.concatenate(v) if v else np.zeros(0) for k, v in rows.items()}
            del rows
            cur = quarter_holdings(cat["filer"], cat["sid"], cat["shares"], cat["value"])
            del cat
            chg = breadth_change(cur, prev if q > 0 and prev is not None and prev["q"] == q - 1 else None)
            p2, o2 = role.columns_of(cur["sids"])
            best[q, p2[o2]] = cur["best"][o2]
            breadth[q, p2[o2]] = chg[o2]
            both = o2 & has[q, p2]
            agree[0] += int(np.count_nonzero(both))
            agree[1] += int(np.count_nonzero(n_hold[q, p2[both]] == cur["holders"][both]))
            per_q[quarters[q].isoformat()] = {k: cur[k] for k in ("rows", "rows_mapped", "rows_price_outlier",
                                                                   "filers_zero_total")}
            per_q[quarters[q].isoformat()].update(filers=int(len(cur["filers"])), securities=int(len(cur["sids"])))
            prev = {k: cur[k] for k in ("filers", "sids", "pair_filer", "pair_sid")}
            prev["q"] = q
            del cur
            ctx.budget.report("13f-quarter", quarter=quarters[q].isoformat(), **per_q[quarters[q].isoformat()])
        st["holdings_per_quarter"] = per_q
        st["holders_equal_agg_asof45_role_cells"] = {"cells": agree[0], "equal": agree[1]}
    # quarter-end ownership share (P's share basis) and its change
    io = np.full((nq, n), np.nan)
    io_implausible = 0
    if any(x in names for x in ("inst_own_share", "inst_own_chg_q")):
        for q in range(nq):
            t = ctx.session_on_or_before(int(qdays[q]))
            if t < 0:
                continue
            so = ctx.so[t]
            ok = np.isfinite(so) & (so > 0)
            with np.errstate(invalid="ignore", divide="ignore"):
                v = np.where(ok, inst_sh[q] / np.where(ok, so, 1.0), np.nan)
            bad = np.isfinite(v) & (v > F13_IO_MAX)
            io_implausible += int(np.count_nonzero(bad & has[q]))
            io[q] = np.where(bad | (v < 0), np.nan, v)
    io_chg = np.full((nq, n), np.nan)
    for q in range(1, nq):
        if quarters[q - 1] == prev_quarter_end(quarters[q]):
            io_chg[q] = io[q] - io[q - 1]
    values = {"inst_own_share": io, "inst_breadth_chg": breadth, "inst_own_chg_q": io_chg, "inst_best_ideas": best,
              "inst_n_holders": n_hold}
    reasons = _reasons(names, ("no_fresh_visible_quarter_row", "anchor_value_nan", "forward_filled_finite"))
    writers = {x: ns["FieldWriter"](ctx.output, x, role) for x in names}
    try:
        for t in range(role.n_dates):
            day = int(ctx.days[t])
            anchor = anchor_quarters(qdays, vis, has, day, int(ctx.pm[t]))
            ok = anchor >= 0
            a = np.maximum(anchor, 0)
            latest = np.flatnonzero((vis < ctx.pm[t]) & (qdays <= day))
            filled = ok & (anchor != (latest[-1] if len(latest) else -1))
            member = ctx.member[t]
            for x in names:
                row = np.where(ok, values[x][a, np.arange(n)], np.nan)
                writers[x].write(row)
                r = reasons[x]
                fin = np.isfinite(row)
                r["no_fresh_visible_quarter_row"] += int(np.count_nonzero(member & ~ok))
                r["anchor_value_nan"] += int(np.count_nonzero(member & ok & ~fin))
                r["forward_filled_finite"] += int(np.count_nonzero(member & fin & filled))
            if t % 256 == 0:
                ctx.budget.check("13f-write")
    except BaseException:
        for w in writers.values():
            w.f.close()
        raise
    for w in writers.values():
        w.close()
    if "inst_own_share" in names:
        reasons["inst_own_share"]["quarter_cells_above_declared_max_2"] = io_implausible
    return writers, st, {x: {"nan_reasons_member_cells": reasons[x]} for x in names}


def build_ftd(ctx: Ctx, names, stage: Stage):
    role, ns, n = ctx.role, ctx.ns, ctx.role.n
    lo = date_of(ctx.days[0] - FTD_STALE_DAYS - 60).year
    hi = date_of(ctx.days[-1]).year
    files = stage.years("year={}/ftd.parquet", lo, hi)
    if not files:
        raise ValueError("ftd: no year file of the stage overlaps the role")
    sdays, sav, rows = [], [], {"k": [], "col": [], "qty": []}
    st = {"files": files, "rows": 0, "rows_sealed": 0, "rows_on_role": 0, "rows_collision_excluded": 0,
          "rows_unmapped": 0}
    for rel in files:
        t = stage.table(rel, ["settlement_date", "security_id", "quantity", "available_at", "map_basis"])
        d, sid, qty, av = _days(_col(t, "settlement_date")), _ids(_col(t, "security_id")), _f64(_col(t, "quantity")), \
            _instants(_col(t, "available_at"))
        collision = _codes(_col(t, "map_basis"), ("collision",)) == 0
        del t
        st["rows"] += len(d)
        sealed = av >= SEAL_NS
        st["rows_sealed"] += int(np.count_nonzero(sealed))
        d, sid, qty, av, collision = d[~sealed], sid[~sealed], qty[~sealed], av[~sealed], collision[~sealed]
        ud, inv = np.unique(d, return_inverse=True)       # this file's settlement dates and their latest clock
        uav = np.full(len(ud), BEFORE_ALL, dtype=np.int64)
        np.maximum.at(uav, inv, av)
        sdays.append(ud)
        sav.append(uav)
        st["rows_collision_excluded"] += int(np.count_nonzero(collision))
        st["rows_unmapped"] += int(np.count_nonzero(sid <= 0))
        pos, on = role.columns_of(np.maximum(sid, 0))
        keep = on & (sid > 0) & ~collision & np.isfinite(qty)
        st["rows_on_role"] += int(np.count_nonzero(keep))
        rows["k"].append(d[keep])
        rows["col"].append(pos[keep])
        rows["qty"].append(qty[keep])
        del d, sid, qty, av, collision, pos, on, keep
        _release()
        ctx.budget.check("ftd-read")
    d_all, av_all = np.concatenate(sdays), np.concatenate(sav)
    cal = np.unique(d_all)
    vis_d = np.full(len(cal), BEFORE_ALL, dtype=np.int64)
    np.maximum.at(vis_d, np.searchsorted(cal, d_all), av_all)
    del d_all, av_all, sdays, sav
    prefix_vis = np.maximum.accumulate(vis_d)
    k = np.searchsorted(cal, np.concatenate(rows["k"]))
    col = np.concatenate(rows["col"])
    qty = np.concatenate(rows["qty"])
    del rows
    ctx.budget.admit((len(cal) + 1) * n * 8 + (16 << 20), "ftd-matrix")
    cum = np.zeros((len(cal) + 1, n))
    np.add.at(cum, (k + 1, col), qty)
    dup = np.unique(k.astype(np.int64) * n + col, return_counts=True)[1]
    st["role_rows_summed_duplicates"] = int(np.count_nonzero(dup > 1))
    np.cumsum(cum, axis=0, out=cum)
    gaps = np.diff(cal)
    st.update({"settlement_calendar": {"dates": int(len(cal)), "first": date_of(cal[0]).isoformat(),
                                       "last": date_of(cal[-1]).isoformat(),
                                       "gaps_over_5_days": int(np.count_nonzero(gaps > 5))}})
    reasons = _reasons(names, ("no_visible_window", "stale", "shares_out_nan", "short_history"))
    x = names[0]
    w = ns["FieldWriter"](ctx.output, x, role)
    try:
        for t in range(role.n_dates):
            kstar = int(np.searchsorted(prefix_vis, ctx.pm[t], side="left")) - 1
            member = ctx.member[t]
            r = reasons[x]
            row = np.full(n, np.nan)
            if kstar < 0:
                r["no_visible_window"] += int(np.count_nonzero(member))
            elif int(ctx.days[t]) - int(cal[kstar]) > FTD_STALE_DAYS:
                r["stale"] += int(np.count_nonzero(member))
            elif kstar + 1 < FTD_WINDOW:
                r["short_history"] += int(np.count_nonzero(member))
            else:
                ts = ctx.session_on_or_before(int(cal[kstar]))
                so = ctx.so[ts] if ts >= 0 else np.full(n, np.nan)
                ok = np.isfinite(so) & (so > 0)
                s = window_sums(cum, kstar, FTD_WINDOW)
                row = np.where(ok, s / np.where(ok, so, 1.0), np.nan)
                r["shares_out_nan"] += int(np.count_nonzero(member & ~ok))
            w.write(row)
            if t % 256 == 0:
                ctx.budget.check("ftd-write")
    except BaseException:
        w.f.close()
        raise
    w.close()
    return {x: w}, st, {x: {"nan_reasons_member_cells": reasons[x]}}


def build_regsho(ctx: Ctx, names, stage: Stage, names_stage: Stage):
    role, ns, n, nd = ctx.role, ctx.ns, ctx.role.n, ctx.role.n_dates
    lists = stage.table("lists.parquet", ["market", "list_date", "status", "available_at"])
    l_m, l_d, l_s, l_av = _text(_col(lists, "market")), _days(_col(lists, "list_date")), _text(_col(lists, "status")), \
        _instants(_col(lists, "available_at"))
    del lists
    ok_list = np.isin(l_s, REGSHO_LIST_STATUSES) & (l_av < SEAL_NS)
    prefix = np.unique(l_d[ok_list & (l_d < ctx.days[0])])[-(REGSHO_WINDOW + 2):]
    cal = np.concatenate((prefix, ctx.days))
    e0 = len(prefix)
    nm = len(REGSHO_MARKETS)
    A = np.full((nm, len(cal)), NEVER, dtype=np.int64)
    m_idx = np.array([REGSHO_MARKETS.index(m) if m in REGSHO_MARKETS else -1 for m in l_m.tolist()], dtype=np.int64)
    kk = np.searchsorted(cal, l_d)
    on_cal = (kk < len(cal)) & (cal[np.minimum(kk, len(cal) - 1)] == l_d)
    use = ok_list & on_cal & (m_idx >= 0)
    np.minimum.at(A, (m_idx[use], kk[use]), l_av[use])
    st = {"lists_used": int(np.count_nonzero(use)), "lists_outside_the_window_calendar": int(np.count_nonzero(ok_list & ~on_cal)),
          "lists_unknown_market": int(np.count_nonzero(ok_list & (m_idx < 0))),
          "lists_by_market_on_calendar": {m: int(np.count_nonzero(A[i] < NEVER)) for i, m in enumerate(REGSHO_MARKETS)},
          "prefix_sessions": int(e0)}
    lo, hi = date_of(cal[0]).year, date_of(cal[-1]).year
    files = stage.years("year={}/threshold.parquet", lo, hi)
    ON = np.zeros((len(cal), n), dtype=np.int8)       # market index + 1 of a visible on_list row
    late_rows = rows_used = 0
    for rel in files:
        t = stage.table(rel, ["list_date", "market", "security_id", "on_list", "available_at"])
        d, sid = _days(_col(t, "list_date")), _ids(_col(t, "security_id"))
        mi = _codes(_col(t, "market"), REGSHO_MARKETS)
        onl = pc.fill_null(_col(t, "on_list"), False).to_numpy(zero_copy_only=False).astype(bool)
        av = _instants(_col(t, "available_at"))
        del t
        k = np.searchsorted(cal, d)
        oc = (k < len(cal)) & (cal[np.minimum(k, len(cal) - 1)] == d)
        pos, on = role.columns_of(np.maximum(sid, 0))
        keep = onl & oc & on & (sid > 0) & (mi >= 0) & (av < SEAL_NS)
        kc = np.minimum(k, len(cal) - 1)
        lav = A[np.maximum(mi, 0), kc]
        late = keep & (av > lav)
        late_rows += int(np.count_nonzero(late))
        A_late = keep & late
        np.maximum.at(A, (mi[A_late], kc[A_late]), av[A_late])  # a row later than its list delays the list
        ON[kc[keep], pos[keep]] = (mi[keep] + 1).astype(np.int8)
        rows_used += int(np.count_nonzero(keep))
        ctx.budget.check("regsho-read")
    st.update(threshold_files=files, on_list_rows_on_role=rows_used, rows_later_than_their_list=late_rows)
    fn = names_stage.table("finra_names.parquet", ["security_id", "available_at", "dissemination_date", "market_class"])
    f_sid, f_av, f_dd = _ids(_col(fn, "security_id")), _instants(_col(fn, "available_at")), _days(_col(fn, "dissemination_date"))
    classes = list(REGSHO_MARKET_OF_CLASS)
    f_cls = _codes(_col(fn, "market_class"), classes)
    del fn
    class_market = np.array([REGSHO_MARKETS.index(REGSHO_MARKET_OF_CLASS[c]) for c in classes] + [-1], dtype=np.int64)
    fpos, fon = role.columns_of(np.maximum(f_sid, 0))
    keep = fon & (f_sid > 0) & (f_av < SEAL_NS)
    order = np.flatnonzero(keep)[np.argsort(f_av[keep], kind="stable")]
    f_av, f_dd, f_col = f_av[order], f_dd[order], fpos[order]
    f_mkt = class_market[f_cls[order]]                     # an unlisted class (-1) indexes the trailing -1
    del f_sid, f_cls, fpos, fon, keep
    _release()
    st["finra_name_rows_on_role"] = int(len(order))
    latest = np.full(n, -1, dtype=np.int64)
    p = 0
    reasons = _reasons(names, ("market_unknown", "market_lists_incomplete", "short_calendar"))
    x = names[0]
    w = ns["FieldWriter"](ctx.output, x, role)
    counts_by_market = {m: 0 for m in REGSHO_MARKETS}
    try:
        for t in range(nd):
            pm = int(ctx.pm[t])
            p2 = int(np.searchsorted(f_av, pm, side="left"))
            if p2 > p:
                np.maximum.at(latest, f_col[p:p2], np.arange(p, p2, dtype=np.int64))
                p = p2
            has = latest >= 0
            li = np.maximum(latest, 0)
            fresh = has & (int(ctx.days[t]) - f_dd[li] <= REGSHO_NAME_MAX_AGE_DAYS) if len(f_dd) else has
            mkt = np.where(fresh, f_mkt[li] if len(f_mkt) else -1, -1)
            member = ctx.member[t]
            r = reasons[x]
            e = e0 + t - 2
            row = np.full(n, np.nan)
            if e - REGSHO_WINDOW + 1 < 0:
                r["short_calendar"] += int(np.count_nonzero(member))
            else:
                win = slice(e - REGSHO_WINDOW + 1, e + 1)
                vis = A[:, win] < pm                              # (markets, 63)
                covered = vis.sum(axis=1)
                onw = ON[win].astype(np.int64)                    # (63, n)
                seen = (onw > 0) & np.take_along_axis(vis.T, np.maximum(onw - 1, 0), axis=1)
                cnt = seen.sum(axis=0).astype(np.float64)
                okm = mkt >= 0
                enough = okm & (covered[np.maximum(mkt, 0)] >= REGSHO_MIN_LISTS)
                row = np.where(enough, cnt, np.nan)
                r["market_unknown"] += int(np.count_nonzero(member & ~okm))
                r["market_lists_incomplete"] += int(np.count_nonzero(member & okm & ~enough))
                for i, m in enumerate(REGSHO_MARKETS):
                    counts_by_market[m] += int(np.count_nonzero(member & enough & (mkt == i)))
            w.write(row)
            if t % 256 == 0:
                ctx.budget.check("regsho-write")
    except BaseException:
        w.f.close()
        raise
    w.close()
    st["finite_member_cells_by_listing_market"] = counts_by_market
    return {x: w}, st, {x: {"nan_reasons_member_cells": reasons[x]}}


def build_svx(ctx: Ctx, names, stage: Stage):
    role, ns, n, nd = ctx.role, ctx.ns, ctx.role.n, ctx.role.n_dates
    vol = _role_matrix(role, "volume.f64", "<f8")
    present = _role_matrix(role, "present.u8", "u1") != 0
    role_sources = []
    for name in ("volume.f64", "present.u8"):
        e = role.manifest["files"][name]
        role_sources.append({"path": str((role.dir / name).resolve()), "bytes": e["bytes"], "sha256": e["sha256"]})
    files = stage.years("year={}/short_volume_ext.parquet", date_of(ctx.days[0]).year, date_of(ctx.days[-1]).year)
    if not files:
        raise ValueError("short_volume_ext: no year file of the stage overlaps the role")
    ctx.budget.admit(nd * n * 8 + (16 << 20), "svx-matrix")
    short = np.zeros((nd, n))
    has = np.zeros((nd, n), dtype=bool)
    avail = np.full(nd, BEFORE_ALL, dtype=np.int64)
    finra_total, vendor_total = {}, {}
    st = {"files": files, "rows": 0, "rows_on_role_sessions": 0, "rows_off_role_calendar": 0, "duplicates_summed": 0}
    for rel in files:
        t = stage.table(rel, ["security_id", "trade_date", "short_volume", "total_volume", "available_at"])
        sid, d = _ids(_col(t, "security_id")), _days(_col(t, "trade_date"))
        sv, tv, av = _f64(_col(t, "short_volume")), _f64(_col(t, "total_volume")), _instants(_col(t, "available_at"))
        del t
        st["rows"] += len(sid)
        k = np.searchsorted(ctx.days, d)
        oc = (k < nd) & (ctx.days[np.minimum(k, nd - 1)] == d)
        pos, on = role.columns_of(np.maximum(sid, 0))
        keep = oc & on & (sid > 0) & np.isfinite(sv) & (sv >= 0) & (av < SEAL_NS)
        st["rows_off_role_calendar"] += int(np.count_nonzero(on & (sid > 0) & ~oc))
        st["rows_on_role_sessions"] += int(np.count_nonzero(keep))
        kk, pp = k[keep], pos[keep]
        st["duplicates_summed"] += int(np.count_nonzero(has[kk, pp]))
        np.add.at(short, (kk, pp), sv[keep])
        has[kk, pp] = True
        np.maximum.at(avail, kk, av[keep])
        both = keep & np.isfinite(tv)
        vv = vol[k[both], pos[both]]
        okv = present[k[both], pos[both]] & np.isfinite(vv) & (vv > 0) & ctx.member[k[both], pos[both]]
        yrs = role.years[k[both][okv]]
        for y in np.unique(yrs):
            sel = yrs == y
            finra_total[str(y)] = finra_total.get(str(y), 0.0) + float(tv[both][okv][sel].sum())
            vendor_total[str(y)] = vendor_total.get(str(y), 0.0) + float(vv[okv][sel].sum())
        del sid, d, sv, tv, av, k, oc, pos, on, keep, both, vv, okv
        _release()
        ctx.budget.check("svx-read")
    st["finra_total_over_vendor_volume_member_cells"] = {
        y: round(finra_total[y] / vendor_total[y], 6) for y in sorted(finra_total) if vendor_total[y] > 0}
    contrib_ok = present & np.isfinite(vol) & (vol > 0) & has
    reasons = _reasons(names, ("short_history", "too_few_sessions", "outside_domain"))
    x = names[0]
    w = ns["FieldWriter"](ctx.output, x, role)
    rs, rv, rn = np.zeros(n), np.zeros(n), np.zeros(n, dtype=np.int64)
    late_sessions, pending = 0, []
    entered = np.zeros(nd, dtype=bool)
    try:
        for t in range(nd):
            rem = t - 2 - SVX_WINDOW
            if t >= 2:
                pending.append(t - 2)
                late_sessions += int(avail[t - 2] >= ctx.pm[t])
            waiting = []
            for s in pending:  # a session enters the running window once its rows are visible, if still inside
                if s <= rem:
                    continue
                if avail[s] < ctx.pm[t]:
                    c = contrib_ok[s]
                    rs += np.where(c, short[s], 0.0)
                    rv += np.where(c, vol[s], 0.0)
                    rn += c
                    entered[s] = True
                else:
                    waiting.append(s)
            pending = waiting
            if rem >= 0 and entered[rem]:
                c = contrib_ok[rem]
                rs -= np.where(c, short[rem], 0.0)
                rv -= np.where(c, vol[rem], 0.0)
                rn -= c
            member = ctx.member[t]
            r = reasons[x]
            if t - 2 - SVX_WINDOW + 1 < 0:
                row = np.full(n, np.nan)
                r["short_history"] += int(np.count_nonzero(member))
            else:
                enough = (rn >= SVX_MIN_SESSIONS) & (rv > 0)
                with np.errstate(invalid="ignore", divide="ignore"):
                    v = np.where(enough, rs / np.where(enough, rv, 1.0), np.nan)
                inside = np.isfinite(v) & (v >= SVX_DOMAIN[0]) & (v <= SVX_DOMAIN[1])
                row = np.where(inside, v, np.nan)
                r["too_few_sessions"] += int(np.count_nonzero(member & ~enough))
                r["outside_domain"] += int(np.count_nonzero(member & enough & ~inside))
            w.write(row)
            if t % 256 == 0:
                ctx.budget.check("svx-write")
    except BaseException:
        w.f.close()
        raise
    w.close()
    st["sessions_not_yet_visible_at_window_entry"] = late_sessions
    return {x: w}, st, {x: {"nan_reasons_member_cells": reasons[x]}}, role_sources


def xsw_events(col: np.ndarray, dd: np.ndarray, ven: np.ndarray) -> tuple:
    """``XSW_RULE`` over rows already in visibility order: (up-switch flag per row, the line's current dissemination
    day and venue after each row, outcome counts). ``ven`` indexes XSW_VENUES, -1 = other."""
    up = {(XSW_VENUES.index(a), XSW_VENUES.index(b)) for a, b in XSW_UP}
    n = len(col)
    event = np.zeros(n, dtype=bool)
    state_dd, state_ven = np.empty(n, dtype=np.int64), np.empty(n, dtype=np.int64)
    counts = dict.fromkeys(("first_rows", "advancing_rows", "up_switches", "advancing_beyond_gap", "conflicts",
                            "out_of_order_rows"), 0)
    cur: dict = {}
    for i, (c, d, v) in enumerate(zip(col.tolist(), dd.tolist(), ven.tolist())):
        s = cur.get(c)
        if s is None:
            s = cur[c] = [d, v]
            counts["first_rows"] += 1
        elif d > s[0]:
            counts["advancing_rows"] += 1
            if d - s[0] > XSW_MAX_GAP_DAYS:
                counts["advancing_beyond_gap"] += 1
            elif (s[1], v) in up:
                event[i] = True
                counts["up_switches"] += 1
            s[0], s[1] = d, v
        elif d == s[0]:
            if v != s[1] and s[1] != -1:
                s[1] = -1
                counts["conflicts"] += 1
        else:
            counts["out_of_order_rows"] += 1
        state_dd[i], state_ven[i] = s[0], s[1]
    return event, state_dd, state_ven, counts


def build_xsw(ctx: Ctx, names, stage: Stage):
    role, ns, n, nd = ctx.role, ctx.ns, ctx.role.n, ctx.role.n_dates
    fn = stage.table("finra_names.parquet", ["security_id", "available_at", "dissemination_date", "market_class"])
    f_sid, f_av, f_dd = _ids(_col(fn, "security_id")), _instants(_col(fn, "available_at")), \
        _days(_col(fn, "dissemination_date"))
    classes = list(XSW_VENUE_OF_CLASS)
    venue_of = np.array([XSW_VENUES.index(XSW_VENUE_OF_CLASS[c]) for c in classes] + [-1], dtype=np.int64)
    f_ven = venue_of[_codes(_col(fn, "market_class"), classes)]          # an unlisted class (-1) -> other
    del fn
    pos, on = role.columns_of(np.maximum(f_sid, 0))
    sealed = f_av >= SEAL_NS
    keep = on & (f_sid > 0) & ~sealed
    st = {"rows": int(len(f_sid)), "rows_sealed": int(np.count_nonzero(sealed)),
          "rows_on_role": int(np.count_nonzero(keep))}
    idx = np.flatnonzero(keep)
    order = idx[np.lexsort((idx, f_ven[idx], f_dd[idx], f_av[idx]))]   # visibility order, deterministic ties
    av, dd, col = f_av[order], f_dd[order], pos[order]
    event, state_dd, state_ven, counts = xsw_events(col, dd, f_ven[order])
    del f_sid, f_av, f_dd, f_ven, pos, on, sealed, keep, idx, order, state_ven
    _release()
    st.update(counts)
    ctx.budget.check("xsw-read")
    reasons = _reasons(names, ("short_history", "market_unknown"))
    x = names[0]
    latest = np.full(n, -1, dtype=np.int64)
    last_up = np.full(n, XSW_NO_DAY, dtype=np.int64)
    first_dd = None
    flagged = p = 0
    w = ns["FieldWriter"](ctx.output, x, role)
    try:
        for t in range(nd):
            p2 = int(np.searchsorted(av, ctx.pm[t], side="left"))        # rows with available_at < mark(t-1)
            if p2 > p:
                np.maximum.at(latest, col[p:p2], np.arange(p, p2, dtype=np.int64))
                e = event[p:p2]
                np.maximum.at(last_up, col[p:p2][e], dd[p:p2][e])
                low = int(dd[p:p2].min())
                first_dd = low if first_dd is None else min(first_dd, low)
                p = p2
            day = int(ctx.days[t])
            member = ctx.member[t]
            r = reasons[x]
            row = np.full(n, np.nan)
            if first_dd is None or day - XSW_WINDOW_DAYS - XSW_MAX_GAP_DAYS < first_dd:
                r["short_history"] += int(np.count_nonzero(member))
            else:
                li = np.maximum(latest, 0)
                fresh = (latest >= 0) & (day - state_dd[li] <= XSW_MAX_GAP_DAYS)
                row = np.where(fresh, (day - last_up <= XSW_WINDOW_DAYS).astype(np.float64), np.nan)
                r["market_unknown"] += int(np.count_nonzero(member & ~fresh))
                flagged += int(np.count_nonzero(member & (row == 1.0)))
            w.write(row)
            if t % 256 == 0:
                ctx.budget.check("xsw-write")
    except BaseException:
        w.f.close()
        raise
    w.close()
    st["flagged_member_cells"] = flagged
    st["lines_with_an_up_switch"] = int(np.count_nonzero(last_up > XSW_NO_DAY))
    return {x: w}, st, {x: {"nan_reasons_member_cells": reasons[x]}}


def _role_matrix(role, name: str, dtype: str) -> np.ndarray:
    entry = role.manifest["files"].get(name)
    blob = (role.dir / name).read_bytes()
    if entry is None or len(blob) != entry["bytes"] or _sha(blob) != entry["sha256"]:
        raise ValueError(f"role {name} bytes do not match the role manifest")
    return np.frombuffer(blob, dtype=dtype).reshape(role.n_dates, role.n)


# ---------------------------------------------------------------------------------------------------------------------
# Orchestration inside the builder's manifest publication
# ---------------------------------------------------------------------------------------------------------------------

def requested(fields) -> list:
    return [x for x in HOLD_FIELDS if x in fields]


# -- --reuse interface (v8 C-3) --------------------------------------------------------------------------------------
def producer_group(name: str) -> str:
    return HOLD_FIELDS[name]["kind"]


def field_spec(name: str) -> dict:
    return HOLD_FIELDS[name]


def seal_pin() -> str:
    """The research seal a holdings payload is computed under (review N-1). Every kind drops the stage rows available
    on or after it before it takes a date's or a session's visibility clock (the latest available_at of the kept rows),
    so a seal move can change a payload of a role that ends before both seals."""
    return SEAL.isoformat()


def reuse_inputs(name: str, inputs: dict) -> dict:
    """This run's input pins of a holdings field: its stage manifest pins (the --reuse source check; a manifest pins
    every file) and the research seal (``seal_pin``, review N-1)."""
    pins = {k: str(inputs.get(k + "_sha256") or "").lower() for k in KIND_STAGES[HOLD_FIELDS[name]["kind"]]}
    pins["seal"] = seal_pin()
    return pins


def entry_inputs(entry: dict) -> dict:
    """The same pins as a manifest entry of a holdings field records them (an entry written before review N-1 records
    no seal: None, so its field is recomputed once)."""
    pins = entry.get("stage_manifest_sha256")
    out = dict(pins) if isinstance(pins, dict) else {}
    out["seal"] = entry.get("seal_date")
    return out


def validate(fields, selected_other, inputs: dict):
    mine = requested(fields)
    for x in mine:
        missing = [d for d in HOLD_FIELDS[x].get("requires", []) if d not in selected_other]
        if missing:
            raise ValueError(f"--fields: {x} requires {', '.join(missing)} in the same run (it reads this run's payload)")
        for key in KIND_STAGES[HOLD_FIELDS[x]["kind"]]:
            if inputs.get(key) is None or not inputs.get(key + "_sha256"):
                flag = "--" + key.replace("_", "-")
                raise ValueError(f"--fields: {x} needs {flag} and {flag}-sha256")
    return mine


def open_stages(names, inputs: dict) -> dict:
    """Pin every stage the requested fields read (manifest SHA, schema, declared rules) before anything is computed."""
    stages = {}
    for x in names:
        for key in KIND_STAGES[HOLD_FIELDS[x]["kind"]]:
            if key not in stages:
                stages[key] = Stage(key, inputs[key], inputs[key + "_sha256"])
    return stages


def build_all(ns, names, role_dir: Path, role_sha256: str, output: Path, manifest: dict, budget, stages: dict,
              reused=None, prior_checks=None):
    """Compute ``names`` into ``output`` and extend ``manifest`` (entries, files, source_checks.holdings).

    ``reused`` ({name: (entry, files pin)}, from --reuse) are already in ``output`` and are appended as they are; a
    kind whose fields are all reused carries its check from ``prior_checks`` (the prior source_checks.holdings)."""
    _release()  # whatever the builder's own groups left in the pools
    reused, prior_checks = reused or {}, prior_checks or {}
    todo = [x for x in names if x not in reused]
    role = ns["Role"](role_dir, role_sha256)
    so = None
    if any("shares_out" in HOLD_FIELDS[x].get("requires", []) for x in todo):
        pin = manifest["files"].get("shares_out.f64")
        path = Path(output) / "shares_out.f64"
        before = _identity(path)
        if pin is None or before[2] != pin["bytes"] or _sha_file(path) != pin["sha256"] or _identity(path) != before:
            raise ValueError("holdings: this run's shares_out.f64 does not match the manifest being published")
        so = np.memmap(path, dtype="<f8", mode="r", shape=(role.n_dates, role.n))  # rows read on demand
        so_source = {"path": str(path.resolve()), "bytes": pin["bytes"], "sha256": pin["sha256"]}
    ctx = Ctx(ns, role, output, budget, so)
    writers, extras, sources, checks = {}, {}, {}, {}
    kinds = {}
    for x in todo:
        kinds.setdefault(HOLD_FIELDS[x]["kind"], []).append(x)
    if "13f" in kinds:
        w, st, ex = build_13f(ctx, kinds["13f"], stages["thirteenf"])
        writers.update(w)
        extras.update(ex)
        checks["thirteenf"] = st
        for x in kinds["13f"]:
            sources[x] = stages["thirteenf"].sources()
        _release()
        budget.report("holdings-13f-complete", fields=len(kinds["13f"]))
    if "ftd" in kinds:
        w, st, ex = build_ftd(ctx, kinds["ftd"], stages["ftd"])
        writers.update(w)
        extras.update(ex)
        checks["ftd"] = st
        for x in kinds["ftd"]:
            sources[x] = stages["ftd"].sources()
        _release()
        budget.report("holdings-ftd-complete")
    if "regsho" in kinds:
        w, st, ex = build_regsho(ctx, kinds["regsho"], stages["regsho_threshold"], stages["security_master"])
        writers.update(w)
        extras.update(ex)
        checks["regsho_threshold"] = st
        for x in kinds["regsho"]:
            sources[x] = stages["regsho_threshold"].sources() + stages["security_master"].sources()
        _release()
        budget.report("holdings-regsho-complete")
    if "svx" in kinds:
        w, st, ex, role_src = build_svx(ctx, kinds["svx"], stages["short_volume_ext"])
        writers.update(w)
        extras.update(ex)
        checks["short_volume_ext"] = st
        for x in kinds["svx"]:
            sources[x] = stages["short_volume_ext"].sources() + role_src
        _release()
        budget.report("holdings-svx-complete")
    if "xsw" in kinds:
        w, st, ex = build_xsw(ctx, kinds["xsw"], stages["security_master"])
        writers.update(w)
        extras.update(ex)
        checks["listing_switch"] = st
        for x in kinds["xsw"]:
            sources[x] = stages["security_master"].sources()
        _release()
        budget.report("holdings-xsw-complete")
    for x in todo:
        if "shares_out" in HOLD_FIELDS[x].get("requires", []):
            sources[x] = sources[x] + [so_source]
    for kind in dict.fromkeys(HOLD_FIELDS[x]["kind"] for x in names):
        key = KIND_CHECK_KEY[kind]
        if kind not in kinds and key in prior_checks:  # every field of the kind reused: its prior check stands
            checks[key] = prior_checks[key]
    checks = {k: checks[k] for k in KIND_CHECK_KEY.values() if k in checks}  # kind order, as a full compute
    ns["Role"](role_dir, role_sha256)  # the axes did not change underneath
    code = ns["module_code_identity"](sys.modules[__name__])  # = code_identity(this file), v8 C-3
    for x in names:
        if x in reused:
            entry, pin = reused[x]
            if x in manifest["files"] or entry["file"] in manifest["files"] or any(e["name"] == x
                                                                                  for e in manifest["fields"]):
                raise ValueError(f"{x}: already in the manifest")
            manifest["files"][entry["file"]] = pin
            manifest["fields"].append(entry)
            continue
        spec, wr = HOLD_FIELDS[x], writers[x]
        size = wr.path.stat().st_size
        if size != role.n_dates * role.n * 8:
            raise ValueError(f"{x}: output size mismatch")
        digest, quantiles = ns["digest_and_quantiles"](wr.path, role, wr.vcount, budget)
        cov = wr.coverage()
        cov["member_finite_quantiles"] = quantiles
        entry = {"name": x, "file": wr.path.name, "dtype": "<f8", "layout": "date-major",
                 "shape": [role.n_dates, role.n], "units": spec["units"], "clock": spec["clock"],
                 "staleness": spec["staleness"], "source_columns": spec["source_columns"], "sources": sources[x],
                 "caveats": spec["caveats"], "coverage": cov, "sha256": digest, "point_in_time": True,
                 "non_pit_aspects": [], "definition": spec["definition"], "formula_id": spec["formula_id"],
                 "visibility_rule": VISIBILITY_RULE, "producer": {"module": Path(__file__).name, **code},
                 "stage_manifest_sha256": {k: stages[k].manifest_source["sha256"]
                                           for k in KIND_STAGES[spec["kind"]]},
                 "seal_date": seal_pin()}  # review N-1: the --reuse input pin (entry_inputs)
        if "requires" in spec:
            entry["depends_on"] = spec["requires"]
        entry.update(extras.get(x, {}))
        if x in manifest["files"] or any(e["name"] == x for e in manifest["fields"]):
            raise ValueError(f"{x}: already in the manifest")
        manifest["files"][wr.path.name] = {"bytes": size, "sha256": digest}
        manifest["fields"].append(entry)
    manifest["source_checks"]["holdings"] = {
        "code": {"module": Path(__file__).name, **code}, "visibility_rule": VISIBILITY_RULE,
        "stages": {k: s.check() for k, s in stages.items()}, **checks}
    del ctx, so  # release the shares_out map before the manifest is published
    budget.report("holdings-complete", fields=len(names), peak_rss_mib=budget.peak >> 20)


# ---------------------------------------------------------------------------------------------------------------------
# The module object the builder binds (a late FIELD_MODULES module; lane A1 of P9 replaced the run/main wrap)
# ---------------------------------------------------------------------------------------------------------------------

def bind(host_namespace: dict) -> "HoldingsFieldModule":
    """The builder's hook (``FIELD_MODULES.append(_holdings.bind(globals()))``). ``HOLD_FIELDS`` stay out of the builder's
    ``ALL_FIELDS``: the builder orders a late module's fields after every other field, as the manifest always had them."""
    return HoldingsFieldModule(host_namespace)


class HoldingsFieldModule:
    """``LATE``: the builder computes and digests every other field, assembles its manifest, then calls ``finish``,
    which writes this module's payloads into the same exclusive output directory and appends their entries (registry
    order, after the builder's) and ``source_checks.holdings`` before the manifest is published (``build_all``)."""
    GROUP, FIELDS, OPTIONS, LATE = GROUP, HOLD_FIELDS, STAGE_KWARGS, True

    def __init__(self, host_namespace: dict):
        self.ns = host_namespace
        self._stages: dict = {}

    @staticmethod
    def add_arguments(parser):
        g = parser.add_argument_group("holdings fields (research_fields_holdings.py: " + ",".join(HOLD_FIELDS) + ")")
        for key in STAGES:
            flag = "--" + key.replace("_", "-")
            g.add_argument(flag, dest=key, type=Path, help=f"the atx-db alpha-panel {key}/ stage directory")
            g.add_argument(flag + "-sha256", dest=key + "_sha256", help="SHA-256 of that stage's manifest.json")

    def check(self, selected, options: dict):
        """Before any output: requirements and stage inputs (``validate``), then every stage the requested fields read
        is opened and pinned (``open_stages``) for ``finish``."""
        self._stages = {}
        mine = requested(selected)
        if mine:
            validate(selected, [x for x in selected if x not in HOLD_FIELDS], options)
            self._stages = open_stages(mine, options)

    def merge_reuse(self, block: dict, order: list, names: list, reused: dict, reasons: dict, prior: dict) -> None:
        """The builder's --reuse block extended with this module's decisions, one merge per kind (group
        ``holdings.<check key>``). The prior check of a fully reused kind goes to ``source_checks.holdings`` through
        ``finish`` (``build_all``), never to the builder's top-level source checks."""
        checks = prior.get(GROUP) if isinstance(prior.get(GROUP), dict) else {}
        for kind, key in KIND_CHECK_KEY.items():
            mine = [x for x in names if HOLD_FIELDS[x]["kind"] == kind]
            if mine:
                self.ns["merge_module_reuse"](block, order, mine, reused, reasons, group=f"{GROUP}.{key}",
                                              prior_check=checks.get(key))
                block.get("_source_checks", {}).pop(f"{GROUP}.{key}", None)

    def finish(self, names, role_dir: Path, role_sha256: str, output: Path, manifest: dict, budget, reused: dict,
               prior: dict) -> None:
        """Compute ``names`` (those not in ``reused``) and extend ``manifest`` before it is published."""
        checks = prior.get(GROUP) if isinstance(prior.get(GROUP), dict) else {}
        build_all(self.ns, names, Path(role_dir), role_sha256, Path(output), manifest, budget, self._stages, reused,
                  checks)
