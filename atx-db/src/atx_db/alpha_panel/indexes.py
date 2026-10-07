"""Stage ``indexes_v2`` (S3.6; P9 request A.1-A.5): rule-based Russell 1000 / 2000 / 3000 and S&P-500-like proxies,
point in time at every row's own ``available_at``.

Official constituent lists and weights are licensed (FTSE Russell, S&P DJI); their public pages are terms-gated,
so nothing is scraped and no overlap with the official lists is measured (documented gap; the LIC lane's index
adapter takes the licensed file when bought). The proxies follow the public methodologies on our data.

Eligible line (both families), on the rank date's panel row, from dated evidence only:
* R7.1 (a) type: the panel ``security_type`` only where ``security_type_basis = 'finra_name'``; a line typed only by
  the 2026-09-18 Nasdaq Trader directory snapshot (or by the vendor) is not eligible. ``is_etf`` = type ETF / fund /
  ETN without ``directory_etf``; ``is_adr`` / ``is_lp`` from that type. No ``directory_*`` column decides selection.
* R7.2 (d) issuer: a CIK only from ``link_tier`` strict / name. A backfill-linked line (``snapshot-run-backfill-v1``:
  the CIK the 2026-09-20 SEC ticker snapshot gives a line alive then) keeps the line but drops the backfill CIK and
  every issuer fact that came through it (``filer_regime`` -> FPI / ADR-likely, ``sec_last_periodic_date``, ``sic``
  -> SPAC / royalty trust, ``sale_ttm``, ``ni_*``). With ``--pit-links`` (link days computed without backfill
  withholding, schema of ``identity/links_combined.parquet``; a non-backfill tier known by the rank mark) the line is
  name-linked to that CIK: issuer facts from its own row when the two CIKs agree, else from a strict / name-linked
  row of that CIK on the date, else it is unlinked (counted). Otherwise it is unlinked: its own company, operating by
  the unlinked rule.
* then as v1: type common / common_unverified / REIT, not ADR or ADR-likely, not a foreign private issuer (the
  US-domicile proxy), not LP or royalty trust (Russell excludes both; business development companies stay),
  operating (``panel.py``'s ``is_operating`` formula recomputed from the inputs above), not an index line, listed on
  NYSE, NYSE American, Nasdaq, NYSE Arca or Cboe BZX (``exchange``, with the dated FINRA repair), raw close >= $1,
  ``market/shares_daily.shrout`` known.
Company = CIK (an unlinked line is its own company); company ME = sum of its eligible lines' shrout x raw close on the
rank date. Every eligible line of a selected company is a member (share classes, as both indexes do).

Russell proxy (rule ``russell-proxy-v2``; FTSE Russell US Indexes construction and methodology):
* rank days (``RUSSELL``): 2018-05-11, 2019-05-10, 2020-05-08, 2021-05-07, 2022-05-06, then the last business day
  of April (2023-04-28 ... 2026-04-30); effective after the close of the fourth Friday of June. From 2026 the
  reconstitution is semi-annual (rank day = last business day of October, effective after the second Friday of
  December, 2026-12-11).
* companies with total ME >= $30M ranked by ME; R3000 = the top 3,000 (no band at 3,000); R1000 / R2000 split at
  rank 1,000 with the +-2.5% cumulative market-cap percentile band: a prior R1000 company stays while its
  cumulative percentile is <= p1000 + 2.5%; a prior R2000 company moves up only below p1000 - 2.5%; others by
  rank. The first proxy year (2018) has no prior membership, so no band. IPO quarterly additions and spin-off
  additions are not modelled.

S&P-500-like proxy (rule ``sp500-proxy-v2``): quarterly, rank date = last session of Feb / May / Aug / Nov,
effective after the third Friday of Mar / Jun / Sep / Dec (the session before when that Friday is a holiday). New
entrants must have positive GAAP earnings (``ni_ttm`` > 0 and ``ni_q`` > 0, as reported; unknown when share classes
disagree) and annual dollar volume >= 0.75 x company ME (``market/liquidity`` ``adv_63`` x 252, known for every
line; float is unknown, so total ME stands in); existing members stay while ranked <= 600 (buffer) without the
earnings screen; the index is filled to 500 companies by ME rank. No committee discretion, sector balance or float.

Point in time (R1-R5, kernel ``private_index_pit``; the house mark is 22:00 UTC):
* R1 ``constituents``: every member selected on the rank-date snapshot, stamped at the rank date's mark. No column
  depends on ``last_session``, ``delisting_date`` or any later session.
* R2 ``effective_from`` = the first NYSE session after the reconstitution date (``exchange_calendar``: rules, checked
  equal to the vendor calendar at every build); ``effective_to`` = the next scheduled reconstitution date
  (``scheduled_reconstitutions``), also for the last one built. Neither moves when the vendor data is extended.
* R3 ``exits``: a member whose line ceased before its scheduled end (``delisting_date`` set, or ``last_session``
  before the vendor's last session; never ``last_session`` alone), stamped at the mark of the first session after
  its last session (the house ``delisting`` convention). Consumers take the latest vintage before their mark.
* R4 ``recon_summary``: stamped at the rank mark, counting rank-date member sets and exits stamped before it.
* R5 ``membership`` of session s: stamped max(rank mark, mark of the session before s), built only from vintages
  stamped by then.
A reconstitution is built whenever its rank date has panel data (2018 on; 2018-2019 carry the prior membership state
the band and the buffer need; acceptance and diagnostics foreground 2020 through the latest data).

Outputs (R8: parts by the UTC year of ``available_at``, ``<product>/available_year=YYYY/<product>.parquet``, each
listed in the manifest): ``constituents`` (``index_id, security_id, cik, effective_from, effective_to`` (inclusive),
``weight`` (line ME share of the index on the rank date), ``company_rank, rank_date, recon_date, basis,
available_at``); ``exits`` (``index_id, security_id, recon_date, last_session, available_at, exit_basis``);
``membership`` (``session_date, security_id, cik, r1000, r2000, r3000, sp500, available_at``); ``recon_summary`` (per
index and reconstitution: lines, companies, additions, deletions, one-way and cap-weighted turnover, breakpoints).
The legacy v1 stage ``indexes/`` (rules ``*-proxy-v1``) is never written; its readers keep it until they re-pin.
"""

