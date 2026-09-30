"""Point-in-time research fields aligned EXACTLY to an existing recent-research role payload.

Reads a published ``atx.recent-research-role/v1`` directory (sessions.i64, ids.u64, member.u8)
and writes one date-major little-endian f64 file per field, shape role dates x role ids, NaN
where not visible or absent, plus a publish-last ``atx.research-role-fields/v1`` manifest.
Nothing is re-projected: the role's own session labels and securityID axis are the only axes.

Sources (all read-only; exact bytes hashed, pinned against their producers' receipts):
* FINRA short interest as-of CSVs (``security_id,available_at,value``): the join replicates
  ``atx-impl/src/asof_field.cpp`` exactly: strict available_at < date(session), latest row wins
  (a visible NaN stays NaN), age > 45 calendar days -> NaN.
* TickerHistory3 vendor rows (must be the role's own source file, same SHA-256): a row dated d
  is the end-of-day mark known at d 22:00 UTC, i.e. the same clock as the role's close. Same-date
  only, no fill. ``shares_out`` instead follows the house A8 rule (vendor share runs start at the
  filing cover date): the share count of the line's last row dated <= date(session) - 90 days,
  restated through cumulReturnFactor to the session's share basis with every factor-break-v1
  re-anchoring step divided out (the vendor factor is not chained across 2021-01-04), and NaN
  outside the declared domain ``SHARES_OUT_DOMAIN``.
* research-lake spine_monthly: value of the latest monthly formation session STRICTLY before
  date(session); line_types: static (non point-in-time) line classification.
* the role payload itself (``mkt_ret``): equal-weight mean of guarded adjusted close-to-close
  returns of the prior session's decision members, broadcast to every present cell of the session.
* issuer-level fields (opt-in, ``ISSUER_FIELDS``; library v4): a pinned identity bridge (T19,
  ``prepare_identity_bridge.py``: dated securityID -> CIK links, P/J, available_at) and a pinned
  fundamental events artifact (T20, ``build_fundamental_events.py``: one snapshot row per filing event
  and a SIC event table). Line i at session d links to a CIK through a bridge row whose [start, end_incl]
  contains d (bridge rows are already point in time: available_at <= start 22:00 UTC, asserted and
  counted per row; a violating row is also gated by available_at <= d 22:00 UTC). Fundamental items and industry groups (``grp_sic2``,
  ``grp_ff12``, ``grp_ff49``) read the CIK's latest row with clock < the mark of session d-L, L =
  ``--fund-lag-sessions`` (declared 1), with 200/400/550-day staleness, on primary (P) lines only;
  ``me_company`` sums shares_out x raw_close over every role line (P and J) of the issuer. The input
  contracts (``atx.identity-bridge/v1``, ``atx.fundamental-events/v1``) are bound in ``BRIDGE_ADAPTER``,
  ``EVENTS_ADAPTER`` and ``SIC_ADAPTER``. ``--sic-events DIR --sic-events-sha256 PIN`` (platform v7 U2) takes the
  grp_* fields' SIC table from the atx-db fundamentals stage instead (``SIC_STAGE_MAPPING``: same clock, lag,
  staleness and guards; the table role universe linked-operating-v3 restricts on), so role and fields share one SIC
  source; every other field is untouched, and without the option every output byte is unchanged.
* FINRA daily short sale volume (opt-in ``sv_ratio126``; library v6.1): the CNMSshvolYYYYMMDD.txt.gz files of
  ``--finra-short-volume``, streamed one day at a time and each verified against the downloader's receipt
  (manifest.csv), mapped to role lines with the short-interest producer's PIT symbol map over the role's own
  TickerHistory3 ``ticker_tk`` (``SV_MAP_RULE``); the 126-session ratio uses the files dated d-126..d-1 only.

Every manifest field entry carries a machine-readable ``point_in_time`` flag (see
``POINT_IN_TIME_DEFINITION``); a false flag names its reason and the non-PIT aspect (values or
presence). The default field list is the point-in-time fields only; the others (``is_common``,
``mktcap_lagged``, ``size_grp``) are produced only when named in ``--fields``. Implied volatility
outside the declared domain ``IV_DOMAIN`` becomes NaN (never clamped) and is counted per field.

Everything available on or after the research seal (``research_window.py`` ``SEAL_DATE``) is excluded; the role
itself must end before it.
Row groups of the vendor file mix all dates: only needed columns are decoded, rows are filtered to
the role ids/dates before any statistic. No warehouse access. Outputs are exclusive and
deterministic (no wall-clock value in any output byte).

Field reuse (``--reuse PRIOR_FIELDS_DIR [--reuse-sha256 PIN] [--reuse-hardlink]``, platform v7 L2): a requested
field is copied from a prior fields directory instead of recomputed iff (``REUSE_RULE``) the prior manifest is a
complete manifest bound to this very role (manifest, sessions, ids and member SHA-256), the field's prior entry has
the same field id and the same formula id (``formula_id``: SHA-256 of the spec-level definition -- units, clock with
the declared lag, staleness, source columns, definition, point-in-time flags, declared domain -- plus the field's
``FORMULA_REVISION``), the same producing code (``producer_fingerprints``: the AST of the field group's builder
functions and of every module-level definition they reach, docstrings dropped, compared with the code that produced
the prior payload -- this builder when its LF SHA-256 matches, else its recorded git blob; unrecoverable code is never
reused; grp_* fields also need the same SIC mapping table), the same inputs (every prior source path lies under this
run's source argument for the field's group and still hashes to the recorded SHA-256; a source without a file SHA-256
is never reused, except sv_ratio126's CNMS directory, whose read files re-hash to its list SHA-256 by
``REUSE_DIRECTORY_RULE``) and every field it depends on is reused too. Copied payload bytes are hashed and must equal the prior
manifest's pin. Reused entries are the prior entries verbatim plus ``reused_from``, which names the code that
produced the payload (never this builder unless it is the same code); the manifest gains a ``reuse`` block. Without
``--reuse`` nothing in the output changes. Field modules (research_fields_sec.py, research_fields_holdings.py; v8 C-3)
reuse by ``REUSE_MODULE_RULE``: the module's PRODUCERS closure plus the builder code it reads through its host handle,
its stage manifest pins as the source check, its formula keys; their computed entries record the module's code
identity as ``producer``.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import gzip
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import time

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.csv as pacsv
import pyarrow.parquet as pq

import code_fingerprint  # noqa: E402  (same directory: the AST closure fingerprints --reuse keys on)
import research_window as rw  # same directory: the research window (TRAIN and the seal)

SCHEMA = "atx.research-role-fields/v1"
ROLE_SCHEMA = "atx.recent-research-role/v1"
DAY_NS = 86_400_000_000_000
EPOCH = dt.date(1970, 1, 1)
SEAL = rw.SEAL  # first sealed date (research_window.py)
FINRA_MAX_STALE_DAYS = 45
FINRA_HEADER = b"security_id,available_at,value"
FINRA_REPUBLICATION_SETTLEMENT_BEFORE = dt.date(2021, 6, 1)
SHARES_LAG_DAYS = 90            # atx_db.research.spine.SHARES_LAG_DAYS (A8)
SHARES_MAX_AGE_DAYS = 400       # atx_db.research.spine.SHARES_MAX_AGE_DAYS
SHARES_UNIT = 1000.0            # A9: the parquet format's `shares` means thousands
THOUSANDS_ROW_CEILING = 100_000_000  # atx_db.migrations.bodies_0327 (1e11 plausible max / 1000)
LINE_PREFIX = "TBLTICKERHISTORY-"
ELIGIBLE_SECURITY_TYPES = ("common", "common_unverified")  # atx_db.research.spine
SIZE_CODES = {"micro": 0.0, "small": 1.0, "large": 2.0, "mega": 3.0}
EARN_CODES = {"N": 0, "-1": 0, "0": 1, "1": 1}  # -1 presumes a known future date: same as N
# Declared by the root controller 2026-09-27 BEFORE any IV measurement (T6 fix round 1): the
# plausible domain of annualized decimal ATM implied volatility, both bounds inclusive.
IV_DOMAIN = (0.02, 5.0)
IV_DOMAIN_RULE = ("vendor value v kept iff float32(0.02) <= v <= float32(5.0) (float32 comparison: the vendor's own "
                  "precision, so a stored 0.02 is in domain); any other non-null value -> NaN, never clamped. below_min "
                  "includes <= 0 and -inf, above_max includes +inf. Counts are role cells after duplicate-key quarantine. "
                  "Domain declared by the root controller 2026-09-27 before any IV measurement")
# Declared by the root controller for T10 (swap-fin-v1 borrow tiers) and applied here (T6 fix round 2):
# restated shares outstanding outside this range are implausible, both bounds inclusive.
SHARES_OUT_DOMAIN = (1e5, 5e10)
# Root ruling (T6 fix round 3; thresholds amended in fix round 4, declared before any v3 screen or NAV scoring):
# vendor share counts ~1000x too small also sit inside the domain. A shares_out cell is invalid when (a) the
# median of the name's daily share volume over the trailing 21 role sessions ending at the session, restated to
# the session's share basis, exceeds 3.0 x shares_out, or (b) si_shares visible at the session exceeds 5.0 x
# shares_out. Fix round 3 declared 1.0 / 1.5, which also removed genuine leveraged, inverse and volatility ETPs
# (1.1-2.8x shares traded a day) and high-short-interest ETFs such as XRT; 3.0 / 5.0 keep them.
SHARES_TURNOVER_WINDOW = 21
SHARES_TURNOVER_MIN_OBS = 11     # the median needs a majority of the window present (declared with the rule)
SHARES_TURNOVER_MAX = 3.0
SHARES_SI_RATIO_MAX = 5.0
SHARES_UNITS_RULE = (
    "applied after the factor-break correction, the C-81 rule and the [1e5, 5e10] domain, in this order: "
    "(a) turnover: u_d = volume_d / f_d over the name's present role sessions d in the 21 role sessions ending at t "
    "(role volume.f64 is raw-share-volume, each day's own share units; f = close/raw is the role's chained factor, "
    "so u is one share basis and median(u) x f_t is the median daily volume in session t's share basis); with at "
    "least 11 present days, median x f_t > 3.0 x shares_out -> NaN. (b) short interest: si_shares visible at t (the "
    "same run's si_shares field: strict available_at < session, 45-day staleness) / shares_out > 5.0 -> NaN. Only "
    "shares_out is set to NaN. Every input is dated <= t. Root ruling, T6 fix round 3; thresholds 3.0 / 5.0 "
    "amended by root in fix round 4 (were 1.0 / 1.5)")
# Rule factor-break-v1, ported from atx-impl/tools/repair_role_factor_breaks.py (T12) with the same
# parameters and classification: the TickerHistory3 2026-09-20 cumulReturnFactor is not chained across
# 2021-01-04 (atx-db VA1 / ruling C-35), so a factor ratio spanning that session carries a step that no
# corporate action made. shares_out divides every repaired step out of its restatement ratio.
FB_RULE = "factor-break-v1"
FB_CELL_STEP = 0.01
FB_CELL_EXCESS = 0.01
FB_MASS_MIN_CELLS = 50
FB_NOISE = 1e-9
FB_SPLIT_RATIO = 1.25
FB_MAX_GAP_DAYS = 10
FB_REPAIRED, FB_KEPT_GAP, FB_KEPT_FOLLOW, FB_KEPT_DIST = 1, 2, 3, 4
FB_ACTIONS = {FB_REPAIRED: "repaired", FB_KEPT_GAP: "kept_gap", FB_KEPT_FOLLOW: "kept_split_follow",
              FB_KEPT_DIST: "kept_distribution"}
FB_PARAMETERS = {"cell_step_ln": FB_CELL_STEP, "cell_excess_ln": FB_CELL_EXCESS, "mass_min_cells": FB_MASS_MIN_CELLS,
                 "noise_ln": FB_NOISE, "split_ratio": FB_SPLIT_RATIO, "max_gap_calendar_days": FB_MAX_GAP_DAYS}
FB_STATEMENT = (
    "observation = a vendor row of a role id on the extended calendar with a unique (date, id) key, finite positive "
    "close (raw), finite volume >= 0 and finite positive cumulReturnFactor f (the role's present contract). step = "
    "consecutive observations p<t of a line <= max_gap_calendar_days apart; s=ln(f_t/f_p), r=ln(raw_t/raw_p), a=r+s. "
    "jump cell: step ending at t with |s|>cell_step_ln and |a|>|r|+cell_excess_ln; mass session: >= mass_min_cells "
    "jump cells. On a mass session b every step with p<b<=t and |s|>noise_ln is kept_gap (gap > max_gap_calendar_days), "
    "kept_split_follow (s<0 and r>=max(|s|/2, ln split_ratio), or s>0 and -r>=max(s/2, ln split_ratio)), "
    "kept_distribution (0<s<ln split_ratio), else repaired with k=f_t/f_p. Every decision reads rows <= t only.")
SPINE_MAX_FORMATION_AGE_DAYS = 35  # consecutive month-end sessions are <= 34 days apart
QUANTILES = (("p0.1", 0.001), ("p1", 0.01), ("p50", 0.5), ("p99", 0.99), ("p99.9", 0.999))
POINT_IN_TIME_DEFINITION = (
    "point_in_time is true when both a field's cell values and which of its cells are NaN use only information "
    "available by the session's decision (every finite cell of every field is known by the session-date 22:00 UTC "
    "mark, before the 23:00 UTC decision). A false flag carries point_in_time_reason and non_pit_aspects "
    "(values and/or presence). Revision vintage of the sources (FINRA republication, unproven vendor vintage) is "
    "reported separately (historical_vintage_verified, vintage_safe_from, caveats) and does not set this flag. "
    "Consumers that must avoid look-ahead refuse point_in_time false fields unless explicitly allowed.")
SPINE_PRESENCE_NOT_PIT = (
    "values are point in time (formation strictly before the session, A8 90-day lagged shares), but WHICH lines have "
    "a value is the research spine universe: an eligible type taken from the 2026-09-18 Nasdaq Trader directory "
    "snapshot (listed lines) or whole-history vendor earnings evidence (delisted lines), admitted from first_earn_date "
    "= the first bar with nEarnCnt_504d > 0, a count of FUTURE earnings events up to 504 sessions ahead "
    "(atx_db.research.spine classify_lines / stage_spine). NaN versus finite therefore leaks survival and "
    "future-earnings information")

DEFAULT_FINRA = Path("C:/atx/data/finra_short_interest")
DEFAULT_TICKERHISTORY = Path("C:/Users/natha/Downloads/TickerHistory3.parquet")
DEFAULT_LAKE = Path("C:/atx/atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23")

TH_TYPES = {"tradingDate": pa.date32(), "securityID": pa.int64(), "shares": pa.int64(),
            "earnFlag": pa.string(), "cumulReturnFactor": pa.float64(), "close": pa.float32(), "volume": pa.float64(),
            "atmCenI_21d": pa.float32(), "atmCenI_63d": pa.float32(), "atmCenI_126d": pa.float32()}
TH_CLOCK = "vendor-eod-row-date==session-date;known-at-session+22h-mark;same-date-only-v1"

FIELDS = {
    "si_shares": {
        "group": "finra", "source_file": "si_shares.csv", "source_columns": ["value"], "point_in_time": True,
        "units": "shares short (FINRA consolidated currentShortPositionQuantity)",
        "clock": "finra-asof:latest-row-with-available_at<date(session)-strict;available_at=official-dissemination-date;replicates-atx-impl-build_asof_column",
        "staleness": "age=date(session)-available_at calendar days; age>45 -> NaN; before first visible row -> NaN; visible NaN stays NaN (no skip-back)",
        "caveats": ["settlements before 2021-06 come from FINRA's later consolidated republication, not the bytes the exchanges disseminated (vintage risk; see vintage_risk counts)",
                    "securityID mapping by ORATS ticker_tk on the last trading day <= settlement (producer mapping_report.json)"]},
    "si_dtc": {
        "group": "finra", "source_file": "si_dtc.csv", "source_columns": ["value"], "point_in_time": True,
        "units": "days to cover (FINRA daysToCoverQuantity; FINRA floors the ratio at 1.00)",
        "clock": "finra-asof:latest-row-with-available_at<date(session)-strict;available_at=official-dissemination-date;replicates-atx-impl-build_asof_column",
        "staleness": "age=date(session)-available_at calendar days; age>45 -> NaN; before first visible row -> NaN; visible NaN stays NaN (no skip-back)",
        "caveats": ["FINRA reports days-to-cover floored at 1.00 (about 40% of producer rows equal 1); si_shares / volume is the unfloored alternative",
                    "settlements before 2021-06 come from FINRA's later consolidated republication (vintage risk)"]},
    "iv_atm_21d": {
        "group": "th", "column": "atmCenI_21d", "source_columns": ["atmCenI_21d"], "point_in_time": True, "domain": IV_DOMAIN,
        "units": "annualized ATM implied volatility, decimal (0.30 = 30%), 21-session constant maturity",
        "clock": TH_CLOCK, "staleness": "same-date vendor row only; null or NaN -> NaN; outside the declared domain [0.02, 5.0] (incl. <= 0 and +-inf) -> NaN, counted in plausibility",
        "caveats": ["vendor clean IV: evidence suggests the earnings effect is removed using the vendor earnings calendar (nEarnCnt_*, forward-looking by construction); calendar vintage unproven",
                    "often null (about half of all vendor rows; about 6.5% of high-volume rows in a sample)", "no vintage proof",
                    "vendor garbage rows exist (v1 member maxima 69.3 and 1.19e16): values outside the declared domain are NaN, in-domain vendor errors cannot be detected"]},
    "iv_atm_63d": {
        "group": "th", "column": "atmCenI_63d", "source_columns": ["atmCenI_63d"], "point_in_time": True, "domain": IV_DOMAIN,
        "units": "annualized ATM implied volatility, decimal, 63-session constant maturity",
        "clock": TH_CLOCK, "staleness": "same-date vendor row only; null or NaN -> NaN; outside the declared domain [0.02, 5.0] (incl. <= 0 and +-inf) -> NaN, counted in plausibility",
        "caveats": ["vendor clean IV (earnings-calendar adjusted; calendar vintage unproven)", "often null", "no vintage proof",
                    "values outside the declared domain are NaN; in-domain vendor errors cannot be detected"]},
    "iv_atm_126d": {
        "group": "th", "column": "atmCenI_126d", "source_columns": ["atmCenI_126d"], "point_in_time": True, "domain": IV_DOMAIN,
        "units": "annualized ATM implied volatility, decimal, 126-session constant maturity",
        "clock": TH_CLOCK, "staleness": "same-date vendor row only; null or NaN -> NaN; outside the declared domain [0.02, 5.0] (incl. <= 0 and +-inf) -> NaN, counted in plausibility",
        "caveats": ["vendor clean IV (earnings-calendar adjusted; calendar vintage unproven)", "often null", "no vintage proof",
                    "values outside the declared domain are NaN; in-domain vendor errors cannot be detected"]},
    "earn_recent": {
        "group": "th", "column": "earnFlag", "source_columns": ["earnFlag"], "point_in_time": True,
        "units": "indicator: 1 when the session is the vendor earnings reaction day (earnFlag 0) or the day after (1); 0 for N and -1",
        "clock": TH_CLOCK,
        "staleness": "same-date vendor row only; null or unexpected earnFlag -> NaN",
        "caveats": ["earnFlag -1 (the session before a known future event) is deliberately mapped to 0, indistinguishable from N",
                    "earnFlag 0 is the price-reaction session (median |return| 3.9% vs 1.2% baseline in samples), so the event is public by its close",
                    "vendor calendar vintage unproven"]},
    "shares_out": {
        "group": "th", "column": "shares", "source_columns": ["shares", "cumulReturnFactor", "close", "volume"],
        "point_in_time": True, "domain": SHARES_OUT_DOMAIN, "requires": ["si_shares"],
        "units": "shares outstanding (vendor thousands x 1000), restated to the session's share basis",
        "clock": "A8-vendor-shares-lag90-restated-v2: last vendor observation of the line dated <= date(session)-90 calendar days with 0 < shares <= 1e8 (A9 thousands ceiling), times 1000 x cumulReturnFactor(session observation)/cumulReturnFactor(lag observation), divided by k of every factor-break-v1 repaired step (p,t,k) with lag < t <= session (k is known at t); observation = the role's present contract (unique key, finite positive close and factor, finite volume >= 0)",
        "staleness": "lag observation older than date(session)-90-400 days -> NaN; no same-date vendor observation -> NaN; a restatement spanning a factor-break-v1 kept_gap step (a step across more than 10 days over a mass session: artifact and genuine actions cannot be separated) -> NaN, counted; a line is withheld (NaN) from the date of its first vendor row above the A9 ceiling onward (point-in-time form of ruling C-81: the spine withholds the whole line, which would use rows after the session); a restated value outside the declared domain [1e5, 5e10] -> NaN, counted in plausibility; then a cell whose trailing 21-session median daily volume (session share basis) exceeds 3.0 x shares_out, or whose visible si_shares exceeds 5.0 x shares_out, -> NaN (vendor units defect), counted per rule in plausibility",
        "caveats": ["A8: vendor share runs start at the filing cover date, so same-date vendor shares would leak ~2 weeks; the 90-day modeled lag follows the research spine",
                    "restatement uses the vendor cumulReturnFactor ratio with the factor-break-v1 re-anchoring steps divided out (the vendor factor is not chained across 2021-01-04; see factor_break); genuine splits, consolidations and distributions stay in the ratio, so dividends move it by a few tenths of a percent",
                    "a genuine split that the vendor factor does not show (a raw move with no factor step) is not restated, as before",
                    "one price line's count, not the issuer total across share classes; an ADR line counts ADS"]},
    "mktcap_lagged": {
        "group": "lake", "column": "me_line", "source_columns": ["me_line", "formation_date", "line_id"],
        "point_in_time": False, "non_pit_aspects": ["presence"], "point_in_time_reason": SPINE_PRESENCE_NOT_PIT,
        "units": "USD: formation-session raw close x vendor shares lagged 90 days (spine me_line, me_basis vendor_shares_lag90)",
        "clock": "spine-formation<date(session)-strict-v1: the row of the latest monthly formation session strictly before date(session)",
        "staleness": "one formation only: a line without a spine row (or with NULL me_line) at that formation -> NaN. NaN mostly means the line is OUTSIDE the spine universe at that formation (not an eligible common type, or no vendor earnings evidence yet by the spine's forward count), not 'large' and not a data gap (v1: about 25-27% of role member cells, matching the non-common share); consumers needing a size for every name fall back to shares_out x raw_close. A session whose latest formation is more than 35 days old is refused (partial lake)",
        "caveats": ["spine universe gate uses vendor earnings evidence (nEarnCnt_504d > 0, which counts FUTURE events) and a 2026 directory snapshot: presence is not point in time (see point_in_time_reason)",
                    "one price line's value, not the issuer total"]},
    "size_grp": {
        "group": "lake", "column": "size_grp", "source_columns": ["size_grp", "formation_date", "line_id"],
        "point_in_time": False, "non_pit_aspects": ["presence"], "point_in_time_reason": SPINE_PRESENCE_NOT_PIT,
        "units": "ordinal code micro=0, small=1, large=2, mega=3 (Fama-French NYSE ME breakpoints p20/p50/p80; micro includes nano)",
        "clock": "spine-formation<date(session)-strict-v1: the row of the latest monthly formation session strictly before date(session)",
        "staleness": "one formation only: absent line or NULL size_grp -> NaN; NaN mostly means outside the spine universe (as mktcap_lagged). A session whose latest formation is more than 35 days old is refused (partial lake)",
        "caveats": ["same spine universe caveats as mktcap_lagged: presence is not point in time"]},
    "is_common": {
        "group": "lake", "column": "security_type", "source_columns": ["security_type", "line_id"],
        "point_in_time": False, "non_pit_aspects": ["values"],
        "point_in_time_reason": "static line classification from the 2026-09-18 Nasdaq Trader directory snapshot (lines listed then) and whole-history vendor earnings evidence (every other line): the value encodes survival to 2026 and future earnings (ETF/ADR/REIT/LP = survived; unknown = died without ever reporting earnings); look-ahead in both roles, validation included",
        "units": "indicator: 1 when line_types.security_type is common or common_unverified (the research universe's eligible types), 0 for every other type (ETF, ADR, fund, unknown, ...)",
        "clock": "static-line-classification;NOT-point-in-time",
        "staleness": "constant across sessions; NaN only when the line has no line_types row",
        "caveats": ["classification uses a 2026-09-18 Nasdaq Trader directory snapshot (listed lines) and whole-history vendor earnings evidence (delisted lines): information after " + rw.SEAL_DATE + " by construction; use as a coarse ETF/fund filter, not as a PIT signal",
                    "ADR is typed 0"]},
    "mkt_ret": {
        "group": "role", "source_columns": ["close.f64", "raw_close.f64", "present.u8", "member.u8"], "point_in_time": True,
        "units": "simple return, decimal: equal-weight mean of adjusted close-to-close returns (broadcast)",
        "definition": "row d >= 1: mean over instruments i with member[d-1,i]==1, present[d-1,i]==present[d,i]==1, finite positive close and raw_close at d-1 and d, and not guarded, of close[d,i]/close[d-1,i]-1; guarded (atx-impl strategy_target_replay.cpp rough_return) = non-finite r or |log adj ratio| > 1.5 or |log adj ratio| > |log raw ratio| + 0.10; the scalar fills every cell of row d with present[d,i]==1, NaN elsewhere; row 0 and sessions without contributors are NaN; sum is math.fsum (order independent)",
        "clock": "role-close-mark: row d uses role closes at sessions d-1 and d (known at the d 22:00 UTC mark, the close clock) and decision membership of d-1",
        "staleness": "no fill: row 0, cells not present at d, and sessions with no contributing name are NaN",
        "caveats": ["universe is the role's prior-63-session ADV top-N (ETFs included; common_stock_verified false): an ADV-universe equal-weight market, not a cap-weighted index",
                    "a broadcast field: identical across present names, so it avoids the runner's member-masked vec_avg blanking market templates for boundary names"]},
}
# Non-point-in-time fields are opt-in: produced only when named in --fields.
DEFAULT_FIELDS = tuple(k for k, spec in FIELDS.items() if spec["point_in_time"])

EXCLUDED_SOURCE_COLUMNS = [
    {"columns": "atmCenH_* (proposed hv_<tenor>)",
     "reason": "not realized historical volatility: equals atmCenI_<tenor> exactly whenever nEarnCnt_<tenor> == 0 (sample H/I p10=p50=p90=1.000) and differs only when a scheduled earnings event falls inside the tenor (corr with atmCenI_21d 0.95); an alternate earnings adjustment of the same IV built from the forward-looking earnings calendar and *EMove family. Realized volatility is computable from close in the DSL."},
    {"columns": "nEarnCnt_5d..nEarnCnt_504d",
     "reason": "count FUTURE earnings events within the next N sessions (nEarnCnt_5d is 1 on the five sessions up to and including earnFlag -1 and 0 from the reaction session on): forward-looking"},
    {"columns": "nEarnCnt", "reason": "semantics unverified (typically 8, sometimes 7 on event sessions)"},
    {"columns": "hEMove, iEMove, wkD1, shD1, qtrD1, lnD1", "reason": "forward-horizon moves; excluded as look-ahead by the ORATS history loader design (2026-06-16)"},
    {"columns": "earnFlag == -1", "reason": "presumes a known future event date; mapped to 0 (same as N)"},
    {"columns": "same-date vendor shares", "reason": "A8: vendor runs start at the filing cover date; replaced by the 90-day lag (shares_out)"},
]

# ---------------------------------------------------------------------------
# Issuer-level fields (library v4, T21): fundamentals, company market equity, industry groups.
# Kept in their own registry: FIELDS, DEFAULT_FIELDS and every legacy output byte are unchanged
# when none of these is requested. All are point in time and opt-in (they need pinned inputs).
# Rulings: v4-prereg.md R2 (declared 2026-09-27 before any v4 TRAIN read) and T18 report section 7.
# ---------------------------------------------------------------------------

FUND_LAG_SESSIONS_DECLARED = 1   # v4-prereg R2: +1 declared lag session (--fund-lag-sessions 1)
# Link rule (controller ruling, T21): T19 rows are already point in time (r4 links_asof applied; every row has
# available_at <= start 22:00 UTC), so a line matches on start <= date(session) <= end_incl only. The invariant is
# asserted per row and violations are counted; a violating row is also gated by available_at <= date(session)
# 22:00 UTC, which never binds on a row that meets the invariant.
FUND_STALE_DAYS = 200            # rows of quarterly filers (events staleness_days)
FUND_STALE_DAYS_ANNUAL = 400     # rows of annual-only filers
GRP_STALE_DAYS = 550
MARK_NS = 22 * 3_600_000_000_000  # the role close mark: session date 22:00 UTC
SEAL_NS = (SEAL - EPOCH).days * DAY_NS
OPEN_END_DAY = np.iinfo(np.int64).max
NULL_PERIOD_DAY = -(1 << 40)     # a null period_end is never fresh
SIC_RANGE = (100, 9999)          # atx_db.reference_classifications SIC_MIN / SIC_MAX
# French's Fama-French 12 numbering (Siccodes12): 1 NoDur .. 12 Other.
FF12_NUMBERS = {"NoDur": 1, "Durbl": 2, "Manuf": 3, "Enrgy": 4, "Chems": 5, "BusEq": 6, "Telcm": 7, "Utils": 8,
                "Shops": 9, "Hlth": 10, "Money": 11, "Other": 12}
SIC_MAPPING_VERSIONS = {"ff12": "french_siccodes12_v2", "ff49": "french_siccodes49_v1"}

LINK_RULE = (
    "line -> CIK: the pinned identity-bridge row of the line whose [start, end_incl] contains date(session t) (bridge "
    "rows are point in time: available_at <= start 22:00 UTC, asserted per row; a violating row, counted, is also gated "
    "by available_at <= date(session t) 22:00 UTC); the value is published on primary (P) lines only (secondary J lines, "
    "unlinked lines and lines with two qualifying rows for different CIKs -> NaN)")
FUND_CLOCK = (
    "fund-events-lagged-v1: the linked CIK's latest events row with clock < date(session t-L) 22:00 UTC, L = {lag} declared "
    "lag session(s) on the role calendar (--fund-lag-sessions): usable from the first session whose 22:00 UTC mark follows "
    "the clock, plus L sessions; sessions t < L -> NaN; clock = events accepted_utc (FSDS SUB accepted_utc of the "
    "filing, else FC1 = filed 00:00 UTC + 46 h, clock_basis cf_fc1); consumer selection rule of atx.fundamental-events/v1 "
    "(row-level, latest-clock-wins): for a CIK and mark, take the latest visible row (max accepted_utc, tie by accession). "
    + LINK_RULE)
FUND_STALENESS = (
    "atx.fundamental-events/v1 rule: every item is that row's value (NaN stays NaN; do not fall back to an older row per "
    "item, so paired items always share one anchor); the whole row is stale, and every item NaN, when date(d) - "
    "period_end > staleness_days, where staleness_days = 200 if the CIK has a 10-Q/10-QT (or /A) accession with clock in "
    "(clock - 400 d, clock], else 400 (annual-only filer); only 200 and 400 are admitted; no visible row or null "
    "period_end -> NaN; restatements are latest-clock-wins: a restated value enters at the restating filing's clock and "
    "is never backdated")
FUND_CAVEATS = [
    "values modeled/unaccepted (not F.1-accepted): bounded v4 events producer over the CompanyFacts CF-R snapshot "
    "2026-09-20 (us-gaap + dei only: IFRS filers and most ADRs have no values) and FSDS SUB acceptance clocks",
    "identity from the pinned identity bridge (r4 rehearsal links, rehearsal_identity recorded in source_checks; "
    "scope_complete=false: about 60% of role member cells are linked, TRAIN and VAL alike)",
    "FC1 fallback clock (filed + 46 h) where FSDS SUB lacks the accession: counted per field (fc1_finite_member_cells)",
    "item definitions (concept chains, TTM and discrete-quarter derivation, zero-fill rules for debt, dvc_ttm, "
    "prstkc_ttm, sstk_ttm) belong to the events producer (atx-engine/tools/fundamental_events_schema.md, "
    "atx.fundamental-events/v1); the events manifest SHA-256 is pinned in sources",
]
ME_CLOCK = (
    "role-close-mark: sum over every role line with a qualifying P or J identity-bridge link to the CIK at session t of "
    "shares_out[t] x raw_close[t] (this run's shares_out field; role raw_close.f64 of present lines); published on the "
    "CIK's primary lines. " + LINK_RULE)
ME_STALENESS = (
    "no fill: NaN when any role line linked to the CIK at session t has a non-finite or non-positive shares_out x "
    "raw_close (absent, unpriced, or shares_out NaN); NaN on unlinked, secondary (J) and ambiguous lines")
ME_CAVEATS = [
    "issuer market equity is line-summed vendor ME (shares_out: 90-day lagged vendor shares restated to the session "
    "basis), not DEI entity shares",
    "share classes outside the role (not in its ADV universe) or without a qualifying link are missing: multi-class "
    "issuers can be understated",
    "identity from the pinned identity bridge (r4 rehearsal links, rehearsal_identity recorded in source_checks)",
]
GRP_CLOCK = (
    "fund-events-lagged-v1 applied to the SIC events table: the linked CIK's latest SIC row (FSDS SUB sic as of the "
    "filing; max accepted_utc, tie by accession) with accepted_utc < date(session t-L) 22:00 UTC, L = {lag}; sessions "
    "t < L -> NaN; a row whose sic is null or "
    "outside [100, 9999] carries no SIC and is skipped (counted). " + LINK_RULE)
GRP_STALENESS = (
    "age = date(session) - UTC date of the visible SIC row's accepted_utc, calendar days; age > 550 -> NaN; no visible "
    "SIC row -> "
    "NaN; NaN is an unknown label (the VM keeps NaN labels out of every group)")
GRP_CAVEATS = [
    "SIC as of each filing (FSDS SUB), not the SEC's current snapshot: about 11% of CIKs changed SIC in 2015-2024",
    "mapping from atx_db.reference_classifications (French Siccodes12 v2 / Siccodes49 v1); module code identity and a "
    "digest of the full SIC -> code tables are in source_checks.issuer.sic_mapping",
    "identity from the pinned identity bridge (r4 rehearsal links, rehearsal_identity recorded in source_checks)",
]
# (field/item name, units, what the events producer's item holds). Items are the T18 section 6 proposal plus
# be_lag1q_lag4 (the lag-4 opening equity that dROE needs).
FUND_ITEMS = (
    ("be", "USD", "book equity at the latest period end"),
    ("at", "USD", "total assets at the latest period end"),
    ("at_lag4", "USD", "total assets four fiscal quarters before the latest period end"),
    ("lt", "USD", "total liabilities at the latest period end"),
    ("che", "USD", "cash and short-term investments at the latest period end"),
    ("debt", "USD", "total debt at the latest period end"),
    ("sale_ttm", "USD", "revenue, trailing twelve months"),
    ("gp_ttm", "USD", "gross profit, trailing twelve months"),
    ("oi_ttm", "USD", "operating income, trailing twelve months"),
    ("ni_ttm", "USD", "net income, trailing twelve months"),
    ("ni_q", "USD", "net income, latest discrete fiscal quarter"),
    ("ni_q_lag4", "USD", "net income, the same fiscal quarter one year earlier"),
    ("be_lag1q", "USD", "book equity one quarter before the latest period end (opening equity of the quarter)"),
    ("be_lag1q_lag4", "USD", "opening book equity of the same fiscal quarter one year earlier"),
    ("cfo_ttm", "USD", "net cash from operating activities, trailing twelve months"),
    ("capx_ttm", "USD", "capital expenditures, trailing twelve months"),
    ("xrd_ttm", "USD", "research and development expense, trailing twelve months"),
    ("dvc_ttm", "USD", "common dividends paid, trailing twelve months"),
    ("prstkc_ttm", "USD", "repurchases of common stock, trailing twelve months"),
    ("sstk_ttm", "USD", "proceeds from issuance of common stock, trailing twelve months"),
    ("txt_q", "USD", "income tax expense, latest discrete fiscal quarter"),
    ("txt_q_lag4", "USD", "income tax expense, the same fiscal quarter one year earlier"),
    ("shrs_q", "shares", "weighted-average diluted (else basic) shares of the fiscal quarter (else year) ending at "
                         "the latest period end"),
    ("shrs_q_lag4", "shares", "the same share concept ending four fiscal quarters earlier (pair NaN on a scale error)"),
    ("noa", "USD", "net operating assets at the latest period end: at - che - lt + debt"),
    ("noa_lag4", "USD", "net operating assets four fiscal quarters earlier"),
    ("sue", "unitless", "seasonal-random-walk SUE of the quarter ending at the latest period end on first-reported "
                        "quarterly net income (not EPS): (NI_q - NI_q-4) / sd of the previous <= 8 seasonal differences, "
                        ">= 4 required, sd > 0"),
    ("fscore", "count 0-9", "Piotroski F-score, all nine terms required"),
)
ISSUER_FIELDS = {}
for _name, _units, _what in FUND_ITEMS:
    ISSUER_FIELDS[_name] = {
        "group": "issuer", "kind": "fund", "item": _name, "point_in_time": True, "lagged": True,
        "source_columns": [_name, "cik", "accepted_utc", "accession", "period_end", "staleness_days"],
        "units": _units, "clock": FUND_CLOCK, "staleness": FUND_STALENESS, "caveats": FUND_CAVEATS,
        "definition": f"{_what}: item `{_name}` of the pinned fundamental events artifact, value of the linked CIK's "
                      f"latest visible events row (see clock and staleness)"}
ISSUER_FIELDS["me_company"] = {
    "group": "issuer", "kind": "me", "point_in_time": True, "lagged": False, "requires": ["shares_out"],
    "source_columns": ["shares_out", "raw_close.f64", "present.u8", "identity-bridge links"],
    "units": "USD: issuer market equity, sum of shares_out x raw_close over the issuer's role lines",
    "clock": ME_CLOCK, "staleness": ME_STALENESS, "caveats": ME_CAVEATS,
    "definition": "me_company[t, i] = sum over role lines j linked (P or J) at t to the CIK of primary line i of "
                  "shares_out[t, j] x raw_close[t, j]; NaN when any term is not finite and positive"}
for _name, _code, _units in (
        ("grp_sic2", "sic2", "categorical code: SIC major group floor(sic / 100), 1-99, stored as f64"),
        ("grp_ff12", "ff12", "categorical code: Fama-French 12-industry number (1 NoDur, 2 Durbl, 3 Manuf, 4 Enrgy, "
                             "5 Chems, 6 BusEq, 7 Telcm, 8 Utils, 9 Shops, 10 Hlth, 11 Money, 12 Other), stored as f64"),
        ("grp_ff49", "ff49", "categorical code: Fama-French 49-industry number 1-49, stored as f64; a SIC listed under "
                             "no FF49 industry -> NaN (never 49 Other)")):
    ISSUER_FIELDS[_name] = {
        "group": "issuer", "kind": "grp", "code": _code, "point_in_time": True, "lagged": True,
        "source_columns": ["sic", "cik", "accepted_utc", "accession"], "units": _units, "clock": GRP_CLOCK,
        "staleness": GRP_STALENESS,
        "caveats": GRP_CAVEATS,
        "definition": f"{_code} of the linked CIK's latest visible valid SIC row: sic2 = floor(sic / 100); ff12 / ff49 = "
                      f"atx_db.reference_classifications fama_french_12_for_sic / fama_french_49_for_sic "
                      f"({SIC_MAPPING_VERSIONS['ff12']} / {SIC_MAPPING_VERSIONS['ff49']}) as French's industry number"}
del _name, _units, _what, _code

# ---------------------------------------------------------------------------
# FINRA daily short sale volume (library v6.1, v4-prereg.md "## v6.1 sub-alpha", declared before any read of the
# field or its returns). Own registry, opt-in (needs --finra-short-volume): FIELDS, DEFAULT_FIELDS and every other
# field's output bytes are unchanged whether or not it is requested.
# ---------------------------------------------------------------------------
SV_FORMULA_ID = "finra-cnms-ratio126-lag1-v1"
SV_WINDOW = 126                  # sessions d-126..d-1
SV_MIN_SESSIONS = 63             # fewer sessions with a mapped row -> NaN
SV_LAG_SESSIONS = 1              # the file dated d is never used at d
SV_FILE_PATTERN = r"^CNMSshvol(\d{8})\.txt\.gz$"
SV_HEADER = b"Date|Symbol|ShortVolume|ShortExemptVolume|TotalVolume|Market"
SV_RECEIPT = "manifest.csv"      # the downloader's receipt: sha256 / bytes / rows of each DECOMPRESSED file
# PIT FINRA symbol map of the short-interest producer (build-equity/audits/iteration21_finra_si_asof.py, the
# mapping_report.json behind si_shares / si_dtc), ported verbatim: canonicaliser, 7-day ticker lookback,
# ambiguity drop and exact-match collision rule. The CNMS files spell FINRA/CQS suffixes as lowercase markers
# ("CpK" preferred K, "GCVr" rights, "GTXw" when-issued, "BF/B" class B) where the short-interest symbolCode
# (and ORATS ticker_tk: "C.PRK", "ACP.RT", "AAN.WI", "BF.B") spell them PR / RT / WI: the markers are rewritten to
# that spelling BEFORE the unchanged canonicaliser, so a class share or preferred is handled exactly as in the
# short-interest map (without it "CpK" canonicalises to "CPK", collides with Chesapeake Utilities' CPK and the
# collision rule drops both).
SV_TICKER_LOOKBACK_DAYS = 7      # iteration21_finra_si_asof.LOOKBACK_DAYS
SI_CANON_PATTERN = r"[.\s/\-]"   # iteration21_finra_si_asof._CANON_RE
CNMS_SUFFIX_MARKERS = (("p", "PR"), ("r", "RT"), ("w", "WI"))
SV_SYMBOL_PATTERN = r"^[A-Za-z0-9./\-]+$"
SV_MAP_RULE = (
    "finra-si-symbol-map-v1 (the short-interest producer's rule): CNMS Symbol -> short-interest spelling (lowercase "
    "suffix markers p -> PR, r -> RT, w -> WI; '/' is dropped by the canonicaliser) -> canonical = upper-case, strip "
    "'.', whitespace, '/', '-' (the same canonicaliser on ORATS ticker_tk); ticker source = the role's TickerHistory3 "
    "ticker_tk on the last vendor trading date <= the file date within 7 calendar days (rows with securityID > 0 and "
    "a non-blank ticker; a trading date = a date with any such row); a canonical ticker held by more than one "
    "securityID that date is ambiguous and dropped; several symbols of one file on one securityID keep the one "
    "whose short-interest spelling equals a raw ticker (strip, upper) of that securityID that date, else all are "
    "dropped; within a file the last row of a symbol wins")
SV_CLOCK = (
    f"{SV_FORMULA_ID}: role session d uses the CNMS files dated on the {SV_WINDOW} sessions before d (d-{SV_WINDOW}.."
    "d-1; role calendar, extended before the role's first session by the FINRA file dates, one file per US equity "
    "session); the file dated d is never used at d (lag 1 session; FINRA publishes it the evening of d). Ticker map "
    "of each file: TickerHistory3 rows dated <= the file date (a row dated s is known at the s 22:00 UTC mark)")
SV_FIELDS = {
    "sv_ratio126": {
        "group": "finra_sv", "point_in_time": True,
        "source_columns": ["Date", "Symbol", "ShortVolume", "TotalVolume", "ticker_tk", "tradingDate", "securityID"],
        "units": "ratio (decimal, [0, 1]): summed FINRA consolidated NMS short sale volume / summed total volume",
        "clock": SV_CLOCK,
        "staleness": (f"NaN when fewer than {SV_MIN_SESSIONS} of the sessions d-{SV_WINDOW}..d-1 carry a mapped row "
                      "of the instrument (its PIT ticker absent from the file, unmapped, ambiguous or dropped by the "
                      "collision rule, or no file that session) or when the summed TotalVolume is 0; no fill"),
        "definition": (f"sv_ratio126[d, i] = sum over s in S of ShortVolume[s, i] / sum over s in S of "
                       f"TotalVolume[s, i]; S = the sessions among d-{SV_WINDOW}..d-1 whose CNMSshvol file has a row "
                       f"mapped to instrument i ({SV_MAP_RULE}); NaN when |S| < {SV_MIN_SESSIONS} or the TotalVolume "
                       f"sum is 0. Formula id {SV_FORMULA_ID}"),
        "caveats": [
            "FINRA Reg SHO daily files (consolidated NMS: FINRA TRFs/ADF plus exchange short sale volume) count "
            "market-maker short sales that hedge customer buying: the daily ratio is mostly liquidity provision "
            "(about half of off-exchange volume); only the long-window aggregate is the pre-registered signal",
            "file bytes as downloaded from the FINRA CDN (receipt manifest.csv; downloaded_at range in "
            "short_volume.files), long after the trade date: whether FINRA re-published a file after its trade "
            "date is unverified (vintage risk)",
            "symbology gaps: symbols whose PIT ORATS ticker differs beyond the canonicaliser (renames between the "
            "vendor and FINRA, units, when-issued lines) are unmapped; see short_volume.mapping per-year counts",
            "a session without a CNMS file (FINRA outage) simply contributes no row; counted in "
            "short_volume.calendar"]},
}
# Every producible field: the legacy registry first (its order is the manifest order), then the issuer fields,
# then the short-volume field.
ALL_FIELDS = {**FIELDS, **ISSUER_FIELDS, **SV_FIELDS}
# W5a registry hook: opt-in field modules (research_fields_sec.py: SEC earnings calendar, Form 4, 8-K). Each appends its
# registry after every field above and runs via the FIELD_MODULES loops in run()/main(); --reuse copies its fields by
# the module's own PRODUCERS, stage pins and formula (reuse_module_fields, v8 C-3).
import research_fields_sec  # noqa: E402  (same directory; it does not import this module)
FIELD_MODULES = [research_fields_sec.bind(globals())]
ALL_FIELDS.update(research_fields_sec.FIELDS)

# Field reuse (--reuse). A field's formula id pins its spec-level definition and --reuse also keys on the producing
# code (FIELD_PRODUCERS); bump a revision here only for a change neither shows (e.g. a vendor's changed semantics).
FORMULA_REVISION: dict = {}
FORMULA_DEFINITION_KEYS = ("units", "clock", "staleness", "source_columns", "definition", "point_in_time",
                           "non_pit_aspects")
REUSE_RULE = ("a field is copied from the prior fields directory iff: the prior manifest is complete and bound to this "
              "role (manifest, sessions, ids, member SHA-256); same field id; same formula id (SHA-256 of canonical "
              "{field, revision, units, clock (declared lag), staleness, source_columns, definition, point_in_time, "
              "non_pit_aspects, domain}); same producing code (SHA-256 of the AST, docstrings dropped, of the field "
              "group's builder functions and every module-level definition they reach, for the code that produced "
              "the prior payload -- this builder when its code_sha256_lf matches, else its code_git_blob_sha1 from "
              "git, re-hashed to code_sha256_lf; not recoverable -> recomputed) and, for grp_* fields, the same SIC "
              "mapping table SHA-256; same inputs (every prior source path lies under this run's source argument "
              "for the field's group and re-hashes to its recorded SHA-256); every field it depends on is reused; "
              "copied bytes re-hash to the prior pin")
REUSE_DIRECTORY_RULE = ("v8 C-3: sv_ratio126's CNMS directory source (no single file SHA-256) passes the same-inputs "
                        "check iff the files this run reads for this role (sv_window_files of the current directory "
                        "listing) re-hash, by SV_FILES_LIST_RULE, to the recorded files_sha256 with the recorded "
                        "files_read, first_file_date and last_file_date")
REUSE_SOURCE_CHECK_KEYS = {"th": "tickerhistory", "lake": "lake", "issuer": "issuer", "finra_sv": "finra_short_volume"}
# The producing code of each field group (R1 M-6): its builder functions; producer_fingerprints adds every
# module-level definition they reach. run() and main() orchestrate and are never part of a producer.
FIELD_PRODUCERS = {"role": ("market_return_field",), "finra": ("read_schedule", "finra_field"),
                   "th": ("tickerhistory_fields",), "lake": ("lake_fields",), "issuer": ("issuer_fields",),
                   "finra_sv": ("sv_field",)}
PRODUCER_ORCHESTRATION = frozenset({"run", "main"})
REUSE_CODE_RULE = ("the manifest's code_sha256 is the builder of this manifest and of the computed fields; a reused "
                   "payload was produced by the code named in its reused_from (code_sha256_lf, code_git_blob_sha1, "
                   "producer_sha256), carried unchanged through chained reuse")


def canonical(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def day_of(value: dt.date) -> int:
    return (value - EPOCH).days


def date_of(day: int) -> str:
    return (EPOCH + dt.timedelta(days=int(day))).isoformat()


def sha_bytes(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def sha_file(path: Path, budget: "Budget | None" = None) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
            if budget:
                budget.check("hash")
    return h.hexdigest()


def code_identity(path: Path) -> dict:
    """The executed code's pins: raw bytes (checkout dependent), LF-normalised bytes, and git blob id."""
    raw = path.read_bytes()
    lf = raw.replace(b"\r\n", b"\n")
    return {"code_sha256": sha_bytes(raw), "code_sha256_lf": sha_bytes(lf),
            "code_git_blob_sha1": hashlib.sha1(b"blob %d\0" % len(lf) + lf).hexdigest()}