from __future__ import annotations

import argparse
import bisect
import datetime as dt
import json
import sys
from pathlib import Path
from typing import Any

from . import common as C
from . import exchange_calendar as E
from . import market_common as M
from . import private_index_pit as P
from .security_master import listed_panel_sql

STAGE = "indexes_v2"
LEGACY_STAGE = "indexes"
SCHEMA = "atx.alpha-panel.indexes/v2"
RULES = P.RULES
CLOCK_RULE = "index-vintage-mark-v2"   # rank rows at the rank date's mark; exits / membership as R3 / R5 above
STALENESS_DAYS = 371                   # longest gap between scheduled reconstitutions (2018-06-22 -> 2019-06-28)
MODULES = ("indexes", "private_index_pit", "exchange_calendar", "market_common", "security_master", "common")
PRODUCTS = ("constituents", "exits", "membership", "recon_summary")
LISTED = ("XNYS", "XNAS", "XASE", "ARCX", "BATS")
RUSSELL = [("2018-05-11", "2018-06-22"), ("2019-05-10", "2019-06-28"), ("2020-05-08", "2020-06-26"),
           ("2021-05-07", "2021-06-25"), ("2022-05-06", "2022-06-24"), ("2023-04-28", "2023-06-23"),
           ("2024-04-30", "2024-06-28"), ("2025-04-30", "2025-06-27"), ("2026-04-30", "2026-06-26")]
RUSSELL_MIN_ME = 30e6
BAND = 0.025
SP_N, SP_BUFFER = 500, 600
SP_LIQ_RATIO = 0.75
R2000_COMPANIES = (1900, 2100)   # gate (task-MKT S3.6): R2000 proxy company count at every reconstitution
FOCUS_FROM = dt.date(2020, 1, 1)  # owner steering 2026-10-05: acceptance and diagnostics foreground 2020 on
INPUTS = ("panel", "market/market_shares_manifest.json", "market/market_liquidity_manifest.json", "security_master")

LAKE_STAGES = [
    {"name": "indexes_v2", "lane": "MKT", "schema": "atx.alpha-panel.indexes/v2",
     "module": "atx_db.alpha_panel.indexes", "args": ["build"],
     "inputs": ["panel", "market_shares", "market_liquidity", "security_master"],
     "outputs": [{"glob": "indexes_v2/constituents/available_year=*/constituents.parquet",
                  "view": "indexes_v2_constituents", "keys": ["index_id", "security_id", "recon_date"],
                  "vintage": "interval"},
                 {"glob": "indexes_v2/exits/available_year=*/exits.parquet", "view": "indexes_v2_exits",
                  "keys": ["index_id", "security_id", "recon_date"], "vintage": "event"},
                 {"glob": "indexes_v2/membership/available_year=*/membership.parquet",
                  "view": "indexes_v2_membership", "keys": ["session_date", "security_id"], "vintage": "daily"},
                 {"glob": "indexes_v2/recon_summary/available_year=*/recon_summary.parquet",
                  "view": "indexes_v2_recon_summary", "keys": ["index_id", "recon_date"], "vintage": "vintage"}],
     "staleness": "annual / semi-annual (Russell) and quarterly (S&P proxy) reconstitution", "vintage": "interval",
     "guard_gb": 0.9},
    # The published v1 stage, kept declared for its readers (universe, release_inputs, universe.registry.json).
    {"name": "indexes", "lane": "MKT", "schema": "atx.alpha-panel.indexes/v1", "module": None,
     "doc": "legacy v1 publication (rules russell-proxy-v1 / sp500-proxy-v1, built at 791767cd); immutable, never "
            "rebuilt: this module now builds indexes_v2",
     "inputs": ["panel", "market_shares", "market_liquidity", "security_master"],
     "outputs": [{"glob": "indexes/constituents.parquet", "view": "indexes_constituents", "vintage": "interval"},
                 {"glob": "indexes/membership/year=*/membership.parquet", "view": "indexes_membership",
                  "vintage": "daily"},
                 {"glob": "indexes/recon_summary.parquet", "view": "indexes_recon_summary", "clock": None}],
     "staleness": "annual (Russell) / quarterly (S&P proxy) reconstitution", "vintage": "interval",
     "guard_gb": 0.4},
]


# ---------------------------------------------------------------- selection rules (pure)
def russell_assign(companies: list[tuple[Any, float]], prev_r1000: set, prev_r2000: set,
                   band: float = BAND) -> tuple[dict[Any, tuple[str, int, float]], float]:
    """``{company: (R1000|R2000, rank, cumulative ME share)}`` for the top 3,000 of ``(company, me)`` (ME >= $30M),
    with the +-``band`` cumulative-percentile band around rank 1,000; also returns p1000."""
    ranked = sorted((c for c in companies if c[1] >= RUSSELL_MIN_ME), key=lambda c: (-c[1], str(c[0])))
    total = sum(me for _, me in ranked) or 1.0
    cum, out, p1000 = 0.0, {}, None
    cums = []
    for _, me in ranked:
        cum += me
        cums.append(cum / total)
    p1000 = cums[min(999, len(cums) - 1)] if cums else 0.0
    for i, ((key, _), cp) in enumerate(zip(ranked[:3000], cums[:3000]), start=1):
        if key in prev_r1000:
            idx = "R1000" if cp <= p1000 + band else "R2000"
        elif key in prev_r2000:
            idx = "R1000" if cp < p1000 - band else "R2000"
        else:
            idx = "R1000" if i <= 1000 else "R2000"
        out[key] = (idx, i, cp)
    return out, p1000