def identity(path: Path):
    s = path.stat()
    return (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns)


class Budget:
    """Cooperative time/RSS guard polled at batch and date boundaries (not a kernel quota)."""

    def __init__(self, max_rss_mib=700, max_seconds=1800.0):
        if max_rss_mib < 128 or not 0 < max_seconds <= 7200:
            raise ValueError("invalid resource budget")
        self.max_rss = int(max_rss_mib) << 20
        self.deadline = time.monotonic() + max_seconds
        self.peak = 0
        try:
            import psutil
            self._rss = psutil.Process().memory_info
        except ImportError:  # pragma: no cover - psutil ships with the admitted interpreter
            self._rss = None

    def rss(self) -> int:
        return int(self._rss().rss) if self._rss else 0

    def check(self, stage: str):
        if time.monotonic() > self.deadline:
            raise TimeoutError(f"time budget exceeded at {stage}; partial output kept unpublished")
        rss = self.rss()
        self.peak = max(self.peak, rss)
        if rss > self.max_rss:
            raise MemoryError(f"RSS {rss >> 20} MiB exceeds --max-rss-mib at {stage}; partial output kept unpublished")

    def admit(self, planned: int, stage: str):
        if self.rss() + planned > self.max_rss:
            raise ValueError(f"{stage}: planned {planned >> 20} MiB + current RSS exceeds --max-rss-mib")

    def report(self, stage: str, **values):
        self.check(stage)
        print(canonical({"stage": stage, "rss_mib": self.rss() >> 20, **values}), flush=True)