def sp_select(companies: list[tuple[Any, float, bool]], prev: set, n: int = SP_N,
              buffer: int = SP_BUFFER) -> dict[Any, tuple[int, str]]:
    """``{company: (rank, basis)}`` of an S&P-500-like set from ``(company, me, passes_entry_screens)``."""
    ranked = sorted(companies, key=lambda c: (-c[1], str(c[0])))
    rank = {c[0]: i for i, c in enumerate(ranked, start=1)}
    keep = sorted((k for k in prev if k in rank and rank[k] <= buffer), key=lambda k: rank[k])[:n]
    out = {k: (rank[k], "buffer_stay") for k in keep}
    for key, _, ok in ranked:
        if len(out) >= n:
            break
        if key not in out and ok:
            out[key] = (rank[key], "entry")
    return out


def third_friday(y: int, m: int) -> dt.date:
    d = dt.date(y, m, 15)
    return d + dt.timedelta(days=(4 - d.weekday()) % 7)


def sp_schedule(first: dt.date, last: dt.date) -> list[tuple[dt.date, dt.date]]:
    """(rank month end, rebalance Friday) for every quarter whose rebalance falls in [first, last]."""
    out = []
    for y in range(first.year, last.year + 1):
        for m in (3, 6, 9, 12):
            reb = third_friday(y, m)
            rank_month_end = dt.date(y, m, 1) - dt.timedelta(days=1)
            if first <= rank_month_end and reb <= last:
                out.append((rank_month_end, reb))
    return out


def russell_gate(summary: list[dict[str, Any]], bounds: tuple[int, int] = R2000_COMPANIES) -> dict[str, Any]:
    """GC9 gate status: the R2000 proxy holds ``bounds`` companies at every reconstitution (lines reported too)."""
    r2 = [s for s in summary if s["index_id"] == "R2000P"]
    outside = {str(s["recon_date"]): s["companies"] for s in r2 if not bounds[0] <= s["companies"] <= bounds[1]}
    return {"criterion": f"R2000 proxy companies in [{bounds[0]}, {bounds[1]}] at every reconstitution",
            "pass": bool(r2) and not outside, "outside": outside,
            "companies": {str(s["recon_date"]): s["companies"] for s in r2},
            "lines": {str(s["recon_date"]): s["lines"] for s in r2},
            "official_overlap": "GAP: FTSE Russell / S&P DJI constituent lists are licensed (not measured)"}


def last_session_on_or_before(cal: list[dt.date], d: dt.date) -> dt.date | None:
    i = bisect.bisect_right(cal, d)
    return cal[i - 1] if i else None


def scheduled_reconstitutions(first_year, last_year):
    """Independent methodology schedule, never clipped to a vendor endpoint.

    ``last_year`` is a source-plan horizon with an independently authenticated
    future exchange calendar, not the last observed price year.
    """
    if last_year < first_year:
        raise ValueError('invalid independent schedule horizon')
    russell = {dt.date.fromisoformat(recon) for _, recon in RUSSELL
               if first_year <= int(recon[:4]) <= last_year}
    for year in range(max(2026,first_year),last_year+1):
        friday=third_friday(year,6)+dt.timedelta(days=7)
        russell.add(friday)
        russell.add(third_friday(year,12)-dt.timedelta(days=7))
    sp=[third_friday(year,month) for year in range(first_year,last_year+1) for month in (3,6,9,12)]
    return {**{index:sorted(russell) for index in ('R1000P','R2000P','R3000P')},'SP500P':sp}