def publish(path: Path, value):
    """Exclusive publish-last manifest: a reader sees no manifest or its complete fsynced bytes."""
    content = (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")
    pending = path.with_name("." + path.name + ".pending")
    with pending.open("xb") as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    os.link(pending, path)
    pending.unlink()
    return content


# ---------------------------------------------------------------------------
# Role axes
# ---------------------------------------------------------------------------

class Role:
    def __init__(self, directory: Path, expected_sha256: str):
        self.dir = directory
        manifest_bytes = (directory / "manifest.json").read_bytes()
        self.manifest_sha256 = sha_bytes(manifest_bytes)
        if self.manifest_sha256 != expected_sha256.lower():
            raise ValueError("role manifest SHA-256 does not match --role-sha256")
        m = json.loads(manifest_bytes)
        if m.get("schema") != ROLE_SCHEMA or m.get("status") != "complete":
            raise ValueError("role is not a complete atx.recent-research-role/v1 payload")
        if m.get("instrument_namespace") != "spiderrock.securityID":
            raise ValueError("role instrument namespace is not spiderrock.securityID")
        self.manifest = m
        blobs = {}
        for name in ("sessions.i64", "ids.u64", "member.u8"):
            blob = (directory / name).read_bytes()
            entry = m["files"][name]
            if len(blob) != entry["bytes"] or sha_bytes(blob) != entry["sha256"]:
                raise ValueError(f"role {name} bytes do not match the role manifest")
            blobs[name] = blob
        self.sessions_sha256 = m["files"]["sessions.i64"]["sha256"]
        self.ids_sha256 = m["files"]["ids.u64"]["sha256"]
        sessions = np.frombuffer(blobs["sessions.i64"], dtype="<i8").astype(np.int64)
        ids = np.frombuffer(blobs["ids.u64"], dtype="<u8")
        self.n_dates, self.n = int(m["dates"]), int(m["instruments"])
        if len(sessions) != self.n_dates or len(ids) != self.n or not self.n_dates or not self.n:
            raise ValueError("role axes disagree with the manifest shape")
        if np.any(sessions % DAY_NS != 0) or np.any(np.diff(sessions) <= 0):
            raise ValueError("role sessions are not strictly increasing midnight labels")
        if np.any(ids == 0) or np.any(ids >= np.uint64(1 << 63)) or np.any(np.diff(ids.astype(np.int64)) <= 0):
            raise ValueError("role ids are not strictly increasing positive i64 securityIDs")
        self.days = sessions // DAY_NS
        self.ids = ids.astype(np.int64)
        # The roles end before the research seal; assert it rather than trust it.
        if int(self.days[-1]) >= day_of(SEAL):
            raise rw.SealError(rw.seal_message("role contains a session"))
        self.member = np.frombuffer(blobs["member.u8"], dtype="u1").reshape(self.n_dates, self.n)
        years = self.days.astype("datetime64[D]").astype("datetime64[Y]").astype(np.int64) + 1970
        self.years = years
        self.score_begin = int(m.get("score_begin", 0))
        self.score_end = int(m.get("score_end", self.n_dates))
        self.source_sha256 = m.get("source_sha256")

    def columns_of(self, sid: np.ndarray):
        """Positions of `sid` on the role id axis and a mask of which are on it."""
        pos = np.minimum(np.searchsorted(self.ids, sid), self.n - 1)
        return pos, self.ids[pos] == sid


class FieldWriter:
    """Streams one date-major f64 field and accumulates member-cell coverage/value statistics."""

    def __init__(self, output: Path, name: str, role: Role):
        self.name, self.role = name, role
        self.path = output / f"{name}.f64"
        self.f = self.path.open("xb")
        self.t = 0
        self.year = {}
        self.score = [0, 0]
        self.finite_all = 0
        self.vmin, self.vmax, self.vsum, self.vcount = np.inf, -np.inf, 0.0, 0

    def write(self, row: np.ndarray):
        if row.shape != (self.role.n,):
            raise ValueError(f"{self.name}: row shape mismatch")
        row = np.where(np.isfinite(row), row, np.nan).astype("<f8")  # canonical quiet NaN bytes
        self.f.write(row.tobytes())
        t = self.t
        member = self.role.member[t] != 0
        finite = np.isfinite(row)
        fm = member & finite
        mc, fc = int(np.count_nonzero(member)), int(np.count_nonzero(fm))
        y = self.year.setdefault(int(self.role.years[t]), [0, 0])
        y[0] += mc
        y[1] += fc
        if self.role.score_begin <= t < self.role.score_end:
            self.score[0] += mc
            self.score[1] += fc
        self.finite_all += int(np.count_nonzero(finite))
        if fc:
            v = row[fm]
            self.vmin, self.vmax = min(self.vmin, float(v.min())), max(self.vmax, float(v.max()))
            self.vsum += float(v.sum())
            self.vcount += fc
        self.t += 1

    def close(self):
        self.f.flush()
        os.fsync(self.f.fileno())
        self.f.close()
        if self.t != self.role.n_dates:
            raise ValueError(f"{self.name}: wrote {self.t} of {self.role.n_dates} dates")

    def coverage(self):
        frac = lambda f, m: round(f / m, 6) if m else None
        member = sum(v[0] for v in self.year.values())
        finite = sum(v[1] for v in self.year.values())
        return {"member_cells": member, "finite_member_cells": finite, "finite_member_frac": frac(finite, member),
                "score_window": {"member_cells": self.score[0], "finite_member_cells": self.score[1],
                                 "finite_member_frac": frac(self.score[1], self.score[0])},
                "per_year": {str(y): {"member_cells": v[0], "finite_member_cells": v[1],
                                      "finite_member_frac": frac(v[1], v[0])} for y, v in sorted(self.year.items())},
                "finite_cells_all": self.finite_all,
                "member_finite_min": self.vmin if self.vcount else None,
                "member_finite_max": self.vmax if self.vcount else None,
                "member_finite_mean": (self.vsum / self.vcount) if self.vcount else None}


class RoleRows:
    """Date-major role payload rows read in session order, hashed as read and verified against the role manifest."""

    DTYPES = {"close.f64": "<f8", "raw_close.f64": "<f8", "volume.f64": "<f8", "present.u8": "u1"}

    def __init__(self, role: Role, names):
        self.role, self.names, self.handles, self.hashes = role, list(names), {}, {}
        for name in self.names:
            entry = role.manifest["files"].get(name)
            if entry is None or entry["bytes"] != role.n_dates * role.n * np.dtype(self.DTYPES[name]).itemsize:
                raise ValueError(f"role {name} is missing or its size disagrees with the role shape")
        try:
            for name in self.names:
                self.handles[name], self.hashes[name] = (role.dir / name).open("rb"), hashlib.sha256()
        except BaseException:
            self.close()
            raise

    def row(self, name: str) -> np.ndarray:
        size = self.role.n * np.dtype(self.DTYPES[name]).itemsize
        blob = self.handles[name].read(size)
        if len(blob) != size:
            raise ValueError(f"role {name} is truncated")
        self.hashes[name].update(blob)
        return np.frombuffer(blob, dtype=self.DTYPES[name])

    def verify(self) -> list:
        """After the last row: the files end there and their bytes equal the role manifest's."""
        sources = []
        for name in self.names:
            if self.handles[name].read(1):
                raise ValueError(f"role {name} is longer than the role shape")
            entry = self.role.manifest["files"][name]
            if self.hashes[name].hexdigest() != entry["sha256"]:
                raise ValueError(f"role {name} bytes do not match the role manifest")
            sources.append({"path": str((self.role.dir / name).resolve()), "bytes": entry["bytes"], "sha256": entry["sha256"]})
        return sources

    def close(self):
        for h in self.handles.values():
            h.close()


def column_median(window: np.ndarray):
    """Per-column median of the finite values of a (rows x lines) window and their count (NaN when none)."""
    count = np.count_nonzero(np.isfinite(window), axis=0)
    ordered = np.sort(window, axis=0)  # NaN sorts last
    lo = np.take_along_axis(ordered, np.maximum((count - 1) // 2, 0)[None, :], axis=0)[0]
    hi = np.take_along_axis(ordered, np.maximum(count // 2, 0)[None, :], axis=0)[0]
    return np.where(count > 0, (lo + hi) / 2, np.nan), count


def digest_and_quantiles(path: Path, role: Role, count: int, budget: Budget):
    """One read of a published field: SHA-256 of its bytes and quantiles of its finite member cells."""
    budget.admit(count * 8 + (16 << 20), f"{path.name}-quantiles")
    values = np.empty(count, dtype=np.float64)
    h, filled, row_bytes = hashlib.sha256(), 0, role.n * 8
    with path.open("rb") as f:
        for t in range(role.n_dates):
            blob = f.read(row_bytes)
            if len(blob) != row_bytes:
                raise ValueError(f"{path.name} is truncated")
            h.update(blob)
            row = np.frombuffer(blob, dtype="<f8")
            v = row[(role.member[t] != 0) & np.isfinite(row)]
            if filled + len(v) > count:
                raise ValueError(f"{path.name}: finite member cells disagree with the writer")
            values[filled:filled + len(v)] = v
            filled += len(v)
            if t % 256 == 0:
                budget.check(f"{path.name}-digest")
        if f.read(1):
            raise ValueError(f"{path.name} is longer than the role shape")
    if filled != count:
        raise ValueError(f"{path.name}: finite member cells disagree with the writer")
    if not count:
        return h.hexdigest(), None
    q = np.quantile(values, [x for _, x in QUANTILES], overwrite_input=True)
    return h.hexdigest(), {k: float(x) for (k, _), x in zip(QUANTILES, q)}


# ---------------------------------------------------------------------------
# FINRA short interest (as-of join replicating atx-impl asof_field.cpp)
# ---------------------------------------------------------------------------

def read_schedule(finra: Path):
    blob = (finra / "dissemination_schedule.csv").read_bytes()
    rows = list(csv.DictReader(io.StringIO(blob.decode("utf-8"))))
    dissemination = np.array(sorted({day_of(dt.date.fromisoformat(r["dissemination_date"])) for r in rows}), dtype=np.int64)
    before = [dt.date.fromisoformat(r["dissemination_date"]) for r in rows
              if dt.date.fromisoformat(r["settlement_date"]) < FINRA_REPUBLICATION_SETTLEMENT_BEFORE]
    cutoff = day_of(max(before)) if before else None
    source = {"path": str((finra / "dissemination_schedule.csv").resolve()), "bytes": len(blob), "sha256": sha_bytes(blob)}
    return dissemination, cutoff, source


def parse_asof_csv(blob: bytes):
    """Strict CSV contract of atx-impl parse_asof_csv; returns (ids i64, days i64, values f64)."""
    first = blob.split(b"\n", 1)[0]
    if (first[:-1] if first.endswith(b"\r") else first) != FINRA_HEADER:
        raise ValueError("asof csv: header must be exactly 'security_id,available_at,value'")
    # One optional trailing newline; any empty line (or a bare CR line) is a C++ parse error.
    if b"\n\n" in blob or b"\n\r\n" in blob or b"\r\r" in blob or blob.endswith(b"\n\r"):
        raise ValueError("asof csv: empty line")
    table = pacsv.read_csv(
        io.BytesIO(blob),
        read_options=pacsv.ReadOptions(use_threads=False, block_size=1 << 22),
        parse_options=pacsv.ParseOptions(delimiter=",", quote_char=False, double_quote=False,
                                         escape_char=False, newlines_in_values=False),
        convert_options=pacsv.ConvertOptions(column_types={c: pa.string() for c in ("security_id", "available_at", "value")},
                                             strings_can_be_null=False, include_columns=["security_id", "available_at", "value"]))
    if table.column_names != ["security_id", "available_at", "value"]:
        raise ValueError("asof csv: unexpected columns")
    sid_text, date_text, value_text = (table.column(i).combine_chunks() for i in range(3))
    if not pc.all(pc.match_substring_regex(sid_text, r"^[0-9]+$")).as_py():
        raise ValueError("asof csv: security_id is not a positive i64")
    ids = pc.cast(sid_text, pa.int64()).to_numpy()
    if np.any(ids <= 0):
        raise ValueError("asof csv: security_id is not a positive i64")
    if not pc.all(pc.match_substring_regex(date_text, r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")).as_py():
        raise ValueError("asof csv: available_at is not YYYY-MM-DD")
    days = pc.cast(pc.cast(pc.strptime(date_text, format="%Y-%m-%d", unit="s"), pa.date32()), pa.int32()).to_numpy().astype(np.int64)
    lower = pc.utf8_lower(value_text)
    nan_literal = pc.or_(pc.equal(lower, "nan"), pc.equal(pc.utf8_length(value_text), 0)).to_numpy(zero_copy_only=False)
    decimal = pc.match_substring_regex(value_text, r"^-?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][-+]?[0-9]+)?$").to_numpy(zero_copy_only=False)
    if np.any(~nan_literal & ~decimal):
        raise ValueError("asof csv: value is not a finite decimal")
    values = np.full(len(ids), np.nan)
    if decimal.any():
        parsed = pc.cast(pc.filter(value_text, pa.array(decimal)), pa.float64()).to_numpy()
        if not np.all(np.isfinite(parsed)):
            raise ValueError("asof csv: value is not a finite decimal")
        values[decimal] = parsed
    order = np.lexsort((days, ids))
    ids, days, values = ids[order], days[order], values[order]
    if np.any((ids[1:] == ids[:-1]) & (days[1:] == days[:-1])):
        raise ValueError("asof csv: duplicate (security_id, available_at)")
    return ids, days, values


def finra_field(name: str, finra: Path, role: Role, output: Path, schedule, budget: Budget):
    spec = FIELDS[name]
    path = finra / "asof" / spec["source_file"]
    captured = identity(path)
    blob = path.read_bytes()
    digest = sha_bytes(blob)
    receipt_bytes = (finra / "asof" / "manifest.json").read_bytes()
    receipt = json.loads(receipt_bytes)
    pinned = receipt.get("outputs", {}).get(name, {}).get("sha256")
    if pinned != digest:
        raise ValueError(f"{name}: CSV bytes do not match the as-of producer receipt (asof/manifest.json)")
    budget.admit(len(blob) * 6, f"{name}-parse")
    ids, days, values = parse_asof_csv(blob)
    del blob
    dissemination, vintage_cutoff, schedule_source = schedule
    if not np.all(np.isin(days, dissemination)):
        raise ValueError(f"{name}: an available_at is not an official FINRA dissemination date")
    sealed = days >= day_of(SEAL)
    rows_total, rows_sealed = len(ids), int(np.count_nonzero(sealed))
    ids, days, values = ids[~sealed], days[~sealed], values[~sealed]
    pos, on = role.columns_of(ids)
    rows_matched = int(np.count_nonzero(on))
    col, days, values = pos[on].astype(np.int64), days[on], values[on]
    key = (col << 32) | days  # rows are sorted by (id, day) and the axis is sorted: keys ascend
    if np.any(np.diff(key) <= 0):
        raise ValueError(f"{name}: internal as-of key order violated")
    # Sentinel -1 (column -1) keeps every lookup in bounds and is never visible.
    key, values = np.concatenate(([-1], key)), np.concatenate(([np.nan], values))
    del ids, pos, on, col
    budget.report(f"{name}-parsed", rows=rows_total, matched=rows_matched, sealed=rows_sealed)
    writer = FieldWriter(output, name, role)
    columns = np.arange(role.n, dtype=np.int64)
    vintage_cells, last_republished = 0, -1
    for t in range(role.n_dates):
        d = int(role.days[t])
        found = np.searchsorted(key, (columns << 32) | d, side="left") - 1  # last key < (col, d): strict
        visible = (key[found] >> 32) == columns
        src_day = key[found] & 0xFFFFFFFF
        safe = found
        visible &= (d - src_day) <= FINRA_MAX_STALE_DAYS
        row = np.where(visible, values[safe], np.nan)
        writer.write(row)
        if vintage_cutoff is not None:
            republished = visible & (src_day <= vintage_cutoff)
            vintage_cells += int(np.count_nonzero(republished & np.isfinite(row) & (role.member[t] != 0)))
            if republished.any():
                last_republished = t
        if t % 256 == 0:
            budget.check(f"{name}-join")
    writer.close()
    if identity(path) != captured:
        raise ValueError(f"{name}: source changed while reading")
    coverage = writer.coverage()
    coverage["vintage_risk"] = {
        "rule": f"visible row disseminated on or before {date_of(vintage_cutoff) if vintage_cutoff is not None else None} (settlement before {FINRA_REPUBLICATION_SETTLEMENT_BEFORE.isoformat()}): later FINRA republication",
        "finite_member_cells": vintage_cells,
        # Any cell (member or not, NaN value or not) whose visible row is republished counts here.
        "last_session_with_republished_visible_cell": date_of(role.days[last_republished]) if last_republished >= 0 else None,
        "first_session_vintage_safe": (date_of(role.days[last_republished + 1]) if last_republished + 1 < role.n_dates else None)}
    extra = {"vintage_safe_from": date_of(vintage_cutoff + 1) if vintage_cutoff is not None else None}
    sources = [{"path": str(path.resolve()), "bytes": captured[2], "sha256": digest},
               {"path": str((finra / "asof" / "manifest.json").resolve()), "bytes": len(receipt_bytes), "sha256": sha_bytes(receipt_bytes)},
               schedule_source]
    stats = {"rows_total": rows_total, "rows_available_on_or_after_2025_dropped": rows_sealed,
             "rows_matched_axis": rows_matched, "rows_ignored_unknown_id": rows_total - rows_sealed - rows_matched,
             "max_stale_days": FINRA_MAX_STALE_DAYS}
    return writer, sources, coverage, stats, extra


# ---------------------------------------------------------------------------
# TickerHistory3 vendor rows
# ---------------------------------------------------------------------------

def factor_breaks(crf: np.ndarray, raw: np.ndarray, days: np.ndarray, budget: Budget) -> dict:
    """Rule factor-break-v1 (``FB_STATEMENT``) over vendor observations on one date axis.

    ``crf`` (f64) and ``raw`` (f32) are rows x lines, NaN where the row is not an observation. Returns
    the jump-cell count per row, the mass rows, per-mass-row class counts, and every crossing step with
    |s| > noise as arrays (line, step-end day t, k, action). A step crossing several mass rows is listed
    once, under the first (its class depends only on (p, t)). Each decision reads rows <= t only."""
    n_rows, n = crf.shape
    jump = np.zeros(n_rows, dtype=np.int64)
    last = np.full(n, -1, dtype=np.int64)
    lf, lr = np.full(n, np.nan), np.full(n, np.nan)
    with np.errstate(divide="ignore", invalid="ignore"):
        for e in range(n_rows):
            f = crf[e]
            pt = np.isfinite(f)
            if pt.any():
                rw = raw[e].astype(np.float64)
                step = pt & (last >= 0) & ((days[e] - days[np.maximum(last, 0)]) <= FB_MAX_GAP_DAYS)
                s = np.log(f) - np.log(lf)
                r = np.log(rw) - np.log(lr)
                jump[e] = np.count_nonzero(step & (np.abs(s) > FB_CELL_STEP) & (np.abs(r + s) > np.abs(r) + FB_CELL_EXCESS))
                last[pt] = e
                lf[pt], lr[pt] = f[pt], rw[pt]
            if e % 256 == 0:
                budget.check("factor-break-scan")
    mass = [int(b) for b in np.flatnonzero(jump >= FB_MASS_MIN_CELLS)]
    obs = np.isfinite(crf)
    split = math.log(FB_SPLIT_RATIO)
    parts, sessions, seen = [], [], np.empty(0, dtype=np.int64)
    for b in mass:
        before, after = obs[:b], obs[b:]
        j = np.flatnonzero(before.any(axis=0) & after.any(axis=0))
        p = (b - 1) - np.argmax(before[::-1], axis=0)[j]
        t = b + np.argmax(after, axis=0)[j]
        fp, ft = crf[p, j], crf[t, j]
        rp, rt = raw[p, j].astype(np.float64), raw[t, j].astype(np.float64)
        s = np.log(ft) - np.log(fp)
        r = np.log(rt) - np.log(rp)
        half = np.abs(s) / 2
        follow = ((s < 0) & (r >= np.maximum(half, split))) | ((s > 0) & (-r >= np.maximum(half, split)))
        action = np.where(np.abs(s) <= FB_NOISE, 0,
                 np.where((days[t] - days[p]) > FB_MAX_GAP_DAYS, FB_KEPT_GAP,
                 np.where(follow, FB_KEPT_FOLLOW,
                 np.where((s > 0) & (s < split), FB_KEPT_DIST, FB_REPAIRED))))
        key = j.astype(np.int64) * n_rows + t
        keep = (action != 0) & ~np.isin(key, seen)
        seen = np.concatenate((seen, key[keep]))
        parts.append((j[keep], days[t[keep]], ft[keep] / fp[keep], action[keep]))
        sessions.append({"row": b, "jump_cells": int(jump[b]), "crossing_steps": int(np.count_nonzero(keep)),
                         **{name: int(np.count_nonzero(action[keep] == code)) for code, name in FB_ACTIONS.items()}})
        budget.check("factor-break-steps")
    del obs
    cat = lambda i, dtype: (np.concatenate([x[i] for x in parts]).astype(dtype) if parts else np.empty(0, dtype))
    quiet = np.where(jump < FB_MASS_MIN_CELLS, jump, 0)
    top = int(np.argmax(quiet)) if len(quiet) else 0
    return {"jump": jump, "mass": mass, "sessions": sessions,
            "j": cat(0, np.int64), "t_day": cat(1, np.int64), "k": cat(2, np.float64), "action": cat(3, np.int64),
            "max_non_mass": int(quiet[top]) if len(quiet) else 0,
            "max_non_mass_row": top if len(quiet) and quiet[top] > 0 else None}


def tickerhistory_fields(names, th: Path, role: Role, output: Path, budget: Budget):
    iv_names = [n for n in names if FIELDS[n]["column"].startswith("atmCenI_")]
    want_earn, want_shares = "earn_recent" in names, "shares_out" in names
    captured = identity(th)
    pf = pq.ParquetFile(th, memory_map=False)
    columns = ["tradingDate", "securityID"] + [FIELDS[n]["column"] for n in iv_names]
    columns += ["earnFlag"] if want_earn else []
    columns += ["shares", "cumulReturnFactor", "close", "volume"] if want_shares else []
    schema = pf.schema_arrow
    for c in columns:
        if schema.field(c).type != TH_TYPES[c]:
            raise ValueError(f"TickerHistory column {c} is {schema.field(c).type}, expected {TH_TYPES[c]}")
    budget.report("th-hash-start", bytes=captured[2])
    digest = sha_file(th, budget)
    if role.source_sha256 is None or digest != role.source_sha256:
        raise ValueError("TickerHistory SHA-256 differs from the role's source_sha256 (a different vendor file)")
    if identity(th) != captured:
        raise ValueError("TickerHistory changed during hashing")
    budget.report("th-hash-complete", sha256=digest)
    n, nd = role.n, role.n_dates
    first, last = int(role.days[0]), int(role.days[-1])
    pre = SHARES_LAG_DAYS + SHARES_MAX_AGE_DAYS if want_shares else 0
    # Extended date axis: calendar days before the role (shares lag only) then the role sessions.
    ext_days = np.concatenate((np.arange(first - pre, first, dtype=np.int64), role.days))
    n_ext = len(ext_days)
    planned = n_ext * n * 2 + nd * n * (4 * len(iv_names) + (1 if want_earn else 0))
    planned += n_ext * n * (8 + 8 + 4 + 1) if want_shares else 0  # q, crf, raw, factor-break observation mask
    budget.admit(planned + (64 << 20), "tickerhistory-matrices")
    counts = np.zeros((n_ext, n), dtype=np.uint16)
    iv = {k: np.full((nd, n), np.nan, dtype=np.float32) for k in iv_names}
    earn = np.full((nd, n), -1, dtype=np.int8) if want_earn else None
    # shares/cumulReturnFactor (file basis), and the observation's factor and raw close (NaN: no observation)
    q = np.full((n_ext, n), np.nan) if want_shares else None
    crf = np.full((n_ext, n), np.nan) if want_shares else None
    raw = np.full((n_ext, n), np.nan, dtype=np.float32) if want_shares else None
    never = np.iinfo(np.int64).max
    first_above = np.full(n, never, dtype=np.int64)  # date of the line's first row above the A9 ceiling
    st = {"rows_scanned": 0, "rows_on_or_after_2025_skipped": 0, "rows_selected": 0, "rows_off_role_calendar": 0,
          "earnflag_null": 0, "earnflag_unexpected": 0, "iv_nonpositive_or_nonfinite": 0,
          "shares_rows_above_a9_ceiling": 0}
    for b, batch in enumerate(pf.iter_batches(batch_size=65536, columns=columns, use_threads=False)):
        budget.check("th-batch")
        st["rows_scanned"] += batch.num_rows
        d = pc.fill_null(batch.column("tradingDate").cast(pa.int32()), -1).to_numpy().astype(np.int64)
        st["rows_on_or_after_2025_skipped"] += int(np.count_nonzero(d >= day_of(SEAL)))
        idx = np.flatnonzero((d >= first - pre) & (d <= last))
        if not len(idx):
            continue
        sid = pc.fill_null(batch.column("securityID"), 0).to_numpy()[idx]
        pos, on = role.columns_of(sid)
        idx, j, dd = idx[on], pos[on], d[idx][on]
        e = np.minimum(np.searchsorted(ext_days, dd), n_ext - 1)
        cal = ext_days[e] == dd
        st["rows_off_role_calendar"] += int(np.count_nonzero(~cal))
        idx, j, e = idx[cal], j[cal], e[cal]
        if not len(idx):
            continue
        st["rows_selected"] += len(idx)
        np.add.at(counts, (e, j), 1)
        sub = batch.take(pa.array(idx, type=pa.int64()))
        role_rows = e >= pre
        t, jr = e[role_rows] - pre, j[role_rows]
        for k in iv_names:
            v = sub.column(FIELDS[k]["column"]).to_numpy(zero_copy_only=False).astype(np.float32)[role_rows]
            bad = ~(np.isfinite(v) & (v > 0))
            st["iv_nonpositive_or_nonfinite"] += int(np.count_nonzero(bad & ~np.isnan(v)))
            iv[k][t, jr] = v  # raw vendor value (null -> NaN); the declared domain is applied per cell at write
        if want_earn:
            flag = sub.column("earnFlag").filter(pa.array(role_rows))
            code = np.full(len(flag), -1, dtype=np.int8)
            known = np.zeros(len(flag), dtype=bool)
            for text, value in EARN_CODES.items():
                hit = pc.fill_null(pc.equal(flag, text), False).to_numpy(zero_copy_only=False)
                code[hit] = value
                known |= hit
            null = flag.is_null().to_numpy(zero_copy_only=False)
            st["earnflag_null"] += int(np.count_nonzero(null))
            st["earnflag_unexpected"] += int(np.count_nonzero(~known & ~null))
            earn[t, jr] = code
        if want_shares:
            shares = pc.fill_null(sub.column("shares"), 0).to_numpy().astype(np.float64)
            factor = sub.column("cumulReturnFactor").to_numpy(zero_copy_only=False).astype(np.float64)
            above = shares > THOUSANDS_ROW_CEILING
            st["shares_rows_above_a9_ceiling"] += int(np.count_nonzero(above))
            np.minimum.at(first_above, j[above], ext_days[e[above]])
            close = sub.column("close").to_numpy(zero_copy_only=False).astype(np.float32)
            volume = sub.column("volume").to_numpy(zero_copy_only=False).astype(np.float64)
            # An observation meets the role's present contract (prepare_recent_research.py projection).
            obs = (np.isfinite(factor) & (factor > 0) & np.isfinite(close) & (close > 0)
                   & np.isfinite(volume) & (volume >= 0))
            with np.errstate(divide="ignore", invalid="ignore"):
                q[e, j] = np.where((shares > 0) & ~above & obs, shares / factor, np.nan)
            crf[e, j] = np.where(obs, factor, np.nan)
            raw[e, j] = np.where(obs, close, np.float32(np.nan))
        if b % 64 == 0:
            budget.report("th-batch", batch=b, rows_scanned=st["rows_scanned"], rows_selected=st["rows_selected"])
    if identity(th) != captured:
        raise ValueError("TickerHistory changed while reading")
    # All positive duplicate (date, securityID) keys are quarantined, never picked.
    dup = counts > 1
    st["duplicate_keys_quarantined"] = int(np.count_nonzero(dup))
    dup_role = dup[pre:]
    for k in iv_names:
        iv[k][dup_role] = np.nan
    if want_earn:
        earn[dup_role] = -1
    fb = None
    if want_shares:
        q[dup] = np.nan
        crf[dup] = np.nan
        raw[dup] = np.nan
        st["shares_lines_withheld_c81"] = int(np.count_nonzero(first_above != never))
    del counts, dup, dup_role
    if want_shares:
        fb = factor_breaks(crf, raw, ext_days, budget)
        del raw
        # The role's close must carry the same repair: a mass session strictly inside the role (a return
        # across it is in the role) must be exactly the set the role's factor-break-v1 repair block lists.
        inside = sorted(date_of(ext_days[b]) for b in fb["mass"] if first < int(ext_days[b]) <= last)
        listed = sorted(str(x.get("session")) for x in (role.manifest.get("repair") or {}).get("mass_sessions", []))
        if inside != listed:
            raise ValueError(f"{FB_RULE}: mass sessions inside the role {inside} differ from the role manifest repair "
                             f"block {listed} (bind the factor-break-v1 repaired role)")
        budget.report("factor-break", mass_sessions=[date_of(ext_days[b]) for b in fb["mass"]],
                      crossing_steps=int(len(fb["j"])))
    source = [{"path": str(th.resolve()), "bytes": captured[2], "sha256": digest,
               "row_groups": pf.metadata.num_row_groups, "rows": pf.metadata.num_rows}]
    results, extras = {}, {}
    for k in iv_names:
        lo, hi = (np.float32(x) for x in FIELDS[k]["domain"])
        c = {"below_min": 0, "above_max": 0, "member_below_min": 0, "member_above_max": 0}
        w = FieldWriter(output, k, role)
        for t in range(nd):
            v = iv[k][t]
            seen = ~np.isnan(v)
            low, high = seen & ~(v >= lo), seen & (v > hi)  # float32 comparison; -inf low, +inf high
            member = role.member[t] != 0
            c["below_min"] += int(np.count_nonzero(low))
            c["above_max"] += int(np.count_nonzero(high))
            c["member_below_min"] += int(np.count_nonzero(low & member))
            c["member_above_max"] += int(np.count_nonzero(high & member))
            w.write(np.where(low | high, np.nan, v.astype(np.float64)))
        w.close()
        results[k] = w
        extras[k] = {"plausibility": {
            "min": FIELDS[k]["domain"][0], "max": FIELDS[k]["domain"][1], "inclusive": True,
            "units": "annualized decimal", "rule": IV_DOMAIN_RULE,
            "implausible_to_nan": c["below_min"] + c["above_max"],
            "implausible_to_nan_member": c["member_below_min"] + c["member_above_max"], **c}}
        del iv[k]
        budget.check(f"{k}-write")
    if want_earn:
        w = FieldWriter(output, "earn_recent", role)
        for t in range(nd):
            w.write(np.where(earn[t] < 0, np.nan, earn[t].astype(np.float64)))
        w.close()
        results["earn_recent"] = w
        del earn
    if want_shares:
        w = FieldWriter(output, "shares_out", role)
        last_valid = np.full(n, -1, dtype=np.int64)
        cols = np.arange(n)
        p = 0
        withheld_cells = 0
        rep, gap = fb["action"] == FB_REPAIRED, fb["action"] == FB_KEPT_GAP
        rj, rt, rk = fb["j"][rep], fb["t_day"][rep], fb["k"][rep]
        gj, gt = fb["j"][gap], fb["t_day"][gap]
        lo, hi = SHARES_OUT_DOMAIN
        c = {"below_min": 0, "above_max": 0, "member_below_min": 0, "member_above_max": 0,
             "restated": 0, "restated_member": 0, "gap": 0, "gap_member": 0,
             "published": 0, "published_member": 0,
             "turnover": 0, "turnover_member": 0, "turnover_not_evaluable": 0, "turnover_also_si": 0,
             "si": 0, "si_member": 0, "si_not_evaluable": 0}
        # Units rules (a) and (b) read the role's volume/close/raw/present rows and the si_shares field this
        # run already published, both in session order: row t uses rows <= t only.
        si_path = output / "si_shares.f64"
        if si_path.stat().st_size != nd * n * 8:
            raise ValueError("shares_out: this run's si_shares.f64 has the wrong size")
        window = np.full((SHARES_TURNOVER_WINDOW, n), np.nan)  # u = volume / f, NaN where not present
        stream = RoleRows(role, ("volume.f64", "close.f64", "raw_close.f64", "present.u8"))
        try:
            with si_path.open("rb") as si_file:
                for t in range(nd):
                    day = int(role.days[t])
                    lag = day - SHARES_LAG_DAYS
                    while p < n_ext and ext_days[p] <= lag:
                        last_valid[np.isfinite(q[p])] = p
                        p += 1
                    src = np.maximum(last_valid, 0)
                    fresh = (last_valid >= 0) & (ext_days[src] >= lag - SHARES_MAX_AGE_DAYS)
                    lag_day = ext_days[src]
                    # factor-break-v1: divide out every repaired step (p, t, k) with lag < t <= session; a
                    # kept_gap step in that window leaves the ratio ambiguous. Both are known at t <= the session.
                    corr = np.ones(n)
                    hit = fresh[rj] & (lag_day[rj] < rt) & (rt <= day)
                    np.multiply.at(corr, rj[hit], rk[hit])
                    ambiguous = np.zeros(n, dtype=bool)
                    ambiguous[gj[fresh[gj] & (lag_day[gj] < gt) & (gt <= day)]] = True
                    with np.errstate(invalid="ignore"):
                        row = np.where(fresh, q[src, cols] * crf[pre + t] * SHARES_UNIT / corr, np.nan)
                    member = role.member[t] != 0
                    amb = ambiguous & np.isfinite(row)
                    c["gap"] += int(np.count_nonzero(amb))
                    c["gap_member"] += int(np.count_nonzero(amb & member))
                    row[ambiguous] = np.nan
                    # C-81, point in time: withheld once an above-ceiling row dated <= the session is known.
                    withheld = first_above <= day
                    row[withheld] = np.nan
                    withheld_cells += int(np.count_nonzero(withheld))
                    finite = np.isfinite(row)
                    low, high = finite & (row < lo), finite & (row > hi)
                    c["below_min"] += int(np.count_nonzero(low))
                    c["above_max"] += int(np.count_nonzero(high))
                    c["member_below_min"] += int(np.count_nonzero(low & member))
                    c["member_above_max"] += int(np.count_nonzero(high & member))
                    row[low | high] = np.nan
                    # Cells the factor-break correction restated (counted before the units rules: fields-v2 basis).
                    restated = np.isfinite(row) & (corr != 1.0)
                    c["restated"] += int(np.count_nonzero(restated))
                    c["restated_member"] += int(np.count_nonzero(restated & member))
                    # (a) turnover: trailing 21-session median daily volume in session t's share basis.
                    volume, close = stream.row("volume.f64"), stream.row("close.f64")
                    raw_close, present = stream.row("raw_close.f64"), stream.row("present.u8") != 0
                    with np.errstate(divide="ignore", invalid="ignore"):
                        f = np.where(present & (close > 0) & (raw_close > 0), close / raw_close, np.nan)
                        window[t % SHARES_TURNOVER_WINDOW] = np.where(
                            np.isfinite(f) & (f > 0) & np.isfinite(volume) & (volume >= 0), volume / f, np.nan)
                    median, seen = column_median(window)
                    median_volume = median * f
                    finite = np.isfinite(row)
                    evaluable = (seen >= SHARES_TURNOVER_MIN_OBS) & np.isfinite(median_volume)
                    c["turnover_not_evaluable"] += int(np.count_nonzero(finite & ~evaluable))
                    turnover = finite & evaluable & (median_volume > SHARES_TURNOVER_MAX * row)
                    # (b) short interest visible at t (this run's si_shares row t) above 5.0 x shares_out.
                    blob = si_file.read(n * 8)
                    if len(blob) != n * 8:
                        raise ValueError("shares_out: this run's si_shares.f64 is truncated")
                    si = np.frombuffer(blob, dtype="<f8")
                    c["si_not_evaluable"] += int(np.count_nonzero(finite & ~np.isfinite(si)))
                    with np.errstate(invalid="ignore"):
                        si_high = finite & np.isfinite(si) & (si / row > SHARES_SI_RATIO_MAX)
                    c["turnover"] += int(np.count_nonzero(turnover))
                    c["turnover_member"] += int(np.count_nonzero(turnover & member))
                    c["turnover_also_si"] += int(np.count_nonzero(turnover & si_high))
                    si_only = si_high & ~turnover
                    c["si"] += int(np.count_nonzero(si_only))
                    c["si_member"] += int(np.count_nonzero(si_only & member))
                    row[turnover | si_high] = np.nan
                    published = restated & np.isfinite(row)
                    c["published"] += int(np.count_nonzero(published))
                    c["published_member"] += int(np.count_nonzero(published & member))
                    w.write(row)
                    if t % 256 == 0:
                        budget.check("shares_out-write")
            role_inputs = stream.verify()
        except BaseException:
            w.f.close()  # refused: the partial file stays unpublished (no manifest), but its handle is released
            raise
        finally:
            stream.close()
        w.close()
        results["shares_out"] = w
        st["shares_cells_withheld_c81"] = withheld_cells
        quiet = fb["max_non_mass_row"]
        extras["shares_out"] = {
            "plausibility": {
                "min": lo, "max": hi, "inclusive": True, "units": "shares",
                "rule": "restated value v kept iff 1e5 <= v <= 5e10 (float64); any other finite value -> NaN, never clamped. "
                        "Counts are role cells after the C-81 and factor-break rules. Range declared by the root controller "
                        "for T10 (swap-fin-v1 borrow tiers) and applied in T6 fix round 2. Then the units rules "
                        "(turnover, si_ratio; T6 fix round 3). implausible_to_nan counts every rule",
                "implausible_to_nan": c["below_min"] + c["above_max"] + c["turnover"] + c["si"],
                "implausible_to_nan_member": (c["member_below_min"] + c["member_above_max"]
                                              + c["turnover_member"] + c["si_member"]),
                **{k: c[k] for k in ("below_min", "above_max", "member_below_min", "member_above_max")},
                "units_rule": SHARES_UNITS_RULE,
                "turnover": {"window_sessions": SHARES_TURNOVER_WINDOW, "min_present_sessions": SHARES_TURNOVER_MIN_OBS,
                             "max_median_volume_over_shares_out": SHARES_TURNOVER_MAX,
                             "volume_basis": "role volume.f64 (raw-share-volume) restated to the session's share basis by the role close/raw factor ratio",
                             "to_nan": c["turnover"], "to_nan_member": c["turnover_member"],
                             "also_above_si_ratio": c["turnover_also_si"],
                             "not_evaluable_cells": c["turnover_not_evaluable"], "inputs": role_inputs},
                "si_ratio": {"max_si_shares_over_shares_out": SHARES_SI_RATIO_MAX,
                             "si_shares": "this run's si_shares field (visible at the session: strict available_at < session, 45-day staleness)",
                             "to_nan": c["si"], "to_nan_member": c["si_member"],
                             "counting": "cells not already set to NaN by the turnover rule",
                             "not_evaluable_cells": c["si_not_evaluable"],
                             "not_evaluable_counting": "cells finite after the domain (the turnover rule's population) whose si_shares is NaN at the session (no visible FINRA row, or older than 45 days)"}},
            "factor_break": {
                "rule": FB_RULE, "ported_from": "atx-impl/tools/repair_role_factor_breaks.py (T12): same parameters and step classification",
                "statement": FB_STATEMENT, "parameters": FB_PARAMETERS,
                "use": "restatement ratio cumulReturnFactor(session)/cumulReturnFactor(lag) divided by k of every repaired step "
                       "with lag < t <= session; a kept_gap step in that window -> NaN",
                "mass_sessions": [{"session": date_of(ext_days[x["row"]]),
                                   "inside_role": bool(first < int(ext_days[x["row"]]) <= last),
                                   **{k: v for k, v in x.items() if k != "row"}} for x in fb["sessions"]],
                "role_repair_mass_sessions": listed,
                "max_non_mass_jump_cells": fb["max_non_mass"],
                "max_non_mass_session": date_of(ext_days[quiet]) if quiet is not None else None,
                "restated_cells": c["restated"], "restated_member_cells": c["restated_member"],
                "restated_counting": "restated_cells: finite cells a repaired step was divided out of, counted after the gap, C-81 "
                                     "and domain rules and BEFORE the units rules (the fields-v2 basis); restated_published_cells: "
                                     "those still finite after the units rules",
                "restated_published_cells": c["published"], "restated_published_member_cells": c["published_member"],
                "gap_ambiguous_to_nan_cells": c["gap"], "gap_ambiguous_to_nan_member_cells": c["gap_member"]}}
    return results, source, st, digest, extras


# ---------------------------------------------------------------------------
# Research lake (spine_monthly, line_types)
# ---------------------------------------------------------------------------

def lake_file(lake: Path, listed: dict, rel: str):
    entry = listed.get(rel)
    if entry is None:
        raise ValueError(f"lake file {rel} is not listed in the lake manifest")
    blob = (lake / rel).read_bytes()
    digest = sha_bytes(blob)
    if len(blob) != entry["bytes"] or digest != entry["sha256"]:
        raise ValueError(f"lake file {rel} does not match the lake manifest (being rewritten?)")
    table = pq.read_table(pa.BufferReader(blob))
    return table, {"path": str((lake / rel).resolve()), "bytes": len(blob), "sha256": digest}


def line_ids(role: Role, line_id):
    line_id = line_id.combine_chunks() if isinstance(line_id, pa.ChunkedArray) else line_id
    if line_id.null_count or not pc.all(pc.starts_with(line_id, LINE_PREFIX)).as_py():
        raise ValueError("lake line_id is not TBLTICKERHISTORY-<securityID>")
    digits = pc.utf8_slice_codeunits(line_id, len(LINE_PREFIX))
    if not pc.all(pc.match_substring_regex(digits, r"^[0-9]+$")).as_py():
        raise ValueError("lake line_id suffix is not a securityID")
    sid = pc.cast(digits, pa.int64()).to_numpy()
    pos, on = role.columns_of(sid)
    return sid, pos, on


def lake_fields(names, lake: Path, role: Role, output: Path, budget: Budget):
    manifest_path = lake / "_manifest.json"
    manifest_bytes = manifest_path.read_bytes()
    m = json.loads(manifest_bytes)
    listed = {f["path"]: f for ds in ("spine_monthly", "line_types") for f in m["datasets"][ds]["files"]}
    sources = [{"path": str(manifest_path.resolve()), "bytes": len(manifest_bytes), "sha256": sha_bytes(manifest_bytes),
                "snapshot_id": m.get("snapshot_id"), "snapshot_sha256": m.get("snapshot_sha256")}]
    st = {}
    results = {}
    spine_names = [x for x in names if x in ("mktcap_lagged", "size_grp")]
    if spine_names:
        first_year = int(role.years[0]) - 1
        listed_years = sorted(int(p.split("year=")[1].split("/")[0]) for p in listed if p.startswith("spine_monthly/"))
        f_day, f_col, f_me, f_sg = [], [], [], []
        formations = set()
        for year in range(first_year, int(role.years[-1]) + 1):
            if year >= SEAL.year:
                raise rw.SealError(rw.seal_message(f"spine year {year} requested"))
            rel = f"spine_monthly/year={year}/part-0.parquet"
            if rel not in listed:
                if listed_years and year < listed_years[0]:
                    continue
                raise ValueError(f"spine year {year} missing from the lake manifest")
            table, src = lake_file(lake, listed, rel)
            sources.append(src)
            for c, typ in (("formation_date", pa.date32()), ("me_line", pa.float64()), ("size_grp", pa.string()), ("line_id", pa.string())):
                if table.schema.field(c).type != typ:
                    raise ValueError(f"spine column {c} is {table.schema.field(c).type}, expected {typ}")
            fd = pc.fill_null(table.column("formation_date").cast(pa.int32()), -1).to_numpy().astype(np.int64)
            if np.any(fd < 0):
                raise ValueError("spine row without formation_date")
            keep = fd < day_of(SEAL)
            formations.update(np.unique(fd[keep]).tolist())
            _, pos, on = line_ids(role, table.column("line_id"))
            sel = keep & on
            f_day.append(fd[sel])
            f_col.append(pos[sel])
            f_me.append(table.column("me_line").to_numpy(zero_copy_only=False).astype(np.float64)[sel])
            sg_text = table.column("size_grp").combine_chunks().filter(pa.array(sel))
            sg = np.full(len(sg_text), np.nan)
            known = sg_text.is_null().to_numpy(zero_copy_only=False)
            for text, code in SIZE_CODES.items():
                hit = pc.fill_null(pc.equal(sg_text, text), False).to_numpy(zero_copy_only=False)
                sg[hit] = code
                known |= hit
            if not known.all():
                raise ValueError("unexpected spine size_grp value")
            f_sg.append(sg)
            budget.check(f"spine-{year}")
        fdays = np.array(sorted(formations), dtype=np.int64)
        if not len(fdays):
            raise ValueError("the lake has no spine formation for the role's years")
        months =[(EPOCH + dt.timedelta(days=int(x))) for x in fdays]
        if any((b.year * 12 + b.month) - (a.year * 12 + a.month) != 1 for a, b in zip(months, months[1:])):
            raise ValueError("spine formations are not month-contiguous (partial lake?)")
        day_all, col_all = np.concatenate(f_day), np.concatenate(f_col)
        k = np.searchsorted(fdays, day_all)
        flat = k * role.n + col_all
        if len(np.unique(flat)) != len(flat):
            raise ValueError("duplicate spine (formation, line) row")
        me = np.full((len(fdays), role.n), np.nan)
        sgm = np.full((len(fdays), role.n), np.nan)
        me_v = np.concatenate(f_me)
        me[k, col_all] = np.where(np.isfinite(me_v) & (me_v > 0), me_v, np.nan)
        sgm[k, col_all] = np.concatenate(f_sg)
        st["spine_formations"] = len(fdays)
        st["spine_first_formation"] = date_of(fdays[0]) if len(fdays) else None
        st["spine_last_formation"] = date_of(fdays[-1]) if len(fdays) else None
        st["spine_rows_on_axis"] = int(len(flat))
        latest = np.searchsorted(fdays, role.days, side="left") - 1  # latest formation < session
        age = np.where(latest >= 0, role.days - fdays[np.maximum(latest, 0)], 0)
        if len(age) and int(age.max()) > SPINE_MAX_FORMATION_AGE_DAYS:
            t_bad = int(np.argmax(age > SPINE_MAX_FORMATION_AGE_DAYS))
            raise ValueError(f"stale spine formation: session {date_of(role.days[t_bad])} would use the formation of "
                             f"{date_of(fdays[latest[t_bad]])} ({int(age[t_bad])} days > {SPINE_MAX_FORMATION_AGE_DAYS}; partial lake?)")
        st["spine_max_formation_age_days"] = int(age.max()) if len(age) else None
        writers = {x: FieldWriter(output, x, role) for x in spine_names}
        for t in range(role.n_dates):
            f = int(latest[t])
            for x, w in writers.items():
                src = me if x == "mktcap_lagged" else sgm
                w.write(src[f] if f >= 0 else np.full(role.n, np.nan))
        for x, w in writers.items():
            w.close()
            results[x] = w
    if "is_common" in names:
        table, src = lake_file(lake, listed, "line_types/year=0/part-0.parquet")
        sources.append(src)
        if table.schema.field("security_type").type != pa.string():
            raise ValueError("line_types.security_type is not a string")
        sid, pos, on = line_ids(role, table.column("line_id"))
        if len(np.unique(sid)) != len(sid):
            raise ValueError("duplicate line_types line_id")
        stype = table.column("security_type").combine_chunks()
        eligible = pc.fill_null(pc.is_in(stype, value_set=pa.array(list(ELIGIBLE_SECURITY_TYPES))), False).to_numpy(zero_copy_only=False)
        vec = np.full(role.n, np.nan)
        vec[pos[on]] = np.where(eligible[on], 1.0, 0.0)
        types = stype.filter(pa.array(on)).to_pylist()
        st["line_types_security_type_on_axis"] = {str(k): types.count(k) for k in sorted(set(map(str, types)))}
        st["line_types_ids_missing_on_axis"] = int(np.count_nonzero(np.isnan(vec)))
        w = FieldWriter(output, "is_common", role)
        for t in range(role.n_dates):
            w.write(vec)
        w.close()
        results["is_common"] = w
    if sha_bytes(manifest_path.read_bytes()) != sources[0]["sha256"]:
        raise ValueError("lake manifest changed while reading")
    return results, sources, st


# ---------------------------------------------------------------------------
# Role-derived broadcast market return
# ---------------------------------------------------------------------------

def market_return_field(role: Role, output: Path, budget: Budget):
    """Stream close/raw_close/present row by row, hashing exactly the bytes used."""
    n = role.n
    specs = {"close.f64": "<f8", "raw_close.f64": "<f8", "present.u8": "u1"}
    handles, hashes, sources = {}, {}, []
    for name, dtype in specs.items():
        entry = role.manifest["files"][name]
        if entry["bytes"] != role.n_dates * n * np.dtype(dtype).itemsize:
            raise ValueError(f"role {name} size disagrees with the role shape")
        handles[name], hashes[name] = (role.dir / name).open("rb"), hashlib.sha256()

    def row_of(name):
        size = n * np.dtype(specs[name]).itemsize
        blob = handles[name].read(size)
        if len(blob) != size:
            raise ValueError(f"role {name} is truncated")
        hashes[name].update(blob)
        return np.frombuffer(blob, dtype=specs[name])

    writer = FieldWriter(output, "mkt_ret", role)
    counts, guarded_total, prev = [], 0, None
    try:
        for d in range(role.n_dates):
            close, raw, present = row_of("close.f64"), row_of("raw_close.f64"), row_of("present.u8") != 0
            row = np.full(n, np.nan)
            if prev is not None:
                c0, r0, p0 = prev
                with np.errstate(divide="ignore", invalid="ignore", over="ignore"):
                    priced = (p0 & present & np.isfinite(c0) & np.isfinite(close) & np.isfinite(r0) & np.isfinite(raw)
                              & (c0 > 0) & (close > 0) & (r0 > 0) & (raw > 0))
                    r = close / c0 - 1
                    log_return = np.log(close) - np.log(c0)
                    raw_log_return = np.log(raw) - np.log(r0)
                    guarded = priced & (~np.isfinite(r) | (np.abs(log_return) > 1.5)
                                        | (np.abs(log_return) > np.abs(raw_log_return) + .10))
                member = role.member[d - 1] != 0
                use = member & priced & ~guarded
                k = int(np.count_nonzero(use))
                counts.append(k)
                guarded_total += int(np.count_nonzero(member & guarded))
                if k:
                    row[present] = math.fsum(r[use].tolist()) / k
            writer.write(row)
            prev = (close, raw, present)
            if d % 256 == 0:
                budget.check("mkt_ret")
        for name in specs:
            if handles[name].read(1):
                raise ValueError(f"role {name} is longer than the role shape")
    finally:
        for h in handles.values():
            h.close()
    writer.close()
    for name in specs:
        if hashes[name].hexdigest() != role.manifest["files"][name]["sha256"]:
            raise ValueError(f"role {name} bytes do not match the role manifest")
        sources.append({"path": str((role.dir / name).resolve()), "bytes": role.manifest["files"][name]["bytes"],
                        "sha256": hashes[name].hexdigest()})
    sources.append({"path": str((role.dir / "member.u8").resolve()), "bytes": role.manifest["files"]["member.u8"]["bytes"],
                    "sha256": role.manifest["files"]["member.u8"]["sha256"]})
    c = np.array(counts, dtype=np.int64)
    score = c[max(role.score_begin - 1, 0):max(role.score_end - 1, 0)]
    stats = {"contributors_per_session": {
        "sessions": len(c), "min": int(c.min()) if len(c) else None, "median": float(np.median(c)) if len(c) else None,
        "max": int(c.max()) if len(c) else None, "zero_contributor_sessions": int(np.count_nonzero(c == 0)),
        "score_window_min": int(score.min()) if len(score) else None,
        "score_window_median": float(np.median(score)) if len(score) else None},
        "guarded_member_intervals": guarded_total}
    return writer, sources, stats


# ---------------------------------------------------------------------------
# Issuer-level fields: input adapter (T19 bridge, T20 events), links, per-session joins
# ---------------------------------------------------------------------------

# Adapter: the committed input contracts. T19 `atx.identity-bridge/v1` (prepare_identity_bridge.py docstring,
# commit 83fbbf84) and T20 `atx.fundamental-events/v1` (fundamental_events_schema.md, commit 77771dd2). Each
# input is one manifest-listed parquet file, verified against the manifest's bytes and SHA-256 before parsing.
BRIDGE_ADAPTER = {
    "schema": "atx.identity-bridge/v1", "file": "links.parquet",
    "columns": ("sr_id", "cik", "start", "end_incl", "available_at", "primary", "basis"),
}
EVENTS_ADAPTER = {
    "schema": "atx.fundamental-events/v1", "file": "fundamental_events.parquet",
    "columns": ("cik", "accepted_utc", "accession", "clock_basis", "period_end", "staleness_days"),
}
SIC_ADAPTER = {
    "schema": "atx.fundamental-events/v1", "file": "sic_events.parquet",
    "columns": ("cik", "accepted_utc", "accession", "sic"),
}
# The atx-db fundamentals stage's SIC table (platform v7 U2: ``--sic-events``, the SIC source of role universe
# linked-operating-v3 and of the grp_* fields when given). It is read through SIC_ADAPTER's consumer semantics
# (load_events) after this column mapping, so the clock, the tie-break, the seal and the valid-SIC guard are the ones
# of the atx.fundamental-events/v1 table.
SIC_STAGE_ADAPTER = {
    "schema": "atx.alpha-panel.fundamentals/v2", "stage": "fundamentals", "file": "sic_events.parquet",
    "columns": ("cik", "clock_utc", "accession", "sic", "sic_basis", "sic2", "ff12", "ff49"),
    "rename": {"clock_utc": "accepted_utc"},
}
SIC_STAGE_BASES = ("fsds_sub", "carried")
SIC_STAGE_MAPPING = (
    "atx-db stage fundamentals (atx.alpha-panel.fundamentals/v2, rule fund-events-pit-v2) sic_events.parquet, pinned by "
    "the stage manifest SHA-256, read as the atx.fundamental-events/v1 SIC table: cik -> cik; clock_utc (naive UTC: FSDS "
    "SUB accepted_utc, else filed 00:00 UTC + 46 h) -> accepted_utc (the same clock definition); accession -> accession "
    "(tie-break at equal clocks); sic -> sic (the SUB SIC of the CIK's own filing, sic_basis fsds_sub, else the CIK's "
    "latest earlier such SIC carried to this filing's clock, sic_basis carried: both are the SIC in force at the clock, "
    "so both are used). One row per periodic-form filing event (10-K/10-Q/10-KT/10-QT/20-F/40-F and /A) of every Company "
    "Facts CIK. sic2 / ff12 / ff49 are not used: groups come from this builder's own SIC mapping table (the stage "
    "labels are compared with it and the disagreeing rows counted). Consumer rule unchanged: the linked CIK's latest row "
    "(max clock, tie by accession) with clock < the mark of session t-L, age = date(t) - UTC date(clock) <= 550 days; "
    f"rows with clock on or after {rw.SEAL_DATE} and sic outside [100, 9999] are dropped (counted)")
BRIDGE_KINDS = ("P", "J")                              # any other primary value (N, ...) is dropped and counted
BRIDGE_EXCLUDED_BASES = ("current_ticker_verified",)   # T18: starts at the 2026 snapshot, never backfills history
STALENESS_DAYS_ALLOWED = (FUND_STALE_DAYS, FUND_STALE_DAYS_ANNUAL)  # v4-prereg R2: 200 / 400 only


def pinned_manifest(directory: Path, expected_sha256: str, what: str, schema: str):
    blob = (directory / "manifest.json").read_bytes()
    digest = sha_bytes(blob)
    if digest != str(expected_sha256).lower():
        raise ValueError(f"{what} manifest SHA-256 does not match --{what}-sha256")
    m = json.loads(blob)
    if m.get("schema") != schema or m.get("status") != "complete":
        raise ValueError(f"{what} manifest is not a complete {schema} artifact")
    return m, {"path": str((directory / "manifest.json").resolve()), "bytes": len(blob), "sha256": digest}


def read_listed(directory: Path, manifest: dict, adapter: dict, what: str, budget: Budget, extra=()):
    """The adapter's manifest-listed parquet file, hash-verified from the exact bytes parsed; contract columns plus
    `extra` (requested items) must all be present."""
    entry = (manifest.get("files") or {}).get(adapter["file"])
    if not isinstance(entry, dict):
        raise ValueError(f"{what}: manifest does not list {adapter['file']}")
    blob = (directory / adapter["file"]).read_bytes()
    digest = sha_bytes(blob)
    if len(blob) != int(entry.get("bytes", -1)) or digest != entry.get("sha256"):
        raise ValueError(f"{what}: {adapter['file']} does not match its manifest entry")
    names = pq.read_schema(pa.BufferReader(blob)).names
    missing = [c for c in adapter["columns"] if c not in names]
    if missing:
        raise ValueError(f"{what}: {adapter['file']} lacks contract column(s) {missing}")
    missing = [c for c in extra if c not in names]
    if missing:
        raise ValueError(f"{what}: {adapter['file']} lacks requested column(s) {missing}")
    table = pq.read_table(pa.BufferReader(blob), columns=list(adapter["columns"]) + [c for c in extra
                                                                                     if c not in adapter["columns"]])
    budget.check(f"{what}-read")
    return table, [{"path": str((directory / adapter["file"]).resolve()), "bytes": len(blob), "sha256": digest}]


def column_of(table, name: str):
    col = table.column(name)
    return col.combine_chunks() if isinstance(col, pa.ChunkedArray) else col


def as_instants_ns(col, what: str) -> np.ndarray:
    """A timestamp column (any unit; tz-aware is a UTC instant, naive is declared UTC) as i64 ns since the epoch."""
    if not pa.types.is_timestamp(col.type):
        raise ValueError(f"{what} is {col.type}, expected a timestamp (UTC instant)")
    if col.null_count:
        raise ValueError(f"{what} has null values")
    return pc.cast(pc.cast(col, pa.timestamp("ns", tz=col.type.tz)), pa.int64()).to_numpy(zero_copy_only=False)


def as_days(col, what: str, null_day=None) -> np.ndarray:
    if not pa.types.is_date32(col.type):
        raise ValueError(f"{what} is {col.type}, expected date32")
    if col.null_count and null_day is None:
        raise ValueError(f"{what} has null values")
    days = pc.fill_null(pc.cast(col, pa.int32()), 0).to_numpy(zero_copy_only=False).astype(np.int64)
    if col.null_count:
        days = np.where(col.is_null().to_numpy(zero_copy_only=False), null_day, days)
    return days


def as_ids(col, what: str) -> np.ndarray:
    if not pa.types.is_integer(col.type) or col.null_count:
        raise ValueError(f"{what} is not a non-null integer column")
    ids = pc.cast(col, pa.int64()).to_numpy(zero_copy_only=False).astype(np.int64)
    if np.any(ids <= 0):
        raise ValueError(f"{what} is not a positive id")
    return ids


def as_f64(col, what: str) -> np.ndarray:
    if not (pa.types.is_floating(col.type) or pa.types.is_integer(col.type)):
        raise ValueError(f"{what} is {col.type}, expected a number")
    return pc.fill_null(pc.cast(col, pa.float64()), np.nan).to_numpy(zero_copy_only=False).astype(np.float64)


def as_text(col) -> pa.Array:
    return pc.fill_null(pc.cast(col, pa.string()), "")


def load_bridge(directory: Path, expected_sha256: str, role: Role, budget: Budget):
    """T19 identity bridge -> link rows on the role axis (column, CIK, start/end day, available_at ns, P flag)."""
    m, man_src = pinned_manifest(directory, expected_sha256, "identity-bridge", BRIDGE_ADAPTER["schema"])
    if not isinstance(m.get("rehearsal_identity"), bool):
        raise ValueError("identity-bridge manifest carries no boolean rehearsal_identity flag")
    table, sources = read_listed(directory, m, BRIDGE_ADAPTER, "identity-bridge", budget)
    sid = as_ids(column_of(table, "sr_id"), "identity-bridge sr_id")
    cik = as_ids(column_of(table, "cik"), "identity-bridge cik")
    start = as_days(column_of(table, "start"), "identity-bridge start")
    end = as_days(column_of(table, "end_incl"), "identity-bridge end_incl", null_day=OPEN_END_DAY)
    avail = as_instants_ns(column_of(table, "available_at"), "identity-bridge available_at")
    kind = as_text(column_of(table, "primary"))
    is_p = pc.equal(kind, "P").to_numpy(zero_copy_only=False)
    known = pc.is_in(kind, value_set=pa.array(list(BRIDGE_KINDS))).to_numpy(zero_copy_only=False)
    excluded = pc.is_in(as_text(column_of(table, "basis")),
                        value_set=pa.array(list(BRIDGE_EXCLUDED_BASES))).to_numpy(zero_copy_only=False)
    if np.any(start > end):
        raise ValueError("identity-bridge link with start after end_incl")
    sealed = avail >= SEAL_NS
    pos, on = role.columns_of(sid)
    keep = known & ~excluded & ~sealed & on
    late = keep & (avail > start * DAY_NS + MARK_NS)  # T19 invariant: available_at <= start 22:00 UTC
    st = {"rehearsal_identity": m["rehearsal_identity"], "rule": m.get("rule"),
          "scope_complete": (m.get("source") or {}).get("scope_complete"), "rows_total": int(len(sid)),
          "rows_dropped_kind_not_p_or_j": int(np.count_nonzero(~known)),
          "rows_dropped_excluded_basis": int(np.count_nonzero(known & excluded)),
          "rows_available_on_or_after_2025_dropped": int(np.count_nonzero(known & ~excluded & sealed)),
          "rows_ignored_off_axis": int(np.count_nonzero(known & ~excluded & ~sealed & ~on)),
          "rows_used": int(np.count_nonzero(keep)), "rows_used_primary": int(np.count_nonzero(keep & is_p)),
          "rows_used_available_after_start_mark": int(np.count_nonzero(late)),
          "rows_used_available_exactly_at_start_mark": int(np.count_nonzero(keep & (avail == start * DAY_NS + MARK_NS))),
          "link_rule": "start <= date(session) <= end_incl; rows with available_at > start 22:00 UTC (invariant "
                       "violations, counted) are also gated by available_at <= date(session) 22:00 UTC"}
    links = {"col": pos[keep].astype(np.int64), "cik": cik[keep], "start": start[keep], "end": end[keep],
             "avail": avail[keep], "primary": is_p[keep]}
    return links, [man_src] + sources, st


def resolve_links(links: dict, role: Role, marks: np.ndarray, budget: Budget):
    """Session x line matrix of the dense CIK index (-1 unlinked, -2 ambiguous) and a primary flag.

    A row qualifies at session t when start <= date(t) <= end_incl and available_at <= mark(t); the second
    condition never binds on a row meeting T19's invariant (available_at <= start mark). Two qualifying rows of one
    line for different CIKs make the cell ambiguous (T19 guarantees disjoint intervals per line; kept as a guard);
    P and J rows for one CIK make it primary."""
    ciks = np.unique(links["cik"])
    cidx = np.searchsorted(ciks, links["cik"]).astype(np.int32)
    link = np.full((role.n_dates, role.n), -1, dtype=np.int32)
    primary = np.zeros((role.n_dates, role.n), dtype=bool)
    t_lo = np.maximum(np.searchsorted(role.days, links["start"], side="left"),
                      np.searchsorted(marks, links["avail"], side="left"))
    t_hi = np.searchsorted(role.days, links["end"], side="right")
    never = 0
    for k in np.lexsort((links["avail"], links["start"], links["col"])):
        a, b, j, c = int(t_lo[k]), int(t_hi[k]), int(links["col"][k]), cidx[k]
        if a >= b:
            never += 1
            continue
        seg = link[a:b, j]
        free, same = seg == -1, seg == c
        seg[~free & ~same] = -2
        seg[free] = c
        if links["primary"][k]:
            primary[a:b, j] |= free | same
    budget.check("issuer-links")
    st = {"linked_ciks": int(len(ciks)), "rows_never_qualifying_on_role": never,
          "ambiguous_cells": int(np.count_nonzero(link == -2))}
    return ciks, link, primary, st


def load_events(directory: Path, manifest: dict, adapter: dict, items, ciks: np.ndarray, what: str, budget: Budget,
                listed=None):
    """Rows of linked CIKs, sealed rows dropped, ordered by (accepted_utc, accession): the contract's
    latest-row selection (max accepted_utc, tie by accession) is then the last row visible before a mark.
    ``listed``: an already read_listed (table, sources) in the adapter's column names (load_sic_stage)."""
    table, sources = listed if listed is not None else read_listed(directory, manifest, adapter, what, budget,
                                                                   extra=tuple(items))
    cik = as_ids(column_of(table, "cik"), f"{what} cik")
    clock = as_instants_ns(column_of(table, "accepted_utc"), f"{what} accepted_utc")
    accession = as_text(column_of(table, "accession"))
    sealed = clock >= SEAL_NS
    pos = np.minimum(np.searchsorted(ciks, cik), max(len(ciks) - 1, 0))
    linked = (ciks[pos] == cik) if len(ciks) else np.zeros(len(cik), dtype=bool)
    keep = ~sealed & linked
    by_clock = pc.sort_indices(pa.table({"c": clock, "a": accession, "i": np.arange(len(cik))}),
                               sort_keys=[("c", "ascending"), ("a", "ascending"), ("i", "ascending")])
    order = by_clock.to_numpy(zero_copy_only=False).astype(np.int64)
    order = order[keep[order]]
    ev = {"clock": clock[order], "cidx": pos[order].astype(np.int64)}
    st = {"schema": manifest.get("schema"), "values_label": manifest.get("values_label"),
          "rehearsal_identity": manifest.get("rehearsal_identity"), "rows_total": int(len(cik)),
          "rows_available_on_or_after_2025_dropped": int(np.count_nonzero(sealed)),
          "rows_ignored_unlinked_cik": int(np.count_nonzero(~sealed & ~linked)), "rows_used": int(len(order))}
    key = np.lexsort((ev["clock"], ev["cidx"]))
    st["rows_sharing_cik_and_clock"] = int(np.count_nonzero((np.diff(ev["cidx"][key]) == 0)
                                                            & (np.diff(ev["clock"][key]) == 0)))
    if "period_end" in adapter["columns"]:
        pe = as_days(column_of(table, "period_end"), f"{what} period_end", null_day=NULL_PERIOD_DAY)[order]
        ev["period_end"] = pe
        st["rows_null_period_end"] = int(np.count_nonzero(pe == NULL_PERIOD_DAY))
        stale = column_of(table, "staleness_days")
        if not pa.types.is_integer(stale.type) or stale.null_count:
            raise ValueError(f"{what} staleness_days is not a non-null integer column")
        ev["stale_days"] = pc.cast(stale, pa.int64()).to_numpy(zero_copy_only=False).astype(np.int64)[order]
        if np.any(~np.isin(ev["stale_days"], STALENESS_DAYS_ALLOWED)):
            raise ValueError(f"{what} staleness_days outside the declared {list(STALENESS_DAYS_ALLOWED)}")
        st["rows_used_staleness_400"] = int(np.count_nonzero(ev["stale_days"] == FUND_STALE_DAYS_ANNUAL))
        basis = pc.utf8_lower(as_text(column_of(table, "clock_basis")))
        ev["fc1"] = pc.match_substring(basis, "fc1").to_numpy(zero_copy_only=False).astype(bool)[order]
        st["rows_used_fc1_clock"] = int(np.count_nonzero(ev["fc1"]))
    if "sic" in adapter["columns"]:
        sic = as_f64(column_of(table, "sic"), f"{what} sic")[order]
        valid = np.isfinite(sic) & (sic == np.floor(sic)) & (sic >= SIC_RANGE[0]) & (sic <= SIC_RANGE[1])
        st["rows_used_invalid_sic_skipped"] = int(np.count_nonzero(~valid))
        for k in ("clock", "cidx"):
            ev[k] = ev[k][valid]
        ev["sic"] = sic[valid].astype(np.int64)
        st["rows_used"] = int(np.count_nonzero(valid))
    ev["values"] = {x: as_f64(column_of(table, x), f"{what} {x}")[order] for x in items}
    del table
    budget.check(f"{what}-events")
    return ev, sources, st


def reference_classifications():
    """The atx-db SIC classification helpers of this worktree (imported only when SIC groups are needed)."""
    import sys
    src = Path(__file__).resolve().parents[2] / "atx-db" / "src"
    if "atx_db" not in sys.modules and str(src) not in sys.path:
        sys.path.insert(0, str(src))
    from atx_db import reference_classifications as rc  # noqa: PLC0415 - only when group fields are requested
    return rc


def sic_mapping():
    """SIC -> (FF12 number, FF49 number) tables for SIC 0..9999 from the pinned atx-db helpers."""
    rc = reference_classifications()
    versions = {"ff12": rc.FF12_MAPPING_VERSION, "ff49": rc.FF49_MAPPING_VERSION}
    if versions != SIC_MAPPING_VERSIONS:
        raise ValueError(f"SIC mapping versions {versions} differ from the declared {SIC_MAPPING_VERSIONS}")
    ff49_numbers = {code: number for number, code, *_ in rc.FF49_INDUSTRIES}
    ff12, ff49 = np.full(SIC_RANGE[1] + 1, np.nan), np.full(SIC_RANGE[1] + 1, np.nan)
    for sic in range(SIC_RANGE[0], SIC_RANGE[1] + 1):
        ff12[sic] = FF12_NUMBERS[rc.fama_french_12_for_sic(sic)]
        code = rc.fama_french_49_for_sic(sic)
        ff49[sic] = ff49_numbers[code] if code is not None else np.nan
    table = np.stack([ff12, ff49]).astype("<f8")
    provenance = {"module_path": str(Path(rc.__file__).resolve()), **code_identity(Path(rc.__file__)),
                  "versions": versions, "ff12_numbering": FF12_NUMBERS,
                  "table_sha256": sha_bytes(table.tobytes()),
                  "table_rule": "sha256 of the <f8 bytes of [ff12[0..9999], ff49[0..9999]] (NaN outside [100, 9999] or unlisted)"}
    return ff12, ff49, provenance


def sic_stage_dir(path: Path) -> Path:
    """A fundamentals stage directory named directly or through its sic_events.parquet or manifest.json."""
    path = Path(path)
    return path.parent if path.name in (SIC_STAGE_ADAPTER["file"], "manifest.json") else path


def load_sic_stage(directory: Path, expected_sha256: str, ciks: np.ndarray, budget: Budget, what: str = "sic-events"):
    """``SIC_STAGE_MAPPING``: the atx-db fundamentals stage's SIC table as load_events' SIC events of the linked CIKs
    ``ciks`` (the same arrays, clock and guards as an atx.fundamental-events/v1 sic_events.parquet). Returns (events,
    [stage manifest source, file source], checks)."""
    directory = Path(directory)
    m, man_src = pinned_manifest(directory, expected_sha256, what, SIC_STAGE_ADAPTER["schema"])
    if m.get("stage") != SIC_STAGE_ADAPTER["stage"]:
        raise ValueError(f"{what}: stage {m.get('stage')!r} is not {SIC_STAGE_ADAPTER['stage']!r}")
    table, sources = read_listed(directory, m, SIC_STAGE_ADAPTER, what, budget)
    basis = np.asarray(as_text(column_of(table, "sic_basis")).to_pylist(), dtype=object)
    odd = sorted(set(basis.tolist()) - set(SIC_STAGE_BASES))
    if odd:
        raise ValueError(f"{what}: sic_basis values {odd} outside {list(SIC_STAGE_BASES)}")
    cik = as_ids(column_of(table, "cik"), f"{what} cik")
    clock = as_instants_ns(column_of(table, "clock_utc"), f"{what} clock_utc")
    sic = as_f64(column_of(table, "sic"), f"{what} sic")
    valid = np.isfinite(sic) & (sic == np.floor(sic)) & (sic >= SIC_RANGE[0]) & (sic <= SIC_RANGE[1])
    pos = np.minimum(np.searchsorted(ciks, cik), max(len(ciks) - 1, 0))
    linked = (ciks[pos] == cik) if len(ciks) else np.zeros(len(cik), dtype=bool)
    used = linked & (clock < SEAL_NS) & valid
    # the stage's own labels are never used; count the rows whose labels differ from this builder's mapping
    rc = reference_classifications()
    labels = pa.table({c: column_of(table, c) for c in ("sic", "sic2", "ff12", "ff49")}).group_by(
        ["sic", "sic2", "ff12", "ff49"]).aggregate([("sic", "count")]).to_pylist()
    differ = 0
    for row in labels:
        s = row["sic"]
        if s is not None and SIC_RANGE[0] <= s <= SIC_RANGE[1] and (row["sic2"], row["ff12"], row["ff49"]) != (
                s // 100, rc.fama_french_12_for_sic(s), rc.fama_french_49_for_sic(s)):
            differ += row["sic_count"]
    renamed = table.rename_columns([SIC_STAGE_ADAPTER["rename"].get(c, c) for c in table.column_names])
    del table
    ev, _, st = load_events(directory, m, SIC_ADAPTER, [], ciks, what, budget, listed=(renamed, sources))
    if st["rows_used"] != int(np.count_nonzero(used)):
        raise ValueError(f"{what}: stage row accounting differs from the SIC event arrays")
    st.update({"stage": m.get("stage"), "stage_rule": m.get("rule"), "stage_code_version": m.get("code_version"),
               "column_mapping": SIC_STAGE_MAPPING,
               "rows_by_sic_basis": {b: int(np.count_nonzero(basis == b)) for b in SIC_STAGE_BASES},
               "rows_used_carried": int(np.count_nonzero(used & (basis == "carried"))),
               "rows_stage_labels_differ_from_builder_mapping": int(differ)})
    return ev, [man_src] + sources, st


def advance(ev: dict, latest: np.ndarray, p: int, mark: int) -> int:
    """Move the clock-ordered pointer to every row with clock < mark; latest[c] = the last such row of CIK c."""
    p2 = int(np.searchsorted(ev["clock"], mark, side="left"))
    if p2 > p:
        np.maximum.at(latest, ev["cidx"][p:p2], np.arange(p, p2, dtype=np.int64))
    return p2


def issuer_fields(names, role: Role, output: Path, budget: Budget, *, bridge: Path, bridge_sha256: str,
                  events: Path | None, events_sha256: str | None, lag: int, sic_events: Path | None = None,
                  sic_events_sha256: str | None = None):
    """``sic_events`` (the atx-db fundamentals stage, ``SIC_STAGE_MAPPING``) replaces the events artifact's SIC table
    for the grp_* fields; without it every output byte is the events artifact's."""
    nd, n = role.n_dates, role.n
    fund_names = [x for x in names if ISSUER_FIELDS[x]["kind"] == "fund"]
    grp_names = [x for x in names if ISSUER_FIELDS[x]["kind"] == "grp"]
    want_me = "me_company" in names
    budget.admit(nd * n * 5 + (64 << 20), "issuer-link-matrix")
    links, bridge_sources, bridge_st = load_bridge(bridge, bridge_sha256, role, budget)
    marks = role.days * DAY_NS + MARK_NS
    ciks, link, primary, link_st = resolve_links(links, role, marks, budget)
    del links
    st = {"fund_lag_sessions": lag, "identity_bridge": {**bridge_st, **link_st}}
    fund = sic = None
    sources = {x: list(bridge_sources) for x in names}
    extras = {}
    if fund_names or (grp_names and sic_events is None):
        m, man_src = pinned_manifest(events, events_sha256, "fund-events", EVENTS_ADAPTER["schema"])
        st["fund_events_manifest"] = {"sha256": man_src["sha256"], "values_label": m.get("values_label"),
                                      "rehearsal_identity": m.get("rehearsal_identity"), "items": m.get("items")}
    if fund_names:
        items = [ISSUER_FIELDS[x]["item"] for x in fund_names]
        fund, src, st["fund_events"] = load_events(events, m, EVENTS_ADAPTER, items, ciks, "fund-events", budget)
        for x in fund_names:
            sources[x] += [man_src] + src
    if grp_names:
        if sic_events is not None:
            sic, src, st["sic_events"] = load_sic_stage(sic_events, sic_events_sha256, ciks, budget)
            st["sic_events_override"] = {"manifest_sha256": src[0]["sha256"], "adapter": SIC_STAGE_ADAPTER["schema"],
                                         "column_mapping": SIC_STAGE_MAPPING}
        else:
            sic, src, st["sic_events"] = load_events(events, m, SIC_ADAPTER, [], ciks, "sic-events", budget)
            src = [man_src] + src
        ff12, ff49, st["sic_mapping"] = sic_mapping()
        codes = {"sic2": None, "ff12": ff12, "ff49": ff49}
        for x in grp_names:
            sources[x] += src
    budget.report("issuer-inputs", linked_ciks=len(ciks), fund_rows=len(fund["clock"]) if fund else 0,
                  sic_rows=len(sic["clock"]) if sic else 0)
    link_counts = {"member_cells": 0, "unlinked": 0, "ambiguous": 0, "secondary": 0, "primary": 0}
    reasons = {x: {"no_visible_row": 0, "stale": 0, "visible_nan": 0, "fc1_finite_member_cells": 0} for x in fund_names}
    reasons.update({x: {"no_visible_row": 0, "stale": 0, "unmapped": 0} for x in grp_names})
    if want_me:
        reasons["me_company"] = {"own_line_nan": 0, "other_linked_line_nan": 0, "multi_line_finite": 0}
    fund_latest = np.full(len(ciks), -1, dtype=np.int64)
    sic_latest = np.full(len(ciks), -1, dtype=np.int64)
    fp = sp = 0
    writers, stream, so_file = {}, None, None
    try:
        for x in names:
            writers[x] = FieldWriter(output, x, role)
        if want_me:
            so_path = output / "shares_out.f64"
            if so_path.stat().st_size != nd * n * 8:
                raise ValueError("me_company: this run's shares_out.f64 has the wrong size")
            stream = RoleRows(role, ("raw_close.f64", "present.u8"))
            so_file = so_path.open("rb")
        for t in range(nd):
            day = int(role.days[t])
            member = role.member[t] != 0
            lk = link[t]
            safe = np.maximum(lk, 0)
            pline = (lk >= 0) & primary[t]
            link_counts["member_cells"] += int(np.count_nonzero(member))
            link_counts["unlinked"] += int(np.count_nonzero(member & (lk == -1)))
            link_counts["ambiguous"] += int(np.count_nonzero(member & (lk == -2)))
            link_counts["secondary"] += int(np.count_nonzero(member & (lk >= 0) & ~pline))
            link_counts["primary"] += int(np.count_nonzero(member & pline))
            if t >= lag:
                mark = int(marks[t - lag])
                if fund is not None:
                    fp = advance(fund, fund_latest, fp, mark)
                if sic is not None:
                    sp = advance(sic, sic_latest, sp, mark)
            if fund_names:
                e = np.where(pline, fund_latest[safe], -1)
                has = e >= 0
                es = np.maximum(e, 0)
                if len(fund["clock"]):
                    fresh = has & ((day - fund["period_end"][es]) <= fund["stale_days"][es])
                    fc1 = fund["fc1"][es]
                else:
                    fresh = fc1 = np.zeros(n, dtype=bool)
                for x in fund_names:
                    v = fund["values"][ISSUER_FIELDS[x]["item"]]
                    row = np.where(fresh, v[es], np.nan) if len(v) else np.full(n, np.nan)
                    writers[x].write(row)
                    r = reasons[x]
                    finite = np.isfinite(row)
                    r["no_visible_row"] += int(np.count_nonzero(member & pline & ~has))
                    r["stale"] += int(np.count_nonzero(member & has & ~fresh))
                    r["visible_nan"] += int(np.count_nonzero(member & fresh & ~finite))
                    r["fc1_finite_member_cells"] += int(np.count_nonzero(member & finite & fc1))
            if grp_names:
                s = np.where(pline, sic_latest[safe], -1)
                has = s >= 0
                ss = np.maximum(s, 0)
                if len(sic["clock"]):
                    fresh = has & ((day - sic["clock"][ss] // DAY_NS) <= GRP_STALE_DAYS)
                    code = sic["sic"][ss]
                else:
                    fresh, code = np.zeros(n, dtype=bool), np.zeros(n, dtype=np.int64)
                for x in grp_names:
                    kind = ISSUER_FIELDS[x]["code"]
                    value = (code // 100).astype(np.float64) if kind == "sic2" else codes[kind][code]
                    row = np.where(fresh, value, np.nan)
                    writers[x].write(row)
                    r = reasons[x]
                    r["no_visible_row"] += int(np.count_nonzero(member & pline & ~has))
                    r["stale"] += int(np.count_nonzero(member & has & ~fresh))
                    r["unmapped"] += int(np.count_nonzero(member & fresh & np.isnan(row)))
            if want_me:
                raw, present = stream.row("raw_close.f64"), stream.row("present.u8") != 0
                blob = so_file.read(n * 8)
                if len(blob) != n * 8:
                    raise ValueError("me_company: this run's shares_out.f64 is truncated")
                so = np.frombuffer(blob, dtype="<f8")
                with np.errstate(invalid="ignore", over="ignore"):
                    line_me = np.where(present, so * raw, np.nan)
                ok = np.isfinite(line_me) & (line_me > 0)
                linked = lk >= 0
                idx = lk[linked]
                total = np.bincount(idx, weights=np.where(ok, line_me, 0.0)[linked], minlength=len(ciks))
                bad = np.bincount(idx, weights=(~ok)[linked].astype(np.float64), minlength=len(ciks))
                lines = np.bincount(idx, minlength=len(ciks))
                company = np.where(bad == 0, total, np.nan)
                row = np.where(pline, company[safe] if len(ciks) else np.nan, np.nan)
                writers["me_company"].write(row)
                r = reasons["me_company"]
                r["own_line_nan"] += int(np.count_nonzero(member & pline & ~ok))
                r["other_linked_line_nan"] += int(np.count_nonzero(member & pline & ok & ~np.isfinite(row)))
                r["multi_line_finite"] += int(np.count_nonzero(member & np.isfinite(row) & (lines[safe] > 1)))
            if t % 256 == 0:
                budget.check("issuer-write")
        if want_me:
            if so_file.read(1):
                raise ValueError("me_company: this run's shares_out.f64 is longer than the role shape")
            role_inputs = stream.verify()
            sources["me_company"] += role_inputs
    except BaseException:
        for w in writers.values():
            w.f.close()  # refused: partial files stay unpublished (no manifest), handles released
        raise
    finally:
        if stream is not None:
            stream.close()
        if so_file is not None:
            so_file.close()
    for w in writers.values():
        w.close()
    st["link_member_cells"] = link_counts
    for x in names:
        extras[x] = {"nan_reasons_member_cells": reasons[x]}
        if ISSUER_FIELDS[x]["lagged"]:
            extras[x]["fund_lag_sessions"] = lag
        if x in grp_names and sic_events is not None:
            extras[x]["sic_events_override"] = SIC_STAGE_MAPPING
    return writers, sources, st, extras


# ---------------------------------------------------------------------------
# FINRA daily short sale volume (sv_ratio126; library v6.1)
# ---------------------------------------------------------------------------

_SI_CANON = re.compile(SI_CANON_PATTERN)
_SV_FILE = re.compile(SV_FILE_PATTERN)
SV_COLUMNS = ("Date", "Symbol", "ShortVolume", "ShortExemptVolume", "TotalVolume", "Market")
SV_VOLUMES = ("ShortVolume", "ShortExemptVolume", "TotalVolume")
SV_TH_TYPES = {"tradingDate": pa.date32(), "securityID": pa.int64(), "ticker_tk": pa.string()}
SV_CODE_BITS = 21                # canonical / raw ticker codes < 2^21; days < 2^21 (keys fit an int64)
SV_FILES_LIST_RULE = ("sha256 of canonical JSON (sorted keys, separators ',' ':') of the list of [file name, file "
                      "bytes, file SHA-256] of every CNMS file read (the .gz bytes as read), sorted by name")


def si_canon(ticker: str) -> str:
    """iteration21_finra_si_asof.canon, verbatim: upper-case, then strip '.', whitespace, '/', '-'."""
    return _SI_CANON.sub("", ticker.strip().upper())


def si_raw(ticker: str) -> str:
    """The producer's exact-match spelling (``t.strip().upper()``) of an ORATS ticker or a FINRA symbol."""
    return ticker.strip().upper()


def cnms_to_si(symbol: str) -> str:
    """A CNMS symbol in the short-interest symbolCode spelling: lowercase suffix markers p/r/w -> PR/RT/WI."""
    for marker, spelled in CNMS_SUFFIX_MARKERS:
        symbol = symbol.replace(marker, spelled)
    return symbol


def sv_listing(directory: Path) -> list:
    """(day, name) of every CNMSshvolYYYYMMDD.txt.gz in the directory, ascending; other CNMS* names refuse."""
    out = []
    for path in directory.iterdir():
        m = _SV_FILE.match(path.name)
        if m is None:
            if path.name.startswith("CNMS"):
                raise ValueError(f"short volume: unexpected file name {path.name}")
            continue
        out.append((day_of(dt.date(int(m[1][:4]), int(m[1][4:6]), int(m[1][6:]))), path.name))
    out.sort()
    return out


def read_sv_receipt(directory: Path):
    """The downloader's receipt: file name -> (decompressed bytes, decompressed SHA-256, rows, downloaded_at)."""
    blob = (directory / SV_RECEIPT).read_bytes()
    rows = list(csv.DictReader(io.StringIO(blob.decode("utf-8"))))
    need = {"date", "file", "bytes", "sha256_of_raw_bytes", "rows", "http_status", "downloaded_at"}
    if not rows or not need <= set(rows[0]):
        raise ValueError(f"short volume: {SV_RECEIPT} lacks the columns {sorted(need)}")
    receipt = {}
    for r in rows:
        if not r["file"]:
            continue  # a probe of a non-session date (HTTP 403) carries no file
        m = _SV_FILE.match(r["file"])
        if m is None or r["http_status"] != "200" or r["date"] != f"{m[1][:4]}-{m[1][4:6]}-{m[1][6:]}":
            raise ValueError(f"short volume: {SV_RECEIPT} row for {r['file']!r} is inconsistent")
        entry = (int(r["bytes"]), r["sha256_of_raw_bytes"].lower(), int(r["rows"]), r["downloaded_at"])
        if receipt.setdefault(r["file"], entry)[:3] != entry[:3]:
            raise ValueError(f"short volume: {SV_RECEIPT} lists {r['file']} twice with different bytes")
    return receipt, {"path": str((directory / SV_RECEIPT).resolve()), "bytes": len(blob), "sha256": sha_bytes(blob)}


def parse_cnms(raw: bytes, day: int, name: str):
    """Strict CNMSshvol contract -> (symbols, ShortVolume f64, TotalVolume f64) in file order.

    Exact header; the last line is the row count; every Date is the file's date; symbols match
    ``SV_SYMBOL_PATTERN``; the three volumes are non-negative decimals, ShortVolume <= TotalVolume."""
    nl = raw.find(b"\n")
    if nl < 0 or raw[:nl].rstrip(b"\r") != SV_HEADER:
        raise ValueError(f"{name}: header is not {SV_HEADER.decode()}")
    stripped = raw.rstrip(b"\r\n")
    last = stripped.rfind(b"\n")
    trailer = stripped[last + 1:].rstrip(b"\r")
    if last < nl or not trailer.isdigit():
        raise ValueError(f"{name}: no row-count trailer line")
    body = raw[nl + 1:last + 1]
    if b"\n\n" in body or b"\n\r\n" in body or body.startswith((b"\n", b"\r\n")):
        raise ValueError(f"{name}: empty line")
    if not body:
        if int(trailer):
            raise ValueError(f"{name}: trailer counts {int(trailer)} rows, file has 0")
        return [], np.empty(0), np.empty(0)
    table = pacsv.read_csv(
        io.BytesIO(body),
        read_options=pacsv.ReadOptions(column_names=list(SV_COLUMNS), use_threads=False, block_size=1 << 22),
        parse_options=pacsv.ParseOptions(delimiter="|", quote_char=False, double_quote=False, escape_char=False,
                                         newlines_in_values=False),
        convert_options=pacsv.ConvertOptions(column_types={c: pa.string() for c in SV_COLUMNS},
                                             strings_can_be_null=False))
    if table.num_rows != int(trailer):
        raise ValueError(f"{name}: trailer counts {int(trailer)} rows, file has {table.num_rows}")
    col = {c: table.column(c).combine_chunks() for c in SV_COLUMNS}
    if not pc.all(pc.equal(col["Date"], date_of(day).replace("-", ""))).as_py():
        raise ValueError(f"{name}: a Date differs from the file date")
    if not pc.all(pc.match_substring_regex(col["Symbol"], SV_SYMBOL_PATTERN)).as_py():
        raise ValueError(f"{name}: a Symbol outside {SV_SYMBOL_PATTERN}")
    vol = {}
    for c in SV_VOLUMES:
        if not pc.all(pc.match_substring_regex(col[c], r"^([0-9]+\.?[0-9]*|\.[0-9]+)$")).as_py():
            raise ValueError(f"{name}: {c} is not a non-negative decimal")
        vol[c] = pc.cast(col[c], pa.float64()).to_numpy(zero_copy_only=False)
        if not np.all(np.isfinite(vol[c])):
            raise ValueError(f"{name}: {c} is not finite")
    if np.any(vol["ShortVolume"] > vol["TotalVolume"]):
        raise ValueError(f"{name}: ShortVolume above TotalVolume")
    return col["Symbol"].to_pylist(), vol["ShortVolume"], vol["TotalVolume"]


class SvTickerMap:
    """The PIT ticker map (``SV_MAP_RULE``) over the role's TickerHistory3 for vendor dates [lo, hi].

    Only what can reach a role line is kept: keys (day, canonical code) of role rows, each with its role column or
    -1 when the canonical ticker is held by more than one securityID that day (role or not), and the (day,
    canonical, raw) triples of role rows for the exact-match collision rule."""

    def __init__(self, th: Path, role: Role, lo: int, hi: int, budget: Budget, known_digest=None):
        captured = identity(th)
        pf = pq.ParquetFile(th, memory_map=False)
        for c, typ in SV_TH_TYPES.items():
            if pf.schema_arrow.field(c).type != typ:
                raise ValueError(f"TickerHistory column {c} is {pf.schema_arrow.field(c).type}, expected {typ}")
        if known_digest is not None and known_digest[0] == captured:
            digest = known_digest[1]  # hashed by this run's TickerHistory group; same file identity
        else:
            budget.report("sv-th-hash-start", bytes=captured[2])
            digest = sha_file(th, budget)
        if role.source_sha256 is None or digest != role.source_sha256:
            raise ValueError("TickerHistory SHA-256 differs from the role's source_sha256 (a different vendor file)")
        if identity(th) != captured:
            raise ValueError("TickerHistory changed during hashing")
        self.lo, self.hi = lo, hi
        self.canon_ids, self.raw_ids, codes = {}, {}, {}

        def code_of(ticker: str):
            got = codes.get(ticker)
            if got is None:
                got = (-1, -1) if not ticker.strip() else (
                    self.canon_ids.setdefault(si_canon(ticker), len(self.canon_ids)),
                    self.raw_ids.setdefault(si_raw(ticker), len(self.raw_ids)))
                codes[ticker] = got
            return got

        st = {"rows_scanned": 0, "rows_in_range_valid": 0, "role_rows": 0, "non_role_rows_sharing_a_role_key": 0}

        def rows():
            """(day, securityID, canonical code, raw code) of the valid rows dated in [lo, hi], per batch."""
            for batch in pf.iter_batches(batch_size=65536, columns=list(SV_TH_TYPES), use_threads=False):
                budget.check("sv-th-batch")
                d = pc.fill_null(batch.column(0).cast(pa.int32()), -1).to_numpy().astype(np.int64)
                sid = pc.fill_null(batch.column(1), 0).to_numpy()
                tk = batch.column(2)
                idx = np.flatnonzero((d >= lo) & (d <= hi) & (sid > 0) & tk.is_valid().to_numpy(zero_copy_only=False))
                if not len(idx):
                    yield batch.num_rows, None
                    continue
                enc = pc.dictionary_encode(tk.take(pa.array(idx, type=pa.int64())))
                pairs = np.array([code_of(t) for t in enc.dictionary.to_pylist()], dtype=np.int64).reshape(-1, 2)
                ind = enc.indices.to_numpy(zero_copy_only=False)
                cc, rc = pairs[ind, 0], pairs[ind, 1]
                ok = cc >= 0  # blank tickers dropped (producer: ticker_tk.strip() != "")
                yield batch.num_rows, (d[idx][ok], sid[idx][ok], cc[ok], rc[ok])

        seen = np.zeros(hi - lo + 1, dtype=bool)
        parts = []
        for scanned, got in rows():  # pass 1: trading dates and role rows
            st["rows_scanned"] += scanned
            if got is None:
                continue
            d, sid, cc, rc = got
            seen[d - lo] = True
            st["rows_in_range_valid"] += len(d)
            pos, on = role.columns_of(sid)
            parts.append(np.stack((d[on], pos[on], cc[on], rc[on])).astype(np.int32))  # all < 2^21
        budget.check("sv-th-pass1")
        if max(len(self.canon_ids), len(self.raw_ids), hi + 1, role.n) >= (1 << SV_CODE_BITS):
            raise ValueError("short volume: ticker code or day beyond the key width")
        rows4 = np.concatenate(parts, axis=1) if parts else np.empty((4, 0), np.int32)
        parts.clear()
        rd, rcol, rcc, rrc = (rows4[i].astype(np.int64) for i in range(4))
        del rows4
        st["role_rows"] = len(rd)
        self.kraw = np.unique((rd << (2 * SV_CODE_BITS)) | (rcc << SV_CODE_BITS) | rrc)
        # a role line carrying two canonical tickers on one day (two vendor rows): the collision rule decides
        line_day = np.unique((rd << (2 * SV_CODE_BITS)) | (rcol << SV_CODE_BITS) | rcc) >> SV_CODE_BITS
        st["role_line_days_with_several_canonical_tickers"] = int(np.count_nonzero(
            np.unique(line_day, return_counts=True)[1] > 1))
        del line_day, rrc
        rk = (rd << SV_CODE_BITS) | rcc
        rsid = role.ids[rcol]
        del rd, rcol, rcc
        uk = np.unique(rk)
        nk, ns = [], []
        if len(uk):
            for _, got in rows():  # pass 2: non-role rows sharing a (day, canonical) key with a role row
                if got is None:
                    continue
                d, sid, cc, _ = got
                off = ~role.columns_of(sid)[1]
                k = (d[off] << SV_CODE_BITS) | cc[off]
                hit = uk[np.minimum(np.searchsorted(uk, k), len(uk) - 1)] == k
                nk.append(k[hit])
                ns.append(sid[off][hit])
        if identity(th) != captured:
            raise ValueError("TickerHistory changed while reading")
        nk = np.concatenate(nk) if nk else np.empty(0, np.int64)
        ns = np.concatenate(ns) if ns else np.empty(0, np.int64)
        st["non_role_rows_sharing_a_role_key"] = len(nk)
        keys, sids = np.concatenate((rk, nk)), np.concatenate((rsid, ns))
        del rk, rsid, nk, ns
        o = np.lexsort((sids, keys))
        keys, sids = keys[o], sids[o]
        del o
        budget.check("sv-th-keys")
        distinct = np.ones(len(keys), dtype=bool)
        distinct[1:] = (keys[1:] != keys[:-1]) | (sids[1:] != sids[:-1])
        keys, sids = keys[distinct], sids[distinct]
        start = np.searchsorted(keys, uk)
        count = np.diff(np.append(start, len(keys)))
        self.uk = uk
        self.ucol = np.where(count == 1, role.columns_of(sids[start])[0], -1).astype(np.int64)
        st["ambiguous_role_keys"] = int(np.count_nonzero(count > 1))
        st["trading_dates"] = int(np.count_nonzero(seen))
        st["distinct_canonical_tickers"] = len(self.canon_ids)
        self.dates = lo + np.flatnonzero(seen).astype(np.int64)
        self.stats = st
        self.source = {"path": str(th.resolve()), "bytes": captured[2], "sha256": digest,
                       "columns": list(SV_TH_TYPES), "dates": [date_of(lo), date_of(hi)]}

    def ticker_date(self, day: int):
        """The last vendor trading date <= day within the lookback, else None."""
        k = int(np.searchsorted(self.dates, day, side="right")) - 1
        if k < 0 or day - int(self.dates[k]) > SV_TICKER_LOOKBACK_DAYS:
            return None
        return int(self.dates[k])

    def map_symbols(self, symbols: list, tday: int, cache: dict):
        """Role column (-1 ambiguous, -2 no role key) and exact flag per symbol at ticker date ``tday``."""
        coded = []
        for s in symbols:
            got = cache.get(s)
            if got is None:
                t = cnms_to_si(s)
                got = (self.canon_ids.get(si_canon(t), -1), self.raw_ids.get(si_raw(t), -1),
                       int(any(ch.islower() for ch in t)))
                cache[s] = got
            coded.append(got)
        coded = np.array(coded, dtype=np.int64).reshape(-1, 3)
        cc, rc = coded[:, 0], coded[:, 1]
        known = cc >= 0
        key = (tday << SV_CODE_BITS) | np.where(known, cc, 0)
        p = np.minimum(np.searchsorted(self.uk, key), max(len(self.uk) - 1, 0))
        found = known & (self.uk[p] == key) if len(self.uk) else np.zeros(len(key), dtype=bool)
        col = np.where(found, self.ucol[p] if len(self.uk) else -2, -2)
        triple = (tday << (2 * SV_CODE_BITS)) | (np.where(known, cc, 0) << SV_CODE_BITS) | np.where(rc >= 0, rc, 0)
        q = np.minimum(np.searchsorted(self.kraw, triple), max(len(self.kraw) - 1, 0))
        exact = found & (rc >= 0) & (self.kraw[q] == triple) if len(self.kraw) else np.zeros(len(key), dtype=bool)
        return col, exact, coded[:, 2].astype(bool)


def sv_resolve_collisions(col: np.ndarray, exact: np.ndarray) -> np.ndarray:
    """Producer rule: several rows on one securityID keep the single exact-spelling row, else all drop."""
    mapped = col >= 0
    keep = mapped.copy()
    if mapped.any():
        mc, me = col[mapped], exact[mapped]
        _, inv, cnt = np.unique(mc, return_inverse=True, return_counts=True)
        nex = np.bincount(inv, weights=me.astype(np.float64)).astype(np.int64)
        keep[mapped] = (cnt[inv] == 1) | ((nex[inv] == 1) & me)
    return keep


def sv_window_files(listing: list, role_days) -> tuple:
    """The CNMS files sv_ratio126 reads for a role, from the directory listing (``sv_listing``): (the session calendar
    of the windows, its prefix length, its sessions that open a window, the file days read, ascending). Shared by
    ``sv_field`` and the --reuse check of its directory source."""
    fdays = np.array([d for d, _ in listing], dtype=np.int64)
    names = dict(listing)
    prefix = fdays[fdays < int(role_days[0])][-SV_WINDOW:]
    ext = np.concatenate((prefix, role_days)).astype(np.int64)  # the session calendar of the windows
    on_ext = set(int(x) for x in ext[:-1])  # the last session's file is never inside a window
    return ext, len(prefix), on_ext, sorted(d for d in names if d in on_ext)


def sv_field(role: Role, output: Path, budget: Budget, directory: Path, th: Path, known_digest=None):
    """sv_ratio126 (``SV_FIELDS``): streams the CNMS files one session at a time through a 126-session ring."""
    listing = sv_listing(directory)
    receipt, receipt_source = read_sv_receipt(directory)
    fdays = np.array([d for d, _ in listing], dtype=np.int64)
    names = dict(listing)
    last = int(role.days[-1])
    ext, e0, on_ext, need = sv_window_files(listing, role.days)
    if not need:
        raise ValueError("short volume: no CNMS file dated on a session before the role's last session")
    span = [d for d in names if int(ext[0]) <= d < last]
    off_calendar = sorted(d for d in span if d not in on_ext)
    missing = [int(x) for x in ext[:-1] if int(x) >= int(fdays[0]) and int(x) not in names]
    tmap = SvTickerMap(th, role, need[0] - SV_TICKER_LOOKBACK_DAYS, need[-1], budget, known_digest)
    budget.report("sv-ticker-map", **tmap.stats)
    n, W = role.n, SV_WINDOW
    budget.admit(W * n * 17 + (32 << 20), "sv-ring")
    ring_s, ring_t = np.zeros((W, n)), np.zeros((W, n))
    ring_c = np.zeros((W, n), dtype=bool)
    per_year, files, downloaded, cache = {}, [], [], {}
    counters = ("files", "rows", "duplicate_symbol_rows", "files_without_ticker_date", "rows_mapped_role",
                "rows_ambiguous", "rows_no_role_key", "rows_collision_dropped", "rows_kept",
                "rows_kept_canonical_only", "rows_unknown_lowercase_marker")
    writer = FieldWriter(output, "sv_ratio126", role)
    try:
        for e in range(len(ext)):
            day = int(ext[e])
            if e >= e0:
                count = np.count_nonzero(ring_c, axis=0)
                short, total = ring_s.sum(axis=0), ring_t.sum(axis=0)
                ok = (count >= SV_MIN_SESSIONS) & (total > 0)
                writer.write(np.where(ok, short / np.where(ok, total, 1.0), np.nan))
            if e == len(ext) - 1:
                break
            slot = e % W  # session e - W leaves the window of session e + 1
            ring_s[slot], ring_t[slot], ring_c[slot] = 0.0, 0.0, False
            name = names.get(day)
            if name is None:
                continue
            blob = (directory / name).read_bytes()
            rec = receipt.get(name)
            if rec is None:
                raise ValueError(f"short volume: {name} has no {SV_RECEIPT} row")
            raw = gzip.decompress(blob)
            if len(raw) != rec[0] or sha_bytes(raw) != rec[1]:
                raise ValueError(f"short volume: {name} does not match its {SV_RECEIPT} row")
            symbols, short, total = parse_cnms(raw, day, name)
            if len(symbols) != rec[2]:
                raise ValueError(f"short volume: {name} row count differs from {SV_RECEIPT}")
            files.append([name, len(blob), sha_bytes(blob)])
            downloaded.append(rec[3])
            y = per_year.setdefault(date_of(day)[:4], dict.fromkeys(counters, 0))
            y["files"] += 1
            y["rows"] += len(symbols)
            lastpos = {s: k for k, s in enumerate(symbols)}  # the last row of a symbol wins (producer rule)
            y["duplicate_symbol_rows"] += len(symbols) - len(lastpos)
            idx = np.array(sorted(lastpos.values()), dtype=np.int64)
            tday = tmap.ticker_date(day)
            if tday is None:
                y["files_without_ticker_date"] += 1
                continue
            col, exact, lower = tmap.map_symbols([symbols[k] for k in idx], tday, cache)
            keep = sv_resolve_collisions(col, exact)
            y["rows_mapped_role"] += int(np.count_nonzero(col >= 0))
            y["rows_ambiguous"] += int(np.count_nonzero(col == -1))
            y["rows_no_role_key"] += int(np.count_nonzero(col == -2))
            y["rows_collision_dropped"] += int(np.count_nonzero((col >= 0) & ~keep))
            y["rows_kept"] += int(np.count_nonzero(keep))
            y["rows_kept_canonical_only"] += int(np.count_nonzero(keep & ~exact))
            y["rows_unknown_lowercase_marker"] += int(np.count_nonzero(lower))
            cols, rows = col[keep], idx[keep]
            ring_s[slot, cols], ring_t[slot, cols], ring_c[slot, cols] = short[rows], total[rows], True
            if len(files) % 32 == 0:
                budget.report("sv-files", files=len(files), last=name)
    except BaseException:
        writer.f.close()  # refused: the partial file stays unpublished (no manifest), but its handle is released
        raise
    writer.close()
    files.sort()
    list_sha = sha_bytes(canonical(files).encode("utf-8"))
    source = {"path": str(directory.resolve()), "files_read": len(files), "files_sha256": list_sha,
              "files_list_rule": SV_FILES_LIST_RULE, "first_file_date": date_of(need[0]),
              "last_file_date": date_of(need[-1])}
    totals = {k: sum(v[k] for v in per_year.values()) for k in counters}
    extra = {"short_volume": {
        "formula_id": SV_FORMULA_ID, "window_sessions": SV_WINDOW, "min_sessions": SV_MIN_SESSIONS,
        "lag_sessions": SV_LAG_SESSIONS,
        "files": {**source, "downloaded_at_first": min(downloaded), "downloaded_at_last": max(downloaded),
                  "receipt": receipt_source, "listed": len(listing),
                  "listed_first_date": date_of(fdays[0]), "listed_last_date": date_of(fdays[-1])},
        "calendar": {"rule": "role sessions, preceded by the last <= 126 FINRA file dates before the role's first "
                             "session; the last role session's file is never read",
                     "prefix_sessions": e0,
                     "role_sessions_without_file": len(missing), "role_sessions_without_file_first": [
                         date_of(x) for x in missing[:20]],
                     "files_off_role_calendar": len(off_calendar), "files_off_role_calendar_first": [
                         date_of(x) for x in off_calendar[:20]],
                     "files_not_read_outside_windows": len(listing) - len(files) - len(off_calendar)},
        "mapping": {"rule": SV_MAP_RULE, "suffix_markers": {a: b for a, b in CNMS_SUFFIX_MARKERS},
                    "canonicaliser": SI_CANON_PATTERN, "ticker_lookback_days": SV_TICKER_LOOKBACK_DAYS,
                    "ported_from": "build-equity/audits/iteration21_finra_si_asof.py (si_shares / si_dtc producer)",
                    "totals": totals, "per_year": dict(sorted(per_year.items())),
                    "tickerhistory": tmap.stats}}}
    stats = {"files_read": len(files), "files_sha256": list_sha, **{k: totals[k] for k in (
        "rows", "rows_kept", "rows_ambiguous", "rows_collision_dropped")}}
    return writer, [source, receipt_source, tmap.source], stats, extra


# ---------------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------------

def run(role_dir: Path, role_sha256: str, output: Path, fields=DEFAULT_FIELDS, *, max_rss_mib=700,
        max_seconds=1800.0, finra: Path = DEFAULT_FINRA, tickerhistory: Path = DEFAULT_TICKERHISTORY,
        lake: Path = DEFAULT_LAKE, identity_bridge: Path | None = None, identity_bridge_sha256: str | None = None,
        fund_events: Path | None = None, fund_events_sha256: str | None = None,
        fund_lag_sessions: int = FUND_LAG_SESSIONS_DECLARED, finra_short_volume: Path | None = None,
        reuse: Path | None = None, reuse_sha256: str | None = None, reuse_hardlink: bool = False,
        module_options: dict | None = None, sic_events: Path | None = None, sic_events_sha256: str | None = None):
    fields = list(fields)
    if not fields or len(set(fields)) != len(fields) or any(f not in ALL_FIELDS for f in fields):
        raise ValueError(f"--fields must be distinct names from {', '.join(ALL_FIELDS)}")
    selected = [f for f in ALL_FIELDS if f in fields]  # registry order: stable manifests
    for f in selected:
        missing = [x for x in ALL_FIELDS[f].get("requires", []) if x not in selected]
        if missing:
            raise ValueError(f"--fields: {f} requires {', '.join(missing)} in the same run (its units rule reads it)")
    issuer = [f for f in selected if f in ISSUER_FIELDS]
    kinds = {ISSUER_FIELDS[f]["kind"] for f in issuer}
    if (sic_events is None) != (not sic_events_sha256):
        raise ValueError("--sic-events and --sic-events-sha256 go together")
    if sic_events is not None and "grp" not in kinds:
        raise ValueError("--sic-events replaces the SIC table of the grp_* fields; no grp_* field is requested")
    if issuer:
        if identity_bridge is None or not identity_bridge_sha256:
            raise ValueError("--fields: issuer fields need --identity-bridge and --identity-bridge-sha256")
        if ("fund" in kinds or ("grp" in kinds and sic_events is None)) and (fund_events is None or not fund_events_sha256):
            raise ValueError("--fields: fundamental and group fields need --fund-events and --fund-events-sha256")
        if isinstance(fund_lag_sessions, bool) or not isinstance(fund_lag_sessions, int) or not 0 <= fund_lag_sessions <= 5:
            raise ValueError("--fund-lag-sessions must be an integer in [0, 5] (declared: 1)")
    if any(f in SV_FIELDS for f in selected) and finra_short_volume is None:
        raise ValueError("--fields: sv_ratio126 needs --finra-short-volume (the CNMSshvol*.txt.gz directory)")
    for m in FIELD_MODULES:  # W5a registry hook: each opt-in module checks its inputs before any output
        m.check(selected, module_options or {})
    budget = Budget(max_rss_mib, max_seconds)
    role = Role(role_dir, role_sha256)
    budget.report("role-admitted", dates=role.n_dates, instruments=role.n)
    output.mkdir(parents=False, exist_ok=False)  # exclusive; never reuse or replace
    reused, reuse_block, th_known = {}, None, None
    if reuse is not None:
        roots = {"role": [role_dir], "finra": [finra], "th": [tickerhistory], "lake": [lake],
                 "issuer": [p for p in (identity_bridge, fund_events, role_dir, sic_events) if p is not None],
                 # grp_* inputs: the bridge and THIS run's SIC table (a prior built from the other table is recomputed)
                 "issuer:grp": [p for p in (identity_bridge, sic_events if sic_events is not None else fund_events)
                                if p is not None],
                 "finra_sv": [p for p in (finra_short_volume, tickerhistory) if p is not None]}
        module_fields = {f for m in FIELD_MODULES for f in m.FIELDS}
        reused, reuse_block, th_known = reuse_fields(Path(reuse), reuse_sha256, role,
                                                     [f for f in selected if f not in module_fields], output, budget,
                                                     roots=roots, lag=fund_lag_sessions, tickerhistory=tickerhistory,
                                                     hardlink=reuse_hardlink)
        for m in FIELD_MODULES:  # v8 C-3: each opt-in module reuses by its own PRODUCERS, stage pins and formula
            names = [f for f in selected if f in m.FIELDS]
            if names:
                got, why, prior_checks = reuse_module_fields(
                    sys.modules[type(m).__module__], Path(reuse), reuse_sha256, role, names, output, budget,
                    options=module_options or {}, reused_names=set(reused), hardlink=reuse_hardlink)
                reused.update(got)
                merge_module_reuse(reuse_block, selected, names, got, why, group=m.GROUP,
                                   prior_check=prior_checks.get(m.GROUP))
    outcome = {}
    source_checks = dict(reuse_block.pop("_source_checks")) if reuse_block else {}
    groups = {g: [f for f in selected if ALL_FIELDS[f]["group"] == g and f not in reused]
              for g in ("role", "finra", "th", "lake", "issuer", "finra_sv")}
    field_stats, field_extras = {}, {}
    # th_known: (file identity, SHA-256) of the TickerHistory hashed by the th group (or by the reuse source check),
    # reused by finra_sv
    if groups["role"]:
        w, src, stats = market_return_field(role, output, budget)
        outcome["mkt_ret"] = (w, src, w.coverage())
        field_stats["mkt_ret"] = stats
        budget.report("mkt_ret-complete", **stats["contributors_per_session"])
    if groups["finra"]:
        schedule = read_schedule(finra)
        for name in groups["finra"]:
            w, src, cov, stats, extra = finra_field(name, finra, role, output, schedule, budget)
            outcome[name] = (w, src, cov)
            source_checks[name] = stats
            field_extras[name] = extra
            budget.report(f"{name}-complete", **{k: cov[k] for k in ("finite_member_frac",)})
    if groups["th"]:
        before = identity(tickerhistory)
        writers, src, stats, th_digest, extras = tickerhistory_fields(groups["th"], tickerhistory, role, output, budget)
        th_known = (before, th_digest)
        source_checks["tickerhistory"] = stats
        field_extras.update(extras)
        for name, w in writers.items():
            outcome[name] = (w, src, w.coverage())
        budget.report("tickerhistory-complete", **stats)
    if groups["lake"]:
        writers, src, stats = lake_fields(groups["lake"], lake, role, output, budget)
        source_checks["lake"] = stats
        for name, w in writers.items():
            outcome[name] = (w, src, w.coverage())
        budget.report("lake-complete")
    if groups["issuer"]:  # after th: me_company reads this run's published shares_out.f64
        writers, src, stats, extras = issuer_fields(
            groups["issuer"], role, output, budget, bridge=identity_bridge, bridge_sha256=identity_bridge_sha256,
            events=fund_events, events_sha256=fund_events_sha256, lag=fund_lag_sessions, sic_events=sic_events,
            sic_events_sha256=sic_events_sha256)
        source_checks["issuer"] = stats
        field_extras.update(extras)
        for name, w in writers.items():
            outcome[name] = (w, src[name], w.coverage())
        budget.report("issuer-complete")
    if groups["finra_sv"]:
        w, src, stats, extra = sv_field(role, output, budget, finra_short_volume, tickerhistory, th_known)
        outcome["sv_ratio126"] = (w, src, w.coverage())
        source_checks["finra_short_volume"] = stats
        field_extras["sv_ratio126"] = extra
        budget.report("sv_ratio126-complete", finite_member_frac=outcome["sv_ratio126"][2]["finite_member_frac"])
    for m in FIELD_MODULES:  # W5a registry hook: each opt-in module computes its requested, non-reused fields
        m.compute([f for f in selected if f in m.FIELDS and f not in reused], role, output, budget,
                  module_options or {}, outcome, source_checks, field_extras)
    files, entries = {}, []
    for name in selected:
        if name in reused:
            entry, file_pin = reused[name]
            files[entry["file"]] = file_pin
            entries.append(entry)
            continue
        w, src, cov = outcome[name]
        size = w.path.stat().st_size
        if size != role.n_dates * role.n * 8:
            raise ValueError(f"{name}: output size mismatch")
        digest, quantiles = digest_and_quantiles(w.path, role, w.vcount, budget)
        files[w.path.name] = {"bytes": size, "sha256": digest}
        cov["member_finite_quantiles"] = quantiles
        spec = ALL_FIELDS[name]
        clock = spec["clock"].format(lag=fund_lag_sessions) if spec.get("lagged") else spec["clock"]
        entry = {"name": name, "file": w.path.name, "dtype": "<f8", "layout": "date-major",
                 "shape": [role.n_dates, role.n], "units": spec["units"], "clock": clock,
                 "staleness": spec["staleness"], "source_columns": spec["source_columns"],
                 "sources": src, "caveats": spec["caveats"], "coverage": cov,
                 "sha256": files[w.path.name]["sha256"], "point_in_time": spec["point_in_time"],
                 "non_pit_aspects": spec.get("non_pit_aspects", [])}
        if not spec["point_in_time"]:
            entry["point_in_time_reason"] = spec["point_in_time_reason"]
        if "requires" in spec:
            entry["depends_on"] = spec["requires"]
        if "definition" in spec:
            entry["definition"] = spec["definition"]
        if name in field_stats:
            entry["stats"] = field_stats[name]
        entry.update(field_extras.get(name, {}))
        entries.append(entry)
    # Re-pin the role: the axes must not have changed underneath the run.
    Role(role_dir, role_sha256)
    manifest = {
        "schema": SCHEMA, "status": "complete",
        "role": {"path": str(role_dir.resolve()), "manifest_sha256": role.manifest_sha256,
                 "sessions_sha256": role.sessions_sha256, "ids_sha256": role.ids_sha256,
                 "member_sha256": role.manifest["files"]["member.u8"]["sha256"],
                 "dates": role.n_dates, "instruments": role.n, "score_begin": role.score_begin,
                 "score_end": role.score_end, "first_session": date_of(role.days[0]),
                 "last_session": date_of(role.days[-1]), "source_sha256": role.source_sha256,
                 "clock_recipe": role.manifest.get("clock_recipe")},
        "instrument_namespace": "spiderrock.securityID",
        "seal": {"exclusive_end": SEAL.isoformat(),
                 "rule": f"every source row available on or after {SEAL.isoformat()} is dropped before use; role sessions "
                         f"asserted < {SEAL.isoformat()}"},
        "cell_rule": "NaN where the field is not visible at the session decision or the source is absent",
        "coverage_basis": "member.u8 cells of the role (member cells with a finite value / member cells)",
        "visibility_mark": "every finite cell of every field is known by the session-date 22:00 UTC mark (the role close clock), before the 23:00 UTC decision",
        "point_in_time_definition": POINT_IN_TIME_DEFINITION,
        "non_point_in_time_fields": [e["name"] for e in entries if not e["point_in_time"]],
        "fields": entries, "files": files,
        "excluded_source_columns": EXCLUDED_SOURCE_COLUMNS,
        "source_checks": source_checks,
        **code_identity_of(builder_source()),
        "historical_vintage_verified": False, "common_stock_verified": False,
    }
    revisions = {f: FORMULA_REVISION[f] for f in selected if FORMULA_REVISION.get(f, 1) != 1}
    if revisions:  # absent while every field is at revision 1, so manifests stay byte-identical until a bump
        manifest["formula_revisions"] = revisions
    if reuse_block is not None:
        manifest["reuse"] = reuse_block
    publish(output / "manifest.json", manifest)
    budget.report("fields-complete", fields=len(entries), peak_rss_mib=budget.peak >> 20)
    return manifest


# ---------------------------------------------------------------------------
# Field reuse (--reuse)
# ---------------------------------------------------------------------------

def spec_definition(name: str, lag: int) -> dict:
    """The spec-level definition this code writes for ``name`` (the IC runner's field_definition keys + domain)."""
    spec = ALL_FIELDS[name]
    clock = spec["clock"].format(lag=lag) if spec.get("lagged") else spec["clock"]
    return {"units": spec["units"], "clock": clock, "staleness": spec["staleness"],
            "source_columns": spec["source_columns"], "definition": spec.get("definition"),
            "point_in_time": spec["point_in_time"], "non_pit_aspects": spec.get("non_pit_aspects", []),
            "domain": [float(x) for x in spec["domain"]] if "domain" in spec else None}


def entry_definition(entry: dict) -> dict:
    """The same definition as recorded by a manifest entry (domain from its plausibility block)."""
    d = {k: entry.get(k) for k in FORMULA_DEFINITION_KEYS}
    d["non_pit_aspects"] = entry.get("non_pit_aspects", [])
    p = entry.get("plausibility")
    d["domain"] = [float(p["min"]), float(p["max"])] if isinstance(p, dict) and "min" in p and "max" in p else None
    return d


def formula_id(name: str, definition: dict, revision: int | None = None) -> str:
    rev = FORMULA_REVISION.get(name, 1) if revision is None else revision
    return sha_bytes(canonical({"field": name, "revision": rev, "definition": definition}).encode("utf-8"))


def _norm(path) -> str:
    return os.path.normcase(os.path.abspath(str(path)))


def _under(path: str, root) -> bool:
    r = _norm(root)
    return path == r or path.startswith(r.rstrip("\\/") + os.sep)


def builder_source() -> bytes:
    """This builder's source bytes: what the manifest's code identity pins and --reuse compares producers with."""
    return Path(__file__).read_bytes()


def code_identity_of(raw: bytes) -> dict:
    """``code_identity`` of source bytes (kept apart: code_identity is part of the issuer producer's code)."""
    lf = raw.replace(b"\r\n", b"\n")
    return {"code_sha256": sha_bytes(raw), "code_sha256_lf": sha_bytes(lf),
            "code_git_blob_sha1": hashlib.sha1(b"blob %d\0" % len(lf) + lf).hexdigest()}


def git_blob(blob_sha1) -> bytes | None:
    """The blob ``blob_sha1`` of this module's git repository, or None (no git, unknown blob)."""
    if not isinstance(blob_sha1, str) or not re.fullmatch(r"[0-9a-f]{40}", blob_sha1):
        return None
    try:
        done = subprocess.run(["git", "-C", str(Path(__file__).resolve().parent), "cat-file", "blob", blob_sha1],
                              capture_output=True, timeout=120, check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    return done.stdout if done.returncode == 0 else None


def producer_source(ident: dict, current: bytes | None = None, what: str = "this builder") -> tuple[bytes | None, str]:
    """(LF source, how it was found) of the code named by a code identity, or (None, why not): this builder's source
    (default) or ``current`` (a field module's source) when the LF SHA-256 matches, else the recorded git blob."""
    lf_sha = ident.get("code_sha256_lf")
    if not isinstance(lf_sha, str):
        return None, "no recorded code_sha256_lf"
    current = (builder_source() if current is None else current).replace(b"\r\n", b"\n")
    if sha_bytes(current) == lf_sha:
        return current, f"{what} (same code_sha256_lf)"
    blob = git_blob(ident.get("code_git_blob_sha1"))
    if blob is None:
        return None, "code_sha256_lf differs from this builder and its git blob is not available"
    lf = blob.replace(b"\r\n", b"\n")
    if sha_bytes(lf) != lf_sha:
        return None, "the recorded git blob does not hash to the recorded code_sha256_lf"
    return lf, f"git blob {ident['code_git_blob_sha1']}"


def legacy_origin(entry: dict, name: str) -> dict:
    """Code identity of the builder of a payload reused before --reuse recorded it (a reused_from without
    code_git_blob_sha1): follow the reused_from records through manifests that still hash to their pins, to the one
    that built the payload (or a record that names its code); {} when the chain breaks."""
    rec = entry.get("reused_from")
    for _ in range(16):
        if not isinstance(rec, dict) or not isinstance(rec.get("dir"), str):
            return {}
        try:
            blob = (Path(rec["dir"]) / "manifest.json").read_bytes()
        except OSError:
            return {}
        if sha_bytes(blob) != rec.get("manifest_sha256"):
            return {}
        m = json.loads(blob)
        e = next((x for x in m.get("fields") or [] if isinstance(x, dict) and x.get("name") == name), None)
        if e is None:
            return {}
        if not isinstance(e.get("reused_from"), dict):
            return {k: m.get(k) for k in ("code_sha256", "code_sha256_lf", "code_git_blob_sha1")}
        rec = e["reused_from"]
        if rec.get("code_git_blob_sha1"):
            return {k: rec.get(k) for k in ("code_sha256", "code_sha256_lf", "code_git_blob_sha1")}
    return {}


def producer_fingerprints(source: bytes) -> dict:
    """{field group: SHA-256 of its producing code} of a builder source (R1 M-6): the group's FIELD_PRODUCERS and
    every module-level definition (function, class, constant, import) they reach by name, each as its AST without
    docstrings (code_fingerprint.fingerprints). None for a group whose builder functions the source lacks; ValueError
    when it does not parse."""
    return code_fingerprint.fingerprints(source, FIELD_PRODUCERS, PRODUCER_ORCHESTRATION)


# ---------------------------------------------------------------------------
# Field module reuse (v8 C-3): research_fields_sec.py and research_fields_holdings.py
# ---------------------------------------------------------------------------
# A module exports PRODUCERS {producer group: (entry names,)}, HOST_HANDLES (how it reads this builder: ``h.X`` or
# ``ns["X"]``), producer_group(name), field_spec(name), reuse_inputs(name, options) and entry_inputs(entry).
MODULE_FORMULA_KEYS = ("formula_id", "units", "clock", "staleness", "source_columns", "definition", "domain")
REUSE_MODULE_RULE = ("a field module's field is copied from the prior fields directory iff: the prior manifest is complete "
                     "and bound to this role; same field; same formula (formula_id, units, clock, staleness, "
                     "source_columns, definition, domain of the module's spec); same producing code (SHA-256 of the AST, "
                     "docstrings dropped, of the module's PRODUCERS group and every module-level definition it reaches, "
                     "plus the closure of every builder definition it reads through its host handle, for the module and "
                     "builder code that produced the prior payload: the entry's producer and the builder named by the "
                     "manifest or by reused_from.host_code, found as this code or its git blob); same inputs (the stage "
                     "manifest SHA-256s, and the SEC identity bridge, recorded by the entry); every field it requires "
                     "is reused; copied bytes re-hash to the prior pin")


def module_source(module) -> bytes:
    """A field module's source bytes: what its computed entries' producer identity pins and --reuse compares with."""
    return Path(module.__file__).read_bytes()


def module_code_identity(module) -> dict:
    """``code_identity`` of a field module's source (the ``producer`` of the entries it computes)."""
    return code_identity_of(module_source(module))


def module_fingerprints(module, source: bytes, host_source: bytes) -> dict:
    """{producer group: SHA-256} of a field module's source bound into a builder source (code_fingerprint.Host)."""
    host = code_fingerprint.Host(host_source.replace(b"\r\n", b"\n"), module.HOST_HANDLES, PRODUCER_ORCHESTRATION)
    return code_fingerprint.fingerprints(source.replace(b"\r\n", b"\n"), module.PRODUCERS, host=host)


def module_formula(record: dict) -> dict:
    """The definition keys a module's field spec and its manifest entry share (tuples compared as lists)."""
    return json.loads(canonical({k: list(record[k]) if k == "domain" else record[k]
                                 for k in MODULE_FORMULA_KEYS if k in record}))


def reuse_module_fields(module, prior_dir: Path, prior_sha256: str | None, role: Role, names: list, output: Path,
                        budget: Budget, *, options: dict, reused_names: set, hardlink: bool = False):
    """Decide (``REUSE_MODULE_RULE``) and copy the reusable ``names`` of a field module from a prior fields directory.

    Returns ({name: (entry, files pin)}, {name: why not reused}, the prior manifest's source_checks)."""
    prior, prior_sha = load_prior(prior_dir, prior_sha256, role)
    entries = {e["name"]: e for e in prior.get("fields", []) if isinstance(e, dict) and "name" in e}
    shape = [role.n_dates, role.n]
    manifest_code = {k: prior.get(k) for k in ("code_sha256", "code_sha256_lf", "code_git_blob_sha1")}
    current_module = module_source(module)
    current = module_fingerprints(module, current_module, builder_source())
    label = Path(module.__file__).name
    found = {}   # (module lf, builder lf) -> (fingerprints or None, how)

    def producers_of(e):
        ident_m = e.get("producer") if isinstance(e.get("producer"), dict) else {}
        rec = e.get("reused_from") if isinstance(e.get("reused_from"), dict) else {}
        ident_h = rec.get("host_code") if isinstance(rec.get("host_code"), dict) else manifest_code
        key = (ident_m.get("code_sha256_lf"), ident_h.get("code_sha256_lf"))
        if key not in found:
            src_m, how_m = producer_source(ident_m, current_module, "this module")
            src_h, how_h = producer_source(ident_h)
            fps, how = None, f"{label}: {how_m}; builder: {how_h}"
            if src_m is not None and src_h is not None:
                try:
                    fps = module_fingerprints(module, src_m, src_h)
                except ValueError as err:
                    how = str(err)
            found[key] = (fps, how)
        return (ident_m, ident_h, *found[key])

    reasons, candidates = {}, {}
    for name in names:
        e, pin = entries.get(name), (prior.get("files") or {}).get(f"{name}.f64")
        if e is None or pin is None:
            reasons[name] = "absent from the prior manifest"
            continue
        if (e.get("file") != f"{name}.f64" or e.get("dtype") != "<f8" or e.get("layout") != "date-major"
                or e.get("shape") != shape or e.get("sha256") != pin.get("sha256")
                or pin.get("bytes") != role.n_dates * role.n * 8):
            reasons[name] = "prior entry layout/shape/pin differs"
            continue
        spec = module.field_spec(name)
        if module_formula(e) != module_formula(spec):
            reasons[name] = "formula differs (formula id or definition changed)"
            continue
        group = module.producer_group(name)
        ident_m, ident_h, fps, how = producers_of(e)
        if fps is None:
            reasons[name] = f"producing code not recoverable: {how}"
            continue
        if fps.get(group) is None or fps[group] != current[group]:
            reasons[name] = f"producing code differs ({label} group {group} with the builder code it reads; {how})"
            continue
        if module.entry_inputs(e) != module.reuse_inputs(name, options):
            reasons[name] = "inputs differ (stage manifest or bridge SHA-256)"
            continue
        missing = [d for d in spec.get("requires", []) if d not in reused_names]
        if missing:
            reasons[name] = f"depends on {', '.join(missing)}, which is recomputed"
            continue
        candidates[name] = (e, pin, ident_m, ident_h, fps[group], how)
    reused = {}
    for name in names:
        if name not in candidates:
            continue
        e, pin, ident_m, ident_h, producer, how = candidates[name]
        digest, size = _copy_payload(prior_dir / f"{name}.f64", output / f"{name}.f64", hardlink, budget)
        if digest != pin["sha256"] or size != pin["bytes"]:
            raise ValueError(f"--reuse: prior payload {name}.f64 does not match its manifest pin (corrupt prior dir)")
        entry = json.loads(canonical(e))
        # the code that produced the payload (the origin through chained reuse), never this run's code
        entry["reused_from"] = {"dir": str(prior_dir.resolve()), "manifest_sha256": prior_sha,
                                "producer": {k: ident_m.get(k) for k in ("module", "code_sha256", "code_sha256_lf",
                                                                         "code_git_blob_sha1")},
                                "host_code": {k: ident_h.get(k) for k in ("code_sha256", "code_sha256_lf",
                                                                          "code_git_blob_sha1")},
                                "producer_sha256": producer, "producer_code": how,
                                "inputs": module.entry_inputs(e), "payload_sha256": digest,
                                "mode": "hardlink" if hardlink else "copy"}
        reused[name] = (entry, {"bytes": size, "sha256": digest})
    budget.report("reuse-plan-module", module=label, reused=len(reused), computed=len(names) - len(reused))
    return reused, reasons, prior.get("source_checks") or {}


def merge_module_reuse(block: dict, order: list, names: list, reused: dict, reasons: dict, *, group: str,
                       prior_check=None) -> None:
    """Extend a manifest's reuse block with a field module's decisions (lists in ``order``); carry the prior source
    check of ``group`` when every one of ``names`` is reused (recorded as partial when only some are)."""
    done = set(block["reused"]) | {n for n in names if n in reused}
    listed = set(block["reused"]) | set(block["computed"]) | set(names)
    why = {**block["not_reused"], **{n: reasons[n] for n in names if n in reasons}}
    block["reused"] = [n for n in order if n in done]
    block["computed"] = [n for n in order if n in listed and n not in done]
    block["not_reused"] = {n: why[n] for n in order if n in why}
    lf = {**block["producing_code_sha256_lf"],
          **{n: reused[n][0]["reused_from"]["producer"]["code_sha256_lf"] for n in names if n in reused}}
    block["producing_code_sha256_lf"] = {n: lf[n] for n in order if n in lf}
    block.setdefault("module_rule", REUSE_MODULE_RULE)
    if prior_check is None or not any(n in reused for n in names):
        return
    if all(n in reused for n in names):
        block.setdefault("_source_checks", {})[group] = prior_check
        block["source_checks_from_prior"] = sorted(set(block["source_checks_from_prior"]) | {group})
    else:
        block["prior_source_checks_of_partial_groups"][group] = prior_check


def inputs_sha256(entry: dict, role_sha256: str) -> str:
    """SHA-256 of the field's inputs: the role pin, the declared lag and every (normalised path, SHA-256) source."""
    srcs = sorted([_norm(x["path"]), x.get("sha256")] for x in entry.get("sources") or [])
    return sha_bytes(canonical({"role_manifest_sha256": role_sha256, "fund_lag_sessions": entry.get("fund_lag_sessions"),
                                "sources": srcs}).encode("utf-8"))


def _copy_payload(src: Path, dst: Path, hardlink: bool, budget: Budget) -> tuple[str, int]:
    """Copy (or hardlink) one payload exclusively, hashing the bytes that land in ``dst``."""
    if hardlink:
        try:
            os.link(src, dst)
            return sha_file(dst, budget), dst.stat().st_size
        except OSError:
            if dst.exists():
                raise
    h, size = hashlib.sha256(), 0
    with src.open("rb") as fi, dst.open("xb") as fo:
        while chunk := fi.read(8 << 20):
            fo.write(chunk)
            h.update(chunk)
            size += len(chunk)
            budget.check("reuse-copy")
        fo.flush()
        os.fsync(fo.fileno())
    return h.hexdigest(), size


def load_prior(prior_dir: Path, prior_sha256: str | None, role: Role) -> tuple[dict, str]:
    """(prior manifest, its SHA-256): a complete fields manifest bound to this role (manifest, sessions, ids, member),
    checked against --reuse-sha256 when given."""
    blob = (prior_dir / "manifest.json").read_bytes()
    prior_manifest_sha = sha_bytes(blob)
    if prior_sha256 is not None and prior_manifest_sha != prior_sha256.lower():
        raise ValueError("--reuse: prior fields manifest SHA-256 does not match --reuse-sha256")
    prior = json.loads(blob)
    if prior.get("schema") != SCHEMA or prior.get("status") != "complete":
        raise ValueError("--reuse: prior directory is not a complete atx.research-role-fields/v1 manifest")
    bound = prior.get("role") or {}
    if (bound.get("manifest_sha256") != role.manifest_sha256 or bound.get("sessions_sha256") != role.sessions_sha256
            or bound.get("ids_sha256") != role.ids_sha256
            or bound.get("member_sha256") != role.manifest["files"]["member.u8"]["sha256"]):
        raise ValueError("--reuse: prior fields manifest is bound to a different role (manifest/sessions/ids/member)")
    return prior, prior_manifest_sha


def reuse_fields(prior_dir: Path, prior_sha256: str | None, role: Role, selected: list, output: Path, budget: Budget,
                 *, roots: dict, lag: int, tickerhistory: Path, hardlink: bool = False):
    """Decide (``REUSE_RULE``) and copy the reusable fields of a prior fields directory into ``output``.

    Returns ({name: (entry, files pin)}, reuse block for the manifest, th_known or None)."""
    prior, prior_manifest_sha = load_prior(prior_dir, prior_sha256, role)
    entries = {e["name"]: e for e in prior.get("fields", [])}
    prior_revisions = prior.get("formula_revisions") or {}   # absent: every field at revision 1 (all manifests so far)
    shape = [role.n_dates, role.n]
    hashed = {}      # normalised path -> SHA-256 (each distinct source hashed once)
    th_known = None
    directory_checked = []

    def sv_directory_ok(x: dict, allowed) -> str | None:
        """A CNMS directory source of sv_ratio126 (v8 C-3, ``REUSE_DIRECTORY_RULE``): the files this run reads for
        this role (``sv_window_files`` of the current listing) re-hash to the recorded list SHA-256, count and dates."""
        path = _norm(x["path"])
        if not any(_under(path, r) for r in allowed):
            return f"source {x['path']} is outside this run's inputs for group finra_sv"
        d = Path(x["path"])
        if not d.is_dir():
            return f"source {x['path']} is missing"
        key = ("sv-directory", path)
        if key not in hashed:
            try:
                listing = sv_listing(d)
            except ValueError as err:
                return f"source {x['path']}: {err}"
            names = dict(listing)
            need = sv_window_files(listing, role.days)[3]
            files = []
            for day in need:
                f = d / names[day]
                before = identity(f)
                digest = sha_file(f, budget)
                if identity(f) != before:
                    raise ValueError(f"--reuse: source {f} changed while hashing")
                files.append([names[day], before[2], digest])
            files.sort()
            budget.report("reuse-source-directory", path=str(d), files=len(files))
            hashed[key] = {"files_read": len(files), "files_sha256": sha_bytes(canonical(files).encode("utf-8")),
                           "first_file_date": date_of(need[0]) if need else None,
                           "last_file_date": date_of(need[-1]) if need else None}
        directory_checked.append(path)
        if any(x.get(k) != v for k, v in hashed[key].items()):
            return f"source {x['path']} changed (the CNMS files this run reads differ from the prior list)"
        return None

    def source_ok(name: str, entry: dict):
        nonlocal th_known
        srcs = entry.get("sources")
        if not isinstance(srcs, list) or not srcs:
            return "no recorded sources"
        group = ALL_FIELDS[name]["group"]
        allowed = roots.get(f"{group}:{ALL_FIELDS[name].get('kind')}", roots.get(group, []))
        for x in srcs:
            if (group == "finra_sv" and isinstance(x, dict) and isinstance(x.get("path"), str) and "sha256" not in x
                    and x.get("files_list_rule") == SV_FILES_LIST_RULE):
                why = sv_directory_ok(x, allowed)
                if why is not None:
                    return why
                continue
            if not isinstance(x, dict) or "path" not in x or "sha256" not in x:
                return "a source without a file SHA-256 (not reusable by rule)"
            path = _norm(x["path"])
            if not any(_under(path, r) for r in allowed):
                return f"source {x['path']} is outside this run's inputs for group {ALL_FIELDS[name]['group']}"
            if path not in hashed:
                f = Path(x["path"])
                if not f.is_file():
                    return f"source {x['path']} is missing"
                if "bytes" in x and f.stat().st_size != x["bytes"]:
                    hashed[path] = None
                else:
                    before = identity(f)
                    budget.report("reuse-source-hash", path=str(f), bytes=before[2])
                    digest = sha_file(f, budget)
                    if identity(f) != before:
                        raise ValueError(f"--reuse: source {f} changed while hashing")
                    hashed[path] = digest
                    if _norm(tickerhistory) == path:
                        th_known = (before, digest)
            if hashed[path] != x["sha256"]:
                return f"source {x['path']} changed (SHA-256 or size differs from the prior record)"
        return None

    current_fp = producer_fingerprints(builder_source().replace(b"\r\n", b"\n"))
    producers = {}   # code_sha256_lf -> (fingerprints or None, how the producing code was found)
    sic_table = []   # this builder's SIC mapping table SHA-256, computed once when a grp_* field asks

    def producer_of(entry: dict, name: str):
        """(code identity, group fingerprints or None, how) of the code that produced a prior entry's payload: its
        reused_from record for a payload the prior run itself reused (an older record without a blob id: the chain,
        legacy_origin), else the prior manifest's builder."""
        origin = entry.get("reused_from") if isinstance(entry.get("reused_from"), dict) else prior
        ident = {k: origin.get(k) for k in ("code_sha256", "code_sha256_lf", "code_git_blob_sha1")}
        if origin is not prior and not ident["code_git_blob_sha1"]:
            ident = legacy_origin(entry, name) or ident
        key = ident["code_sha256_lf"]
        if key not in producers:
            src, how = producer_source(ident)
            fps = None
            if src is not None:
                try:
                    fps = producer_fingerprints(src)
                except ValueError as err:
                    how = str(err)
            producers[key] = (fps, how)
        return (ident, *producers[key])

    def sic_table_ok() -> bool:
        if not sic_table:
            sic_table.append(sic_mapping()[2]["table_sha256"])
        prior_sic = (((prior.get("source_checks") or {}).get("issuer") or {}).get("sic_mapping") or {})
        return prior_sic.get("table_sha256") == sic_table[0]

    reasons, candidates = {}, {}
    for name in selected:
        e = entries.get(name)
        pin = (prior.get("files") or {}).get(f"{name}.f64")
        if e is None or pin is None:
            reasons[name] = "absent from the prior manifest"
            continue
        if (e.get("file") != f"{name}.f64" or e.get("dtype") != "<f8" or e.get("layout") != "date-major"
                or e.get("shape") != shape or e.get("sha256") != pin.get("sha256")
                or pin.get("bytes") != role.n_dates * role.n * 8):
            reasons[name] = "prior entry layout/shape/pin differs"
            continue
        want = formula_id(name, spec_definition(name, lag))
        got = formula_id(name, entry_definition(e), prior_revisions.get(name, 1))
        if got != want:
            reasons[name] = "formula id differs (definition or FORMULA_REVISION changed)"
            continue
        if ALL_FIELDS[name].get("lagged") and e.get("fund_lag_sessions") not in (None, lag):
            reasons[name] = "declared fund lag differs"
            continue
        group = ALL_FIELDS[name]["group"]
        ident, fps, how = producer_of(e, name)
        if fps is None:
            reasons[name] = f"producing code not recoverable: {how}"
            continue
        if fps.get(group) is None or fps[group] != current_fp[group]:
            reasons[name] = f"producing code differs (builder closure of group {group}; prior code from {how})"
            continue
        if ALL_FIELDS[name].get("kind") == "grp" and not sic_table_ok():
            reasons[name] = "SIC mapping table differs (atx_db.reference_classifications)"
            continue
        why = source_ok(name, e)
        if why:
            reasons[name] = why
            continue
        candidates[name] = (e, pin, want, ident, fps[group], how)
    changed = True
    while changed:  # a reused field's inputs include the fields it depends on: those must be reused too
        changed = False
        for name in list(candidates):
            missing = [d for d in ALL_FIELDS[name].get("requires", []) if d not in candidates]
            if missing:
                reasons[name] = f"depends on {', '.join(missing)}, which is recomputed"
                del candidates[name]
                changed = True
    reused = {}
    for name in selected:
        if name not in candidates:
            continue
        e, pin, fid, ident, producer, how = candidates[name]
        digest, size = _copy_payload(prior_dir / f"{name}.f64", output / f"{name}.f64", hardlink, budget)
        if digest != pin["sha256"] or size != pin["bytes"]:
            raise ValueError(f"--reuse: prior payload {name}.f64 does not match its manifest pin (corrupt prior dir)")
        entry = json.loads(canonical(e))
        # the code that produced the payload (the origin through chained reuse), never this builder's identity
        entry["reused_from"] = {"dir": str(prior_dir.resolve()), "manifest_sha256": prior_manifest_sha,
                                "code_sha256_lf": ident["code_sha256_lf"], "code_sha256": ident["code_sha256"],
                                "code_git_blob_sha1": ident["code_git_blob_sha1"], "producer_sha256": producer,
                                "producer_code": how, "formula_id": fid,
                                "inputs_sha256": inputs_sha256(e, role.manifest_sha256), "payload_sha256": digest,
                                "mode": "hardlink" if hardlink else "copy"}
        reused[name] = (entry, {"bytes": size, "sha256": digest})
    groups = {}
    for name in selected:
        groups.setdefault(ALL_FIELDS[name]["group"], []).append(name)
    prior_checks = prior.get("source_checks") or {}
    carried, partial = {}, {}
    for g, names in groups.items():
        key = REUSE_SOURCE_CHECK_KEYS.get(g)
        if g == "finra":
            carried.update({n: prior_checks[n] for n in names if n in reused and n in prior_checks})
        elif key in prior_checks and all(n in reused for n in names):
            carried[key] = prior_checks[key]
        elif key in prior_checks and any(n in reused for n in names):
            partial[key] = prior_checks[key]  # this run's group check covers only the recomputed fields
    block = {"from": str(prior_dir.resolve()), "manifest_sha256": prior_manifest_sha,
             "code_sha256_lf": prior.get("code_sha256_lf"), "rule": REUSE_RULE, "code_rule": REUSE_CODE_RULE,
             "producing_code_sha256_lf": {n: reused[n][0]["reused_from"]["code_sha256_lf"]
                                          for n in selected if n in reused},
             "mode": "hardlink" if hardlink else "copy",
             "reused": [n for n in selected if n in reused], "computed": [n for n in selected if n not in reused],
             "not_reused": {n: reasons[n] for n in selected if n in reasons},
             "source_checks_from_prior": sorted(carried),
             "prior_source_checks_of_partial_groups": partial, "_source_checks": carried}
    if directory_checked:  # v8 C-3: only when a directory source was checked (other reuse blocks are unchanged)
        block["directory_rule"] = REUSE_DIRECTORY_RULE
    budget.report("reuse-plan", reused=len(block["reused"]), computed=len(block["computed"]))
    return reused, block, th_known


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--role", required=True, type=Path, help="published recent-research role directory")
    p.add_argument("--role-sha256", required=True, help="SHA-256 of the role's manifest.json")
    p.add_argument("--output", required=True, type=Path, help="new exclusive output directory")
    p.add_argument("--fields", default=",".join(DEFAULT_FIELDS),
                   help="comma-separated subset of: " + ",".join(ALL_FIELDS) + " (default: the point-in-time fields "
                        + ",".join(DEFAULT_FIELDS) + "; non-point-in-time fields, the issuer fields "
                        + ",".join(ISSUER_FIELDS) + " and " + ",".join(SV_FIELDS) + " are produced only when named)")
    p.add_argument("--max-rss-mib", type=int, default=700)
    p.add_argument("--max-seconds", type=float, default=1800.0)
    p.add_argument("--finra", type=Path, default=DEFAULT_FINRA, help="FINRA short-interest root (asof/, dissemination_schedule.csv)")
    p.add_argument("--tickerhistory", type=Path, default=DEFAULT_TICKERHISTORY)
    p.add_argument("--lake", type=Path, default=DEFAULT_LAKE, help="research lake snapshot directory")
    p.add_argument("--identity-bridge", type=Path, help="published identity-bridge directory (issuer fields)")
    p.add_argument("--identity-bridge-sha256", help="SHA-256 of the identity bridge's manifest.json")
    p.add_argument("--fund-events", type=Path, help="published fundamental events directory (fundamental and group fields)")
    p.add_argument("--fund-events-sha256", help="SHA-256 of the fundamental events' manifest.json")
    p.add_argument("--fund-lag-sessions", type=int, default=FUND_LAG_SESSIONS_DECLARED,
                   help="declared extra lag (role sessions) after the first session whose close follows a filing clock "
                        f"(default {FUND_LAG_SESSIONS_DECLARED}, v4-prereg R2)")
    p.add_argument("--finra-short-volume", type=Path,
                   help="FINRA daily short sale volume directory (CNMSshvolYYYYMMDD.txt.gz + manifest.csv; sv_ratio126)")
    p.add_argument("--sic-events", type=Path,
                   help="atx-db fundamentals stage directory (sic_events.parquet): the SIC table of the grp_* fields "
                        "instead of --fund-events' (role universe linked-operating-v3 reads the same table)")
    p.add_argument("--sic-events-sha256", help="SHA-256 of the fundamentals stage's manifest.json")
    p.add_argument("--reuse", type=Path, help="prior fields directory: copy unchanged fields instead of recomputing")
    p.add_argument("--reuse-sha256", help="SHA-256 of the prior fields directory's manifest.json (checked when given)")
    p.add_argument("--reuse-hardlink", action="store_true", help="hardlink reused payloads instead of copying them")
    for m in FIELD_MODULES:  # W5a registry hook: each opt-in module adds its own options
        m.add_arguments(p)
    a = p.parse_args(argv)
    run(a.role, a.role_sha256, a.output, [x.strip() for x in a.fields.split(",") if x.strip()],
        max_rss_mib=a.max_rss_mib, max_seconds=a.max_seconds, finra=a.finra, tickerhistory=a.tickerhistory, lake=a.lake,
        identity_bridge=a.identity_bridge, identity_bridge_sha256=a.identity_bridge_sha256,
        fund_events=a.fund_events, fund_events_sha256=a.fund_events_sha256, fund_lag_sessions=a.fund_lag_sessions,
        finra_short_volume=a.finra_short_volume, reuse=a.reuse, reuse_sha256=a.reuse_sha256,
        reuse_hardlink=a.reuse_hardlink, module_options={k: getattr(a, k) for m in FIELD_MODULES for k in m.OPTIONS},
        sic_events=sic_stage_dir(a.sic_events) if a.sic_events is not None else None,
        sic_events_sha256=a.sic_events_sha256)


# W5a registry hook (placeholder: lane W5a registers research_fields_sec.py here)
# W5b registry hook (platform v7): the 13F / FTD / Reg SHO / short-volume-ext fields of research_fields_holdings.py.
# register() wraps run() and main() in this namespace; nothing changes unless one of its fields is requested.
import research_fields_holdings as _holdings  # noqa: E402  (same directory, as prepare_recent_research imports this)
_holdings.register(globals())
# Platform v8 F-1 registry hook: the price and long-lookback fields of research_fields_price.py, an opt-in FIELD_MODULES
# module like research_fields_sec.py (registry after every field above); nothing changes unless one is requested.
import research_fields_price as _price  # noqa: E402  (same directory; it does not import this module)
FIELD_MODULES.append(_price.bind(globals()))
ALL_FIELDS.update(_price.FIELDS)
# Platform v8 F-3 registry hook: the v8 fields of research_fields_v8.py (grp_ff12f49), an opt-in FIELD_MODULES module;
# its bind() appends its registry to ALL_FIELDS itself, so no ALL_FIELDS statement joins the SEC module's host closure.
import research_fields_v8 as _v8  # noqa: E402  (same directory; it does not import this module)
FIELD_MODULES.append(_v8.bind(globals()))

if __name__ == "__main__":
    main()