def russell_ranks(last_year: int) -> list[tuple[dt.date, dt.date]]:
    """Nominal (rank day, reconstitution day) of every Russell proxy reconstitution through ``last_year``: ``RUSSELL``,
    then semi-annual from 2026 (October rank / second Friday of December; from 2027 April / fourth Friday of June),
    matching ``scheduled_reconstitutions``."""
    def last_weekday(y: int, m: int) -> dt.date:
        d = dt.date(y + m // 12, m % 12 + 1, 1) - dt.timedelta(days=1)
        return d - dt.timedelta(days=max(0, d.weekday() - 4))
    out = [(dt.date.fromisoformat(r), dt.date.fromisoformat(e)) for r, e in RUSSELL]
    for y in range(2026, last_year + 1):
        if y > 2026:
            out.append((last_weekday(y, 4), third_friday(y, 6) + dt.timedelta(days=7)))
        out.append((last_weekday(y, 10), third_friday(y, 12) - dt.timedelta(days=7)))
    return [p for p in out if p[1].year <= last_year]


def schedule(cal: list[dt.date], first_year: int, last_year: int) -> dict[str, list[dt.date]]:
    """``scheduled_reconstitutions`` on the exchange calendar: a nominal reconstitution day that is not a session (a
    holiday Friday, e.g. Juneteenth 2026-06-19 for the S&P proxy) moves to the last session before it. ``cal`` is
    the rules calendar and must cover every scheduled day, so no date depends on the vendor data's extent."""
    out = {}
    for index, days in scheduled_reconstitutions(first_year, last_year).items():
        if days and (days[0] < cal[0] or days[-1] > cal[-1]):
            raise ValueError(f"exchange calendar {cal[0]} .. {cal[-1]} does not cover the {index} schedule "
                             f"{days[0]} .. {days[-1]}")
        out[index] = sorted({last_session_on_or_before(cal, d) for d in days})
    return out


def rank_schedule(cal: list[dt.date], panel_days: list[dt.date]) -> list[tuple[str, dt.date, dt.date]]:
    """``(family, rank_date, recon_date)`` of every reconstitution whose exchange-adjusted rank date has panel data.
    A weekend month end may follow the last panel session while its Friday rank snapshot is already available."""
    have = set(panel_days)
    first, last = panel_days[0], panel_days[-1]
    out = []
    for rank_n, recon_n in russell_ranks(last.year + 1):
        rank_d = last_session_on_or_before(cal, rank_n)
        if rank_d in have:
            out.append(("russell", rank_d, last_session_on_or_before(cal, recon_n)))
    for rank_end, reb in sp_schedule(dt.date(first.year, 1, 1), dt.date(last.year + 1, 12, 31)):
        rank_d = last_session_on_or_before(cal, rank_end)
        if rank_d in have:
            out.append(("sp500", rank_d, last_session_on_or_before(cal, reb)))
    return out


# ---------------------------------------------------------------- data
_LINK_TIERS = "('strict', 'name')"
_COMMON = "('common', 'common_unverified', 'REIT')"
_EQUITY = "('common', 'common_unverified', 'ADR', 'REIT', 'LP')"
# provenance of each eligible line, kept for the measured receipt keys
PROVENANCE = ("security_type_basis", "directory_etf", "link_tier", "cik_source", "shrout_source")


def rank_sql(con, d: dt.date, pit_links: Path | None = None) -> str:
    """Every listed panel line on session d with the R7 (a) / (d) eligibility inputs, the verdict ``eligible`` and
    its provenance; ``eligible_v1`` is v1's verdict on the published panel flags (diagnostics only)."""
    root = C.build_root()
    panel = (root / "panel" / f"year={d.year}" / "*.parquet").as_posix()
    shares = (root / "market" / "shares_daily" / f"year={d.year}" / "shares_daily.parquet").as_posix()
    liq = (root / "market" / "liquidity" / f"year={d.year}" / "liquidity.parquet").as_posix()
    listed = ", ".join(f"'{x}'" for x in LISTED)
    mark = f"TIMESTAMP '{d} {C.MARK_HOUR_UTC:02d}:00:00'"
    share_cols = {r[0] for r in con.execute(f"DESCRIBE SELECT * FROM read_parquet('{shares}')").fetchall()}
    source = "s.shrout_source" if "shrout_source" in share_cols else "CAST(NULL AS VARCHAR) AS shrout_source"
    if pit_links is None:
        pl = "SELECT CAST(NULL AS BIGINT) AS security_id, CAST(NULL AS BIGINT) AS pit_cik, 0 AS pit_n WHERE false"
    else:
        # strict / name link days computed without backfill withholding (links_combined schema: strict tiers carry
        # their own labels, e.g. high / medium), known by the rank mark; two CIKs on one day = ambiguous
        pl = f"""SELECT security_id, CASE WHEN count(DISTINCT cik) = 1 THEN min(cik) END AS pit_cik,
                        count(DISTINCT cik) AS pit_n
                 FROM read_parquet('{Path(pit_links).as_posix()}')
                 WHERE DATE '{d}' BETWEEN "start" AND end_incl AND tier IN ('strict', 'high', 'medium', 'name')
                   AND cik IS NOT NULL AND available_at <= {mark}
                 GROUP BY 1"""
    return f"""
        WITH p AS (
            SELECT p.session_date, p.security_id, p.cik, p.link_tier, p.security_type, p.security_type_basis,
                   p.directory_etf, p.is_index, p.exchange, p.raw_close, p.filer_regime, p.sec_last_periodic_date,
                   p.sic, p.sale_ttm, p.finra_issue_name, p.ni_ttm, p.ni_q, p.is_adr, p.is_adr_likely, p.is_fpi,
                   p.is_lp, p.is_royalty_trust, p.is_operating, s.shrout, {source}, l.adv_63
            FROM ({listed_panel_sql(panel)}) p
            LEFT JOIN read_parquet('{shares}') s USING (session_date, security_id)
            LEFT JOIN read_parquet('{liq}') l USING (session_date, security_id)
            WHERE p.session_date = DATE '{d}'),
        pl AS ({pl}),
        fx AS (SELECT cik, filer_regime, sec_last_periodic_date, sic, sale_ttm, ni_ttm, ni_q FROM p
               WHERE link_tier IN {_LINK_TIERS} AND cik IS NOT NULL
               QUALIFY row_number() OVER (PARTITION BY cik ORDER BY security_id) = 1),
        k AS (
            SELECT p.*, pl.pit_cik,
                   CASE WHEN p.link_tier IN {_LINK_TIERS} AND p.cik IS NOT NULL THEN p.link_tier
                        WHEN p.link_tier = 'backfill' AND (pl.pit_cik = p.cik OR fx.cik IS NOT NULL) THEN 'pit_link'
                   END AS cik_source,
                   CASE WHEN p.link_tier = 'backfill' THEN
                        CASE WHEN pl.pit_cik IS NULL AND coalesce(pl.pit_n, 0) > 1 THEN 'ambiguous'
                             WHEN pl.pit_cik IS NULL THEN 'unlinked'
                             WHEN pl.pit_cik = p.cik THEN 'name_same_cik'
                             WHEN fx.cik IS NOT NULL THEN 'name_other_row'
                             ELSE 'name_no_facts' END
                   END AS backfill_relink,
                   fx.filer_regime AS x_regime, fx.sec_last_periodic_date AS x_periodic, fx.sic AS x_sic,
                   fx.sale_ttm AS x_sale_ttm, fx.ni_ttm AS x_ni_ttm, fx.ni_q AS x_ni_q
            FROM p LEFT JOIN pl USING (security_id)
            LEFT JOIN fx ON p.link_tier = 'backfill' AND fx.cik = pl.pit_cik),
        r AS (
            SELECT k.*,
                   CASE WHEN cik_source IN {_LINK_TIERS} THEN cik WHEN cik_source = 'pit_link' THEN pit_cik
                   END AS key_cik,
                   cik_source IN {_LINK_TIERS} OR (cik_source = 'pit_link' AND pit_cik = cik) AS own_facts,
                   CASE WHEN security_type_basis = 'finra_name' THEN security_type END AS type_dated
            FROM k),
        f AS (
            SELECT r.*,
                   CASE WHEN own_facts THEN filer_regime WHEN cik_source = 'pit_link' THEN x_regime END AS f_regime,
                   CASE WHEN own_facts THEN sec_last_periodic_date WHEN cik_source = 'pit_link' THEN x_periodic
                   END AS f_periodic,
                   CASE WHEN own_facts THEN sic WHEN cik_source = 'pit_link' THEN x_sic END AS f_sic,
                   CASE WHEN own_facts THEN sale_ttm WHEN cik_source = 'pit_link' THEN x_sale_ttm END AS f_sale_ttm,
                   CASE WHEN own_facts THEN ni_ttm WHEN cik_source = 'pit_link' THEN x_ni_ttm END AS f_ni_ttm,
                   CASE WHEN own_facts THEN ni_q WHEN cik_source = 'pit_link' THEN x_ni_q END AS f_ni_q,
                   coalesce(type_dated IN ('ETF', 'fund', 'ETN'), false) AS etf_dated
            FROM r),
        g AS (
            SELECT f.*,
                   coalesce(type_dated = 'spac', false)
                       OR (coalesce(f_sic = 6770, false) AND coalesce(f_sale_ttm, 0) = 0) AS spac_dated,
                   coalesce(type_dated = 'ADR', false)
                       OR (coalesce(f_regime = 'fpi_20f', false) AND coalesce(type_dated = 'common_unverified', false))
                       AS adr_likely_dated,
                   coalesce(f_regime IN ('fpi_20f', 'canadian_40f'), false) AS fpi_dated,
                   coalesce(f_sic = 6792, false) OR coalesce(upper(finra_issue_name) LIKE '%ROYALTY TR%', false)
                       AS royalty_dated,
                   coalesce(exchange IN ({listed}) AND raw_close >= 1.0 AND shrout > 0, false) AS market_ok
            FROM f),
        h AS (
            SELECT g.*,
                   coalesce(NOT is_index AND type_dated IN {_EQUITY} AND NOT etf_dated AND NOT spac_dated
                            AND (key_cik IS NULL OR coalesce(session_date - f_periodic <= 400, false)), false)
                       AS operating_dated,
                   -- counterfactual: the same rule keyed on the panel's own CIK and facts (a backfill CIK included)
                   coalesce(NOT is_index AND type_dated IN {_EQUITY} AND NOT etf_dated
                            AND NOT (coalesce(type_dated = 'spac', false)
                                     OR (coalesce(sic = 6770, false) AND coalesce(sale_ttm, 0) = 0))
                            AND (cik IS NULL OR coalesce(session_date - sec_last_periodic_date <= 400, false)), false)
                       AS operating_panel_cik,
                   coalesce(type_dated IN {_COMMON}, false) AND NOT adr_likely_dated AND NOT fpi_dated
                       AND NOT coalesce(type_dated = 'LP', false) AND NOT royalty_dated
                       AND NOT coalesce(is_index, false) AND market_ok AS screens_ok,
                   coalesce(security_type IN {_COMMON}, false) AND NOT coalesce(is_adr, false)
                       AND NOT coalesce(is_adr_likely, false) AND NOT coalesce(is_fpi, false)
                       AND NOT coalesce(is_lp, false) AND NOT coalesce(is_royalty_trust, false)
                       AND coalesce(is_operating, false) AND NOT coalesce(is_index, false) AND market_ok
                       AS eligible_v1
            FROM g)
        SELECT h.*, screens_ok AND operating_dated AS eligible FROM h"""


def snapshot(con, d: dt.date, pit_links: Path | None = None) -> list[dict[str, Any]]:
    """Eligible lines on session d (R7 (a) / (d)) with ME, earnings, liquidity and provenance; the full rank-date
    population stays in the temp table ``rank_lines`` for :func:`rank_census`."""
    con.execute(f"CREATE OR REPLACE TEMP TABLE rank_lines AS {rank_sql(con, d, pit_links)}")
    cur = con.execute(f"""
        SELECT security_id, key_cik AS cik, raw_close, shrout, shrout * raw_close AS me, f_ni_ttm AS ni_ttm,
               f_ni_q AS ni_q, adv_63, {", ".join(PROVENANCE)}
        FROM rank_lines WHERE eligible ORDER BY security_id""")
    cols = [c[0] for c in cur.description]
    return [dict(zip(cols, r, strict=True)) for r in cur.fetchall()]


def rank_census(con) -> dict[str, int]:
    """Population counts of the last :func:`snapshot` (all listed panel lines of the rank date)."""
    cur = con.execute("""
        SELECT count(*) AS listed_lines,
               count(*) FILTER (WHERE eligible) AS eligible_lines,
               count(*) FILTER (WHERE eligible_v1) AS v1_eligible_lines,
               count(*) FILTER (WHERE eligible_v1 AND NOT eligible
                                AND security_type_basis IS DISTINCT FROM 'finra_name') AS v1_eligible_dropped_by_a,
               count(*) FILTER (WHERE eligible_v1 AND NOT eligible
                                AND security_type_basis = 'finra_name') AS v1_eligible_dropped_other,
               count(*) FILTER (WHERE eligible AND NOT eligible_v1) AS eligible_not_v1,
               count(*) FILTER (WHERE link_tier = 'backfill') AS backfill_lines,
               count(*) FILTER (WHERE backfill_relink IN ('name_same_cik', 'name_other_row'))
                   AS backfill_relinked_by_name,
               count(*) FILTER (WHERE backfill_relink = 'name_no_facts') AS backfill_name_link_without_facts,
               count(*) FILTER (WHERE backfill_relink = 'ambiguous') AS backfill_ambiguous_link,
               count(*) FILTER (WHERE backfill_relink IN ('unlinked', 'name_no_facts', 'ambiguous'))
                   AS backfill_left_unlinked,
               count(*) FILTER (WHERE link_tier = 'backfill' AND screens_ok AND NOT operating_dated
                                AND key_cik IS NOT NULL
                                AND cik_source NOT IN ('strict', 'name', 'pit_link')) AS excluded_only_is_operating,
               count(*) FILTER (WHERE link_tier = 'backfill' AND eligible AND NOT operating_panel_cik)
                   AS backfill_cik_would_fail_operating
        FROM rank_lines""")
    return {c[0]: int(v) for c, v in zip(cur.description, cur.fetchone(), strict=True)}


class RankSnapshots:
    """Lazy ``{rank_date: eligible lines}`` for :func:`rank_reconstitutions`: one snapshot per rank date; its census
    and the eligible lines' provenance are kept for the receipt, the line dicts are not."""

    def __init__(self, con, dates, pit_links: Path | None = None):
        self.con, self.dates, self.pit_links = con, set(dates), pit_links
        self.census: dict[dt.date, dict[str, int]] = {}
        self.provenance: dict[dt.date, dict[int, dict[str, Any]]] = {}

    def __contains__(self, d) -> bool:
        return d in self.dates

    def __getitem__(self, d):
        if d not in self.dates:
            raise KeyError(d)
        lines = snapshot(self.con, d, self.pit_links)
        self.census[d] = rank_census(self.con)
        self.provenance[d] = {r["security_id"]: {k: r[k] for k in (*PROVENANCE, "cik")} for r in lines}
        return lines

    def eligibility(self, recons: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
        """Measured per rank date, on the selected set (every member of a reconstitution ranked that day) and on
        the rank-date population: the A.5 keys and their diagnostics."""
        selected: dict[dt.date, set[int]] = {}
        for r in recons:
            selected.setdefault(r["rank_date"], set()).update(r["members"])
        out = {}
        for d, sids in sorted(selected.items()):
            prov = [self.provenance[d][s] for s in sids]
            census = self.census[d]
            out[str(d)] = {
                # a selected line's type taken from anything but the dated FINRA basis (directory / vendor)
                "directory_decided": sum(p["security_type_basis"] != "finra_name" for p in prov),
                "backfill_cik": {
                    "selected_company_keys": sum(p["link_tier"] == "backfill" and p["cik"] is not None
                                                 and p["cik_source"] != "pit_link" for p in prov),
                    "excluded_only_is_operating": census["excluded_only_is_operating"]},
                "selected_lines": len(prov),
                "selected_directory_etf_ignored": sum(bool(p["directory_etf"]) for p in prov),
                "selected_backfill_lines": sum(p["link_tier"] == "backfill" for p in prov),
                "selected_backfill_relinked": sum(p["cik_source"] == "pit_link" for p in prov),
                "selected_backfill_shrout_from_sec": sum(p["link_tier"] == "backfill"
                                                         and str(p["shrout_source"] or "").startswith("sec_")
                                                         for p in prov),
                **{k: v for k, v in census.items() if k != "excluded_only_is_operating"},
            }
        return out


def companies_of(lines: list[dict[str, Any]]) -> dict[Any, dict[str, Any]]:
    out: dict[Any, dict[str, Any]] = {}
    for r in lines:
        key = r["cik"] if r["cik"] is not None else -r["security_id"]
        c = out.setdefault(key, {"me": 0.0, "adv": 0.0, "lines": [], "ni_ttm": r["ni_ttm"], "ni_q": r["ni_q"]})
        c["me"] += r["me"]
        c["adv"] += r["adv_63"] or 0.0
        c["lines"].append(r)
    return out



def rank_reconstitutions(snapshots, rank_schedule):
    """Original company/rank/band/buffer kernels on admitted dated snapshots.

    This pure phase does not authenticate caller data. The fixed source reader
    constructs complete snapshots, retains exclusion census and qualifies cells.
    A selected future reconstitution is kept once its rank snapshot is known.
    """
    recons=[]
    previous1,previous2,previous_sp=set(),set(),set()
    for family,rank_date,recon_date in sorted(rank_schedule,key=lambda row:(row[0],row[2])):
        if rank_date not in snapshots:
            raise ValueError('complete native rank snapshot missing')
        lines=snapshots[rank_date]
        if len({row['security_id'] for row in lines})!=len(lines):
            raise ValueError('ambiguous complete rank line identity')
        companies=companies_of(lines)
        for key,company in companies.items():
            # Same-company native earnings must agree; choose no arbitrary
            # first share class when a dated source has conflicting values.
            for field in ('ni_q','ni_ttm'):
                values={row.get(field) for row in company['lines']}
                if len(values)>1:
                    company[field]=None
        if family=='russell':
            assigned,breakpoint=russell_assign([(key,company['me']) for key,company in companies.items()],
                                               previous1,previous2)
            members={'R1000P':{},'R2000P':{},'R3000P':{}}
            for key,(index,rank,_) in assigned.items():
                basis='rank' if (index=='R1000')==(rank<=1000) else 'band_stay'
                for line in companies[key]['lines']:
                    cell=(line['cik'],line['me'],rank,basis)
                    members[index+'P'][line['security_id']]=cell
                    members['R3000P'][line['security_id']]=(line['cik'],line['me'],rank,'rank')
            for index,selected in members.items():
                recons.append({'index_id':index,'rank_date':rank_date,'recon_date':recon_date,
                    'members':selected,'p1000':breakpoint,
                    'eligible_companies':sum(company['me']>=RUSSELL_MIN_ME for company in companies.values())})
            previous1={key for key,value in assigned.items() if value[0]=='R1000'}
            previous2={key for key,value in assigned.items() if value[0]=='R2000'}
        elif family=='sp500':
            candidates=[(key,company['me'],bool((company['ni_ttm'] or 0)>0 and (company['ni_q'] or 0)>0
                and all(line.get('adv_63') is not None for line in company['lines'])
                and company['adv']*252>=SP_LIQ_RATIO*company['me'])) for key,company in companies.items()]
            selected=sp_select(candidates,previous_sp)
            members={line['security_id']:(line['cik'],line['me'],rank,basis)
                     for key,(rank,basis) in selected.items() for line in companies[key]['lines']}
            recons.append({'index_id':'SP500P','rank_date':rank_date,'recon_date':recon_date,
                          'members':members,'eligible_companies':len(companies)})
            previous_sp=set(selected)
        else:
            raise ValueError('fixed original index family required')
    return recons


def intervals(recons, cal, last_session, *, schedule, data_end, prior_exits=(), planned_calendars=None):
    """R1-R4 products of ``recons``: rank vintages with scheduled bounds, exit vintages, summaries (kernel
    :func:`private_index_pit.derive`). ``schedule`` is independent of the vendor data's extent and holds the next
    scheduled reconstitution even for the last one built; ``last_session`` maps a line to ``(last_session,
    delisting_date)`` (or its last session alone)."""
    return P.derive(recons, cal, last_session, schedule, data_end=data_end,
                    prior_exits=prior_exits, planned_calendars=planned_calendars)


# ---------------------------------------------------------------- build
def _schemas():
    import pyarrow as pa

    ts = pa.timestamp("us", tz="UTC")
    return {
        "constituents": pa.schema([("index_id", pa.string()), ("security_id", pa.int64()), ("cik", pa.int64()),
                                   ("effective_from", pa.date32()), ("effective_to", pa.date32()),
                                   ("weight", pa.float64()), ("company_rank", pa.int64()),
                                   ("rank_date", pa.date32()), ("recon_date", pa.date32()), ("basis", pa.string()),
                                   ("available_at", ts)]),
        "exits": pa.schema([("index_id", pa.string()), ("security_id", pa.int64()), ("recon_date", pa.date32()),
                            ("last_session", pa.date32()), ("available_at", ts), ("exit_basis", pa.string())]),
        "membership": pa.schema([("session_date", pa.date32()), ("security_id", pa.int64()), ("cik", pa.int64()),
                                 ("r1000", pa.bool_()), ("r2000", pa.bool_()), ("r3000", pa.bool_()),
                                 ("sp500", pa.bool_()), ("available_at", ts)]),
        "recon_summary": pa.schema([("index_id", pa.string()), ("rank_date", pa.date32()),
                                    ("recon_date", pa.date32()), ("effective_from", pa.date32()),
                                    ("lines", pa.int64()), ("companies", pa.int64()), ("additions", pa.int64()),
                                    ("deletions", pa.int64()), ("turnover_oneway", pa.float64()),
                                    ("turnover_cap", pa.float64()), ("p1000_cum_share", pa.float64()),
                                    ("eligible_companies", pa.int64()), ("available_at", ts)]),
    }


ORDER = {"constituents": "index_id, recon_date, security_id", "exits": "index_id, recon_date, security_id",
         "membership": "session_date, security_id", "recon_summary": "index_id, recon_date"}


def _write_parts(con, table: str, product: str, out: Path) -> dict[str, int]:
    """``<product>/available_year=YYYY/<product>.parquet`` from ``table`` (``available_at`` written as naive UTC)."""
    years = [r[0] for r in con.execute(f"SELECT DISTINCT year(available_at) FROM {table} ORDER BY 1").fetchall()]
    return {str(y): C.copy_to_parquet(con, f"""
                SELECT * REPLACE (CAST(available_at AS TIMESTAMP) AS available_at) FROM {table}
                WHERE year(available_at) = {y} ORDER BY {ORDER[product]}""",
                out / product / f"available_year={y}" / f"{product}.parquet") for y in years}


def line_states(con) -> dict[int, tuple[dt.date, dt.date | None]]:
    lines = (C.build_root() / "security_master" / "lines.parquet").as_posix()
    return {r[0]: (r[1], r[2]) for r in con.execute(
        f"SELECT security_id, last_session, delisting_date FROM read_parquet('{lines}')").fetchall()}


def panel_sessions(con) -> list[dt.date]:
    panel = (C.build_root() / "panel" / "year=*" / "*.parquet").as_posix()
    return [r[0] for r in con.execute(
        f"SELECT DISTINCT session_date FROM read_parquet('{panel}', union_by_name = true, hive_partitioning = false) "
        f"ORDER BY 1").fetchall()]


def build(pit_links: Path | None = None) -> dict[str, Any]:
    import pyarrow as pa

    out = C.stage_dir(STAGE)
    if (out / "manifest.json").exists():
        raise RuntimeError(f"{out / 'manifest.json'} exists: a published stage is immutable (GC4)")
    existing = sorted(out.rglob("*.parquet"))
    if existing:
        raise RuntimeError(f"existing output parts are immutable (GC4): {existing[0]}")
    pit = None
    if pit_links is not None:
        pit_links = Path(pit_links)
        pit = {"path": str(pit_links), "bytes": pit_links.stat().st_size, "sha256": C.sha256_file(pit_links)}
    receipt: dict[str, Any] = {}
    con = C.connect(memory="300MB", threads=2, db_file="indexes_v2.duckdb")
    try:
        with C.timed(receipt, "select"):
            vendor = C.load_calendar(con)
            data_end = vendor[-1]
            cal = E.sessions(vendor[0], dt.date(data_end.year + 1, 12, 31))
            calendar_check = E.check_against_vendor(cal, vendor)          # aborts on a mismatch
            days = panel_sessions(con)
            ranks = rank_schedule(cal, days)
            snaps = RankSnapshots(con, [r[1] for r in ranks], pit_links)
            recons = rank_reconstitutions(snaps, ranks)
            products = intervals(recons, cal, line_states(con),
                                 schedule=schedule(cal, min(r[1].year for r in ranks), data_end.year + 1),
                                 data_end=data_end)
            # A.5 keys; raises (before anything is written) unless every rank vintage holds its whole member set
            # and no directory type or backfill CIK decided a rank
            receipt.update(P.receipt(products, recons, snaps.eligibility(recons)))
        schemas = _schemas()
        rows: dict[str, Any] = {}
        with C.timed(receipt, "write"):
            for p in ("constituents", "exits", "recon_summary"):
                con.register(f"src_{p}", pa.Table.from_pylist(products[p], schema=schemas[p]))
                rows[p] = _write_parts(con, f"src_{p}", p, out)
                con.unregister(f"src_{p}")
            con.execute("CREATE OR REPLACE TABLE membership_rows (session_date DATE, security_id BIGINT, cik BIGINT, "
                        "r1000 BOOLEAN, r2000 BOOLEAN, r3000 BOOLEAN, sp500 BOOLEAN, available_at TIMESTAMPTZ)")
            batch: list[dict[str, Any]] = []
            for cell in P.iter_membership(products["constituents"], products["exits"], cal, data_end):
                batch.append(cell)
                if len(batch) >= 200_000:
                    _append(con, pa, schemas["membership"], batch)
                    batch = []
            _append(con, pa, schemas["membership"], batch)
            rows["membership"] = _write_parts(con, "membership_rows", "membership", out)
            through = con.execute("SELECT max(session_date) FROM membership_rows").fetchone()[0]
        summary = products["recon_summary"]
        receipt["rows"] = rows
        receipt["exchange_calendar"] = calendar_check
        receipt["coverage"] = {
            "panel_first": days[0], "panel_last": days[-1], "vendor_data_end": data_end,
            "exchange_calendar_through": cal[-1], "membership_through": through,
            "rank_dates": {"first": min(r[1] for r in ranks) if ranks else None,
                           "last": max(r[1] for r in ranks) if ranks else None},
            "last_reconstitution": {i: str(max(s["recon_date"] for s in summary if s["index_id"] == i))
                                    for i in sorted({s["index_id"] for s in summary})},
            "note": "constituents rows of the last reconstitution of each index run to the next scheduled one; "
                    "exits and membership after the vendor data end are not yet known"}
        receipt["focus_2020_plus"] = {"from": FOCUS_FROM, "through": data_end,
                                      "gate": russell_gate([s for s in summary if s["recon_date"] >= FOCUS_FROM]),
                                      "counts_on_recon": {k: v for k, v in receipt["counts_on_recon"].items()
                                                          if k.split(" ")[1] >= str(FOCUS_FROM)}}
        receipt["recon_summary"] = summary
        gate = russell_gate(summary)
        C.write_stage_manifest(STAGE, SCHEMA, MODULES, {
            "rule": list(RULES), "rule_text": __doc__, "clock_rule": CLOCK_RULE, "staleness_days": STALENESS_DAYS,
            "staleness": "annual / semi-annual / quarterly", "r7": {"r7_1": "a", "r7_2": "d"},
            "input_manifests_sha256": M.input_manifests(*INPUTS), "pit_links": pit, "gate_status": gate,
            "gap": "official constituent lists are licensed / terms-gated: no overlap measured",
            "receipt": json.loads(json.dumps(receipt, default=str))},
            pattern="*/available_year=*/*.parquet")
        return receipt
    finally:
        con.close()
        (C.build_root() / "_tmp" / "indexes_v2.duckdb").unlink(missing_ok=True)


def _append(con, pa, schema, batch: list[dict[str, Any]]) -> None:
    if not batch:
        return
    con.register("membership_batch", pa.Table.from_pylist(batch, schema=schema))
    con.execute("INSERT INTO membership_rows SELECT * FROM membership_batch")
    con.unregister("membership_batch")


# ---------------------------------------------------------------- measure
def measure(first_year: int = 2018, pit_links: Path | None = None) -> dict[str, Any]:
    """Per rank date of the published v1 ``indexes/`` stage (read only): how many v1-selected lines had a type
    decided by the directory snapshot (``security_type_basis`` not ``finra_name``), a ``directory_etf`` flag, or a
    backfill link on the rank-date panel row; and the R7 (a) + (d) eligible companies (ME >= $30M) of that date.
    Predicts the R2000 gate: an R2000 proxy of [1,900, 2,100] companies needs about 2,900+ such companies."""
    root = C.build_root()
    cons = (root / LEGACY_STAGE / "constituents.parquet").as_posix()
    con = C.connect(memory="300MB", threads=2)
    try:
        dates = [r[0] for r in con.execute(
            f"SELECT DISTINCT rank_date FROM read_parquet('{cons}') ORDER BY 1").fetchall() if r[0].year >= first_year]
        per: dict[str, Any] = {}
        for d in dates:
            panel = (root / "panel" / f"year={d.year}" / "*.parquet").as_posix()
            cur = con.execute(f"""
                SELECT c.index_id, count(*) AS v1_selected,
                       count(*) FILTER (WHERE p.security_type_basis IS DISTINCT FROM 'finra_name') AS type_not_finra,
                       count(*) FILTER (WHERE p.security_type_basis = 'directory_snapshot') AS type_directory,
                       count(*) FILTER (WHERE p.directory_etf) AS directory_etf,
                       count(*) FILTER (WHERE p.link_tier = 'backfill') AS backfill_link,
                       count(*) FILTER (WHERE p.security_type_basis IS DISTINCT FROM 'finra_name'
                                        OR p.link_tier = 'backfill') AS either,
                       count(DISTINCT coalesce(c.cik, -c.security_id)) AS v1_companies
                FROM read_parquet('{cons}') c
                LEFT JOIN read_parquet('{panel}', union_by_name = true, hive_partitioning = false) p
                  ON p.security_id = c.security_id AND p.session_date = c.rank_date
                WHERE c.rank_date = DATE '{d}' GROUP BY 1 ORDER BY 1""")
            cols = [c[0] for c in cur.description]
            entry: dict[str, Any] = {r[0]: dict(zip(cols[1:], r[1:], strict=True)) for r in cur.fetchall()}
            comp = companies_of(snapshot(con, d, pit_links))
            entry["v2_eligible"] = {**rank_census(con), "companies": len(comp),
                                    "companies_me_30m": sum(c["me"] >= RUSSELL_MIN_ME for c in comp.values())}
            per[str(d)] = entry
        focus = {d: {"R2000P_v1_companies": e.get("R2000P", {}).get("v1_companies"),
                     "v2_companies_me_30m": e["v2_eligible"]["companies_me_30m"]}
                 for d, e in per.items() if d >= str(FOCUS_FROM) and "R2000P" in e}
        return {"source": cons, "pit_links": str(pit_links) if pit_links else None, "per_rank_date": per,
                "focus_2020_plus_russell": focus}
    finally:
        con.close()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", nargs="?", choices=("build", "measure"), default="build")
    ap.add_argument("--pit-links", type=Path, help="strict / name link days without backfill withholding "
                                                   "(identity/links_pit.parquet, links_combined schema)")
    ap.add_argument("--from-year", type=int, default=2018, help="measure: first rank-date year")
    args = ap.parse_args(argv)
    if args.step == "measure":
        print(json.dumps(measure(args.from_year, args.pit_links), default=str, indent=1), flush=True)
    else:
        print(json.dumps(build(args.pit_links), default=str)[:6000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
