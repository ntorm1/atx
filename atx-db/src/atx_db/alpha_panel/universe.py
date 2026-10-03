"""Stage ``universe`` (tier1-v4 Task 2): per line-session universe flags, CRSP-style codes and index-proxy membership
-> ``universe/year=YYYY/universe.parquet``, one row per ``panel`` row (2018-01-02 onward), keys
``(session_date, security_id)``. A joinable stage: the panel and every other input are read, never edited.

Point in time. ``available_at`` = 22:00 UTC of ``session_date``. A row uses its session's panel row and
``market/shares_daily`` shares (prices through the close of d) and issuer facts as of ``cutoff`` = ``lake_cutoff(d)``
= 22:00 UTC of the previous calendar session (strict ``<``; the first calendar session uses d - 1 day):

* ``shrcd_like``, ``exchcd_like``: ``share_exchange_history_as_of(cutoff) WHERE d BETWEEN valid_from AND valid_to``
  (identity_v3 codes, their single consumption path). Where the codes carry no exchange because the panel has none,
  ``exchcd_like`` is the repaired listing exchange below (``exchcd_basis`` = ``finra_class_repair``).
* listing exchange (``exchange_u``, not published): the panel ``exchange``, else the FINRA market class of the
  panel's own as-of FINRA row (dissemination date before d, at most ``FINRA_STALE_DAYS`` old) mapped with
  ``security_master.EXCHANGE_BY_CLASS`` including SC (Nasdaq Capital Market) and BZX (Cboe BZX); the frozen panel is repaired from dated FINRA evidence
  by the codes, market and index builders without rebuilding panel.
* issuer CIK (``cik``): the panel ``cik``, else ``identity/link_table_v3`` as of the cutoff (interval holding d).
* ``filer_regime``: ``sec_filings/filer_regime`` of the CIK as of the cutoff (interval holding d): ``domestic``
  (10-K filer), ``fpi_20f``, ``canadian_40f``, ``fund`` (N-CSR filer), ``none``; NULL when unknown or unlinked.
* SIC: ``classification/issuer_industry`` (SIC in force per filing) of the CIK, latest with clock < cutoff.
* incorporation: the latest original EDGAR issuer header or 10-K / 10-Q / 8-K cover
  ``dei:EntityIncorporationStateCountryCode`` with clock < cutoff; cover wins equal clocks.
  Original headers use max(actual acceptance, filing/change date end-of-day Eastern).
  Sparse missing observations remain unknown; undated profiles never change history.
* BDC: the CIK filed a 10-K, 10-Q, 8-K, N-54A, N-6F or N-2 (or amendment) under an ``814-`` (Investment Company
  Act BDC) file number with clock < cutoff, status is the latest qualifying election/support or N-54C withdrawal event strictly before the cutoff.
* index proxies: ``indexes_membership_as_of(cutoff) WHERE session_date = d`` (membership is known at its rank date).

Flags (every row): ``is_adr`` = panel ``is_adr`` or ``is_adr_likely``; ``is_reit`` = panel ``is_reit`` or SIC 6798;
``is_spac`` = panel ``is_spac`` or SIC 6770 with zero/missing panel revenue (blank check); ``is_cef_etf`` = panel ``is_etf``, ``security_type``
fund, regime ``fund`` or BDC (CRSP share code 14 / 73 analogues); ``is_foreign_issuer`` = panel ``is_fpi``, regime
``fpi_20f`` / ``canadian_40f``, or incorporation outside the US (CRSP second digit 2 analogue).

Rule ``universe-all-common-v2`` (``univ_all_common``):

1. the ``market_index`` all_common rule, imported (``market_index.common_filter`` on the panel flags, ``is_operating``,
   not ``is_index``, listing exchange in ``market_index.LISTED``), on the repaired listing exchange;
2. raw close >= ``MIN_PRICE`` and ``shares_daily.shrout`` > 0;
3. CRSP share code 10/11 refinements (without them NYSE lines exceed the French count by 30-48%, 2018-05 ..
   2022-12): not a preferred, warrant, unit or right ticker (``security_master.SYMBOL_SUFFIX`` on the vendor ticker),
   ``filer_regime`` = ``domestic``, and not ``is_foreign_issuer``, ``is_spac``, ``is_cef_etf`` or ``is_reit``.

``me`` = ``shares_daily.shrout`` x raw close (USD; NULL when shares are unknown). ``univ_r1000p / r2000p / r3000p /
sp500p`` = the ``indexes`` proxy flags (false when not a member).

Gates (tier1-v4 gap analysis 2 item 1, ruling T2-a): all_common daily count (yearly mean) in ``DAILY_BAND``
each year 2020-2025; NYSE-listed all_common lines (``exchcd_like`` 1) within +-5% of the French ME-breakpoint NYSE
count each month <= 2022 (a value comparison); R2000 proxy companies in [1900, 2100] on each reconstitution's first
effective session; codes on >= 99% of all_common cells; flags >= 95% precise on a 100-line hand check (verdicts in
``docs/datasets/universe_hand_check.json``). Coverage of 30 core fields per year on ``univ_all_common``,
``univ_r3000p`` and panel ``member_equity`` -> ``validation/universe.json``.

    python -m atx_db.alpha_panel.universe build [--years 2018-2026]
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import hashlib
import random
import statistics
import sys
from pathlib import Path
from typing import Any

from . import common as C
from . import market_common as M
from .identity_v3 import EXCHCD
from .market_index import LISTED, common_filter
from .security_master import EXCHANGE_BY_CLASS, SYMBOL_SUFFIX

STAGE = "universe"
SCHEMA = "atx.alpha-panel.universe/v1"
RULE = "universe-all-common-v3"
MODULES = ("universe", "issuer_history", "issuer_classes", "market_index", "market_common", "security_master", "identity_v3", "common")
MIN_PRICE = 1.0
FINRA_STALE_DAYS = 45            # = panel.SI_STALE_DAYS (the panel's FINRA freshness rule; asserted in the tests)
EXCHANGE_MAP = EXCHANGE_BY_CLASS
SUFFIX_RE = "|".join(p.pattern for p, _ in SYMBOL_SUFFIX)
US_JURISDICTIONS = tuple(
    "AL AK AZ AR CA CO CT DE DC FL GA HI ID IL IN IA KS KY LA ME MD MA MI MN MS MO MT NE NV NH NJ NM NY NC ND OH OK OR "
    "PA RI SC SD TN TX UT VT VA WA WV WI WY PR GU VI AS MP US X1".split())
SPAC_SIC, REIT_SIC = 6770, 6798
BDC_FORMS = ("10-K", "10-Q", "10-K/A", "10-Q/A", "8-K", "8-K/A", "N-54A", "N-6F", "N-2", "N-2/A")
BDC_WITHDRAW_FORMS = ("N-54C",)
YEARS = range(2018, 2027)
GATE_YEARS = range(2020, 2026)
FRENCH_END = dt.date(2022, 12, 31)
DAILY_BAND = (3200, 4800)          # ruling T2-a
FRENCH_TOL = 0.05
R2000_COMPANIES = (1900, 2100)
CODES_FLOOR = 0.99
HAND_FLOOR = 0.95
GATES: dict[str, Any] = {"daily_band": DAILY_BAND, "french_nyse_tol": FRENCH_TOL, "r2000_companies": R2000_COMPANIES,
                         "codes_on_all_common": CODES_FLOOR, "hand_check_precision": HAND_FLOOR}
FRENCH_BREAKPOINTS = C.PACKAGE_ROOT / "data" / "raw" / "benchmarks" / "parsed" / "bench_french_me_breakpoints.parquet"
HAND_CHECK = C.PACKAGE_ROOT / "docs" / "datasets" / "universe_hand_check.json"
INPUTS = ("panel", "market/market_shares_manifest.json", "indexes", "security_master",
          "security_master/share_exchange_history_manifest.json", "identity/link_table_v3_manifest.json",
          "sec_filings", "classification", "identity_cover", "issuer_history", "issuer_classes")
COVERAGE_INPUTS = ("market/market_liquidity_manifest.json", "borrow_proxy")
PANEL_COLUMNS = ("session_date", "security_id", "cik", "ticker", "exchange", "raw_close", "security_type", "is_adr",
                 "is_adr_likely", "is_fpi", "is_reit", "is_lp", "is_royalty_trust", "is_operating", "is_index",
                 "is_etf", "is_spac", "sale_ttm")
MARK = M.MARK

# no LAKE_STAGES literal: three inputs (share_exchange_history, link_table_v3, identity_cover) are not registered
# stages yet; docs/datasets/universe.registry.json is the stub Task 17 folds into stagelake (GC5)


# ---------------------------------------------------------------- SQL fragments (pure)
def _in(values: tuple[str, ...] | list[str]) -> str:
    return ", ".join(f"'{v}'" for v in values)


def exchange_case(col: str) -> str:
    """FINRA market class -> MIC (``EXCHANGE_MAP``; NULL for an unknown class)."""
    return "CASE " + col + " " + " ".join(f"WHEN '{k}' THEN '{v}'" for k, v in EXCHANGE_MAP.items()) + " END"


def exchcd_case(col: str) -> str:
    """MIC -> CRSP-style exchange code (``identity_v3.EXCHCD``)."""
    return "CASE " + col + " " + " ".join(f"WHEN '{k}' THEN {v}" for k, v in EXCHCD.items()) + " END"


def flags_sql(a: str = "t") -> dict[str, str]:
    """Published flag expressions over a step-3 row ``a`` (panel flags + issuer facts)."""
    us = _in(US_JURISDICTIONS)
    return {
        "is_adr": f"coalesce({a}.is_adr, false) OR coalesce({a}.is_adr_likely, false)",
        "is_reit": (f"coalesce({a}.reit_observed, "
                    f"(coalesce({a}.is_reit, false) OR coalesce({a}.sic_asof = {REIT_SIC}, false)))"),
        "is_spac": (f"coalesce({a}.is_spac, false) OR "
                    f"(coalesce({a}.sic_asof = {SPAC_SIC}, false) AND coalesce({a}.sale_ttm, 0) = 0)"),
        "is_cef_etf": (f"coalesce({a}.is_etf, false) OR coalesce({a}.security_type = 'fund', false) "
                       f"OR coalesce({a}.filer_regime = 'fund', false) OR {a}.is_bdc"),
        "is_foreign_issuer": (f"coalesce({a}.is_fpi, false) "
                              f"OR coalesce({a}.filer_regime IN ('fpi_20f', 'canadian_40f'), false) "
                              f"OR coalesce({a}.inc NOT IN ({us}), false)"),
    }


def base_rule_sql(a: str = "t", exchange: str = "exchange_u") -> str:
    """The ``market_index`` all_common rule (imported filter and exchange list) on listing exchange ``exchange``."""
    filt = common_filter(a).replace(f"NOT coalesce({a}.is_reit, false)",
                                   f"(NOT coalesce({a}.is_reit, false) OR coalesce({a}.reit_observed = false, false))")
    return (f"({filt}) AND coalesce({a}.is_operating, false) AND NOT coalesce({a}.is_index, false) "
            f"AND coalesce({a}.{exchange} IN ({_in(LISTED)}), false)")


def all_common_sql(a: str = "t", price: bool = True) -> str:
    """Rule ``universe-all-common-v3`` over a step-3 row ``a`` (``price=False`` drops the $1 screen)."""
    f = flags_sql(a)
    px = f"coalesce({a}.raw_close >= {MIN_PRICE}, false) AND " if price else ""
    return (f"{base_rule_sql(a)} AND {px}coalesce({a}.shrout > 0, false) "
            f"AND NOT coalesce(regexp_matches({a}.ticker, '{SUFFIX_RE}'), false) "
            f"AND coalesce({a}.filer_regime = 'domestic', false) "
            f"AND NOT ({f['is_foreign_issuer']}) AND NOT ({f['is_spac']}) AND NOT ({f['is_cef_etf']}) "
            f"AND NOT ({f['is_reit']})")


# ---------------------------------------------------------------- inputs
def _p(*parts: str) -> str:
    return (C.build_root().joinpath(*parts)).as_posix()


def load_issuer_tables(con) -> dict[str, int]:
    """Issuer- and line-level as-of inputs into the scratch database (small; read once)."""
    stmts = {
        "finra": f"""SELECT security_id, dissemination_date, market_class
                     FROM read_parquet('{_p('security_master', 'finra_names.parquet')}') WHERE issue_name IS NOT NULL""",
        "codes": f"""SELECT security_id, valid_from, valid_to, shrcd, exchcd, available_at
                     FROM read_parquet('{_p('security_master', 'share_exchange_history.parquet')}')""",
        "links": f"""SELECT security_id, cik, valid_from, coalesce(valid_to, DATE '9999-12-31') AS valid_to, available_at
                     FROM read_parquet('{_p('identity', 'link_table_v3.parquet')}') WHERE cik IS NOT NULL""",
        "regime": f"""SELECT cik, regime, valid_from, coalesce(valid_to, DATE '9999-12-31') AS valid_to, available_at
                      FROM read_parquet('{_p('sec_filings', 'filer_regime.parquet')}')""",
        "sic_hist": f"""SELECT cik, available_at, CAST(sic AS INTEGER) AS sic
                        FROM read_parquet('{_p('classification', 'issuer_industry.parquet')}')
                        WHERE sic IS NOT NULL AND available_at IS NOT NULL""",
        "inc_hist": f"""SELECT cik, CAST(available_at AT TIME ZONE 'UTC' AS TIMESTAMP) AS available_at,
                               arg_max(inc_state_country, adsh) AS inc
                        FROM read_parquet('{_p('identity_cover', 'cover_page.parquet')}')
                        WHERE inc_state_country IS NOT NULL AND available_at IS NOT NULL AND cik IS NOT NULL
                        GROUP BY 1, 2""",
        "bdc": f"""SELECT cik, available_at,
                          bool_and(form NOT IN ({_in(BDC_WITHDRAW_FORMS)})) AS elected
                   FROM read_parquet('{_p('sec_filings', 'filings.parquet')}')
                   WHERE file_number LIKE '814-%' AND available_at IS NOT NULL
                     AND form IN ({_in(BDC_FORMS + BDC_WITHDRAW_FORMS)})
                   GROUP BY 1, 2""",
    }
    header_paths = [Path(_p(stage, 'incorporation.parquet')) for stage in ('issuer_history','issuer_classes')]
    header_paths = [p for p in header_paths if p.exists()]
    if header_paths:
        header_union = ' UNION ALL '.join(f"""SELECT cik, CAST(available_at AT TIME ZONE 'UTC' AS TIMESTAMP),
                   inc_state_country, accession, 0, CAST(accepted_at AT TIME ZONE 'UTC' AS TIMESTAMP)
            FROM read_parquet('{p.as_posix()}')
            WHERE inc_state_country IS NOT NULL AND available_at IS NOT NULL AND cik IS NOT NULL""" for p in header_paths)
        stmts['inc_hist'] = f"""WITH observations AS (
            SELECT cik, CAST(available_at AT TIME ZONE 'UTC' AS TIMESTAMP) AS available_at,
                   inc_state_country AS inc, adsh AS accession, 1 AS priority,
                   CAST(available_at AT TIME ZONE 'UTC' AS TIMESTAMP) AS accepted_sort
            FROM read_parquet('{_p('identity_cover', 'cover_page.parquet')}')
            WHERE inc_state_country IS NOT NULL AND available_at IS NOT NULL AND cik IS NOT NULL
            UNION ALL
            {header_union})
            SELECT cik, available_at, inc FROM observations
            QUALIFY row_number() OVER(PARTITION BY cik,available_at ORDER BY priority DESC,accepted_sort DESC,accession DESC)=1"""
    reit_path = Path(_p('issuer_classes','reit_status.parquet'))
    stmts['reit_hist'] = (f"""SELECT cik, CAST(available_at AT TIME ZONE 'UTC' AS TIMESTAMP) AS available_at,
                              reit_status AS reit_observed
                              FROM read_parquet('{reit_path.as_posix()}') WHERE reit_status IS NOT NULL
                              QUALIFY row_number() OVER(PARTITION BY cik,available_at ORDER BY accepted_at DESC,accession DESC)=1"""
                          if reit_path.exists() else
                          "SELECT CAST(NULL AS BIGINT) cik, CAST(NULL AS TIMESTAMP) available_at, CAST(NULL AS BOOLEAN) reit_observed WHERE false")
    out = {}
    for name, sql in stmts.items():
        con.execute(f"CREATE OR REPLACE TABLE {name} AS {sql}")
        out[name] = con.execute(f"SELECT count(*) FROM {name}").fetchone()[0]
    return out


def build_year(con, y: int) -> None:
    """Steps 1-3 for year ``y`` into table ``t`` (one row per panel row)."""
    panel = _p("panel", f"year={y}", "*.parquet")
    shares = _p("market", "shares_daily", f"year={y}", "shares_daily.parquet")
    member = _p("indexes", "membership", f"year={y}", "membership.parquet")
    cols = ", ".join(f"p.{c}" for c in PANEL_COLUMNS)
    have_shares = Path(shares).exists()
    have_member = Path(member).exists()
    con.execute(f"""CREATE OR REPLACE TABLE t1 AS
        WITH cal AS (SELECT session_date, coalesce(prev_session, session_date - 1) AS prev FROM {M.calendar_sql()}),
        p AS (SELECT {cols}, CAST(cal.prev AS TIMESTAMP) + {MARK} AS cutoff
              FROM read_parquet('{panel}', union_by_name = true, hive_partitioning = false) p JOIN cal USING (session_date))
        SELECT p.*, {'s.shrout' if have_shares else 'CAST(NULL AS DOUBLE) AS shrout'},
               coalesce(p.exchange, CASE WHEN p.session_date - f.dissemination_date <= {FINRA_STALE_DAYS}
                                         THEN {exchange_case('f.market_class')} END) AS exchange_u
        FROM p
        {f"LEFT JOIN read_parquet('{shares}', hive_partitioning = false) s USING (session_date, security_id)" if have_shares else ''}
        ASOF LEFT JOIN finra f ON f.security_id = p.security_id AND p.session_date > f.dissemination_date""")
    # link_table_v3 only where the panel has no CIK; overlapping link rows resolve to the latest valid_from
    con.execute("""CREATE OR REPLACE TABLE lk AS
        SELECT t1.session_date, t1.security_id, arg_max(l.cik, l.valid_from) AS cik
        FROM t1 JOIN links l ON l.security_id = t1.security_id AND t1.session_date BETWEEN l.valid_from AND l.valid_to
                            AND l.available_at < t1.cutoff
        WHERE t1.cik IS NULL GROUP BY 1, 2""")
    con.execute("""CREATE OR REPLACE TABLE t2 AS
        SELECT t1.*, coalesce(t1.cik, lk.cik) AS cik_u FROM t1 LEFT JOIN lk USING (session_date, security_id)""")
    mem = (f"""LEFT JOIN read_parquet('{member}', hive_partitioning = false) m
               ON m.session_date = t2.session_date AND m.security_id = t2.security_id AND m.available_at < t2.cutoff"""
           if have_member else "")
    mcols = ("coalesce(m.r1000, false) AS r1000, coalesce(m.r2000, false) AS r2000, coalesce(m.r3000, false) AS r3000, "
             "coalesce(m.sp500, false) AS sp500") if have_member else \
        "false AS r1000, false AS r2000, false AS r3000, false AS sp500"
    con.execute(f"""CREATE OR REPLACE TABLE t3 AS
        SELECT t2.*, c.shrcd AS shrcd_like, c.exchcd AS exchcd_v3, r.regime AS filer_regime, si.sic AS sic_asof,
               ic.inc AS inc, rh.reit_observed,
               coalesce(b.elected, false) AS is_bdc,
               {mcols}
        FROM t2
        LEFT JOIN codes c ON c.security_id = t2.security_id AND t2.session_date BETWEEN c.valid_from AND c.valid_to
                         AND c.available_at < t2.cutoff
        LEFT JOIN regime r ON r.cik = t2.cik_u AND t2.session_date BETWEEN r.valid_from AND r.valid_to
                          AND r.available_at < t2.cutoff
        {mem}
        ASOF LEFT JOIN bdc b ON b.cik = t2.cik_u AND t2.cutoff > b.available_at
        ASOF LEFT JOIN sic_hist si ON si.cik = t2.cik_u AND t2.cutoff > si.available_at
        ASOF LEFT JOIN inc_hist ic ON ic.cik = t2.cik_u AND t2.cutoff > ic.available_at
        ASOF LEFT JOIN reit_hist rh ON rh.cik = t2.cik_u AND t2.cutoff > rh.available_at""")
    # a panel row must stay one row (the interval inputs never overlap; checked, not assumed)
    n1, n3, keys = con.execute("""SELECT (SELECT count(*) FROM t1), count(*), count(DISTINCT (session_date, security_id))
                                  FROM t3""").fetchone()
    if not n1 == n3 == keys:
        raise RuntimeError(f"universe {y}: row fan-out t1 {n1} -> t3 {n3} ({keys} keys)")
    f = flags_sql("t3")   # the published is_adr / is_reit / is_spac replace the panel's columns of the same name
    con.execute(f"""CREATE OR REPLACE TABLE t AS
        SELECT t3.* EXCLUDE (is_adr, is_reit, is_spac), {', '.join(f'({v}) AS {k}' for k, v in f.items())},
               exchange IS NULL AND exchange_u IS NOT NULL AS exchange_repaired,
               ({all_common_sql('t3')}) AS univ_all_common,
               ({all_common_sql('t3', price=False)}) AS all_common_nop,
               ({base_rule_sql('t3', 'exchange')}) AS mi_panel_exchange,
               ({base_rule_sql('t3')}) AS mi_repaired
        FROM t3""")


def output_sql() -> str:
    return f"""
        SELECT session_date, security_id, cik_u AS cik, CAST(shrcd_like AS INTEGER) AS shrcd_like,
               CAST(coalesce(exchcd_v3, {exchcd_case('exchange_u')}) AS INTEGER) AS exchcd_like,
               CASE WHEN exchcd_v3 IS NOT NULL THEN 'identity_v3'
                    WHEN {exchcd_case('exchange_u')} IS NOT NULL THEN 'finra_class_repair' END AS exchcd_basis,
               is_adr, is_reit, is_spac, is_cef_etf, is_foreign_issuer, filer_regime,
               CASE WHEN shrout > 0 AND raw_close > 0 THEN shrout * raw_close END AS me,
               univ_all_common, r3000 AS univ_r3000p, r1000 AS univ_r1000p, r2000 AS univ_r2000p, sp500 AS univ_sp500p,
               CAST(session_date AS TIMESTAMP) + {MARK} AS available_at
        FROM t ORDER BY session_date, security_id"""


def year_receipt(con) -> list[list[Any]]:
    """Per session: all_common, without the $1 screen, the market_index rule on the panel exchange (plus $1 and
    shares) and on the repaired exchange, NYSE all_common lines, repaired cells, codes present on all_common."""
    return [list(r) for r in con.execute(f"""
        SELECT session_date, count(*) FILTER (WHERE univ_all_common), count(*) FILTER (WHERE all_common_nop),
               count(*) FILTER (WHERE mi_panel_exchange AND raw_close >= {MIN_PRICE} AND shrout > 0),
               count(*) FILTER (WHERE mi_repaired AND raw_close >= {MIN_PRICE} AND shrout > 0),
               count(*) FILTER (WHERE mi_panel_exchange),
               count(*) FILTER (WHERE univ_all_common AND coalesce(exchcd_v3, {exchcd_case('exchange_u')}) = 1),
               count(*) FILTER (WHERE univ_all_common AND exchange_repaired),
               count(*) FILTER (WHERE univ_all_common AND shrcd_like IS NOT NULL
                                AND coalesce(exchcd_v3, {exchcd_case('exchange_u')}) IS NOT NULL),
               count(*) FILTER (WHERE univ_all_common AND exchcd_v3 IS NULL),
               count(*) FILTER (WHERE shrcd_like IS NULL)
        FROM t GROUP BY 1 ORDER BY 1""").fetchall()]


DAILY_COLS = ("all_common", "all_common_no_price_screen", "mi_rule_panel_exchange", "mi_rule_repaired_exchange",
              "mi_rule_panel_exchange_no_price_shares", "nyse_all_common", "all_common_exchange_repaired",
              "all_common_with_codes", "all_common_exchcd_from_repair", "rows_without_shrcd")


# ---------------------------------------------------------------- gates (pure)
def band_gate(daily: dict[str, dict[str, int]], band: tuple[int, int] = DAILY_BAND,
              years: range = GATE_YEARS, col: str = "all_common") -> dict[str, Any]:
    """Yearly mean / min / max of the per-session count ``col``; pass when every gate year's mean is in ``band``."""
    per: dict[str, Any] = {}
    for y in years:
        vals = [v[col] for d, v in daily.items() if d.startswith(str(y))]
        if vals:
            per[str(y)] = {"sessions": len(vals), "mean": round(statistics.mean(vals), 1), "min": min(vals),
                           "max": max(vals)}
    ok = len(per) == len(years) and all(band[0] <= v["mean"] <= band[1] for v in per.values())
    return {"criterion": f"{col} names/day (yearly mean) in [{band[0]}, {band[1]}] each year {years[0]}-{years[-1]}",
            "pass": ok, "per_year": per}


def french_gate(ours: dict[dt.date, tuple[int, int]], french: dict[dt.date, int], tol: float = FRENCH_TOL,
                end: dt.date = FRENCH_END, expected: list[dt.date] | None = None) -> dict[str, Any]:
    """``ours`` = {month end date: (NYSE all_common lines, companies)} on the month's last session; ``french`` =
    {month end: n_nyse_firms}. Pass when every common month <= ``end`` has lines within +-``tol``."""
    rows: list[dict[str, Any]] = []
    outside: dict[str, float] = {}
    missing = [m.isoformat()[:7] for m in (expected or []) if m <= end and (m not in ours or m not in french)]
    for m in sorted(k for k in ours if k in french and k <= end):
        lines, comps = ours[m]
        f = french[m]
        dev = lines / f - 1.0
        rows.append({"month": m.isoformat()[:7], "french": f, "lines": lines, "companies": comps,
                     "dev_lines": round(dev, 4), "dev_companies": round(comps / f - 1.0, 4)})
        if abs(dev) > tol + 1e-9:               # 1050 / 1000 - 1 is 0.05000000000000004
            outside[m.isoformat()[:7]] = round(dev, 4)
    return {"criterion": f"NYSE all_common lines within +-{tol:.0%} of French ME-breakpoint NYSE count, each month "
                         f"<= {end.isoformat()[:7]}",
            "pass": bool(rows) and not outside and not missing, "missing_months": missing,
            "months": len(rows), "months_outside": len(outside),
            "outside": outside, "max_abs_dev": max((abs(r["dev_lines"]) for r in rows), default=None), "monthly": rows}


def r2000_gate(counts: dict[str, int], bounds: tuple[int, int] = R2000_COMPANIES) -> dict[str, Any]:
    outside = {k: v for k, v in counts.items() if not bounds[0] <= v <= bounds[1]}
    return {"criterion": f"univ_r2000p companies in [{bounds[0]}, {bounds[1]}] on each reconstitution's first "
                         f"effective session", "pass": bool(counts) and not outside, "companies": counts,
            "outside": outside}


def hand_check_gate(path: Path | None = None, floor: float = HAND_FLOOR, sample: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    """Precision of the flags asserted true on the hand-checked lines (``rows[].verdict`` 'correct' / 'wrong');
    ``path`` defaults to ``HAND_CHECK`` (read at call time)."""
    path = path or HAND_CHECK
    if not path.exists():
        return {"criterion": f"flags >= {floor:.0%} precise on a 100-line hand check", "pass": False,
                "status": "missing", "file": str(path)}
    doc = json.loads(path.read_text(encoding="utf-8"))
    rows = doc["rows"]
    if sample is not None:
        keys = lambda rs: sorted((r["stratum"], str(r["session_date"]), r["security_id"]) for r in rs)
        valid = (doc.get("rule") == RULE and doc.get("seed") == HAND_SEED
                 and doc.get("sample_sha256") == sample_hash(sample)
                 and keys(rows) == keys(sample) and len(set(keys(rows))) == len(rows))
        if not valid:
            return {"criterion": f"flags >= {floor:.0%} precise on a 100-line hand check", "pass": False,
                    "status": "stale_verdicts_require_recheck", "sample_sha256": sample_hash(sample)}
    n = len(rows)
    ok = sum(1 for r in rows if r["verdict"] == "correct")
    by: dict[str, list[int]] = {}
    for r in rows:
        v = by.setdefault(r["stratum"], [0, 0])
        v[0] += r["verdict"] == "correct"
        v[1] += 1
    return {"criterion": f"flags >= {floor:.0%} precise on a 100-line hand check", "lines": n, "correct": ok,
            "precision": round(ok / n, 4) if n else None, "pass": n >= 100 and ok / n >= floor,
            "by_stratum": {k: {"correct": v[0], "lines": v[1]} for k, v in sorted(by.items())},
            "file": "atx-db/docs/datasets/universe_hand_check.json", "sample_seed": doc.get("seed")}


# ---------------------------------------------------------------- measurements on the published files
def out_glob() -> str:
    return _p(STAGE, "year=*", "universe.parquet")


def measure_french(con) -> dict[str, Any]:
    if not FRENCH_BREAKPOINTS.exists():
        return {"criterion": "French NYSE count", "pass": False, "status": "benchmark file missing"}
    french = dict(con.execute(f"SELECT month_end, n_nyse_firms FROM read_parquet('{FRENCH_BREAKPOINTS.as_posix()}')"
                              ).fetchall())
    ours = {r[0]: (r[1], r[2]) for r in con.execute(f"""
        WITH u AS (SELECT * FROM read_parquet('{out_glob()}', hive_partitioning = false)
                   WHERE session_date <= DATE '{FRENCH_END}'),
        last AS (SELECT max(session_date) AS d FROM u GROUP BY date_trunc('month', session_date))
        SELECT CAST(last_day(u.session_date) AS DATE), count(*), count(DISTINCT coalesce(u.cik, -u.security_id))
        FROM u JOIN last ON u.session_date = last.d
        WHERE u.univ_all_common AND u.exchcd_like = 1 GROUP BY 1""").fetchall()}
    expected = [dt.date(y, m, 1) for y in range(YEARS[0], FRENCH_END.year + 1) for m in range(1, 13)]
    expected = [(d.replace(day=28) + dt.timedelta(days=4)).replace(day=1) - dt.timedelta(days=1) for d in expected]
    return french_gate(ours, french, expected=expected)


def measure_r2000(con) -> dict[str, Any]:
    recon = _p("indexes", "recon_summary.parquet")
    eff = [r[0] for r in con.execute(f"""SELECT DISTINCT effective_from FROM read_parquet('{recon}')
                                        WHERE index_id = 'R2000P' ORDER BY 1""").fetchall()]
    counts = {}
    for d in eff:
        y = d.year
        f = _p(STAGE, f"year={y}", "universe.parquet")
        if not Path(f).exists():
            counts[d.isoformat()] = 0
            continue
        counts[d.isoformat()] = con.execute(f"""SELECT count(DISTINCT coalesce(cik, -security_id))
                                               FROM read_parquet('{f}') WHERE session_date = DATE '{d}' AND univ_r2000p"""
                                            ).fetchone()[0]
    return r2000_gate(counts)


def measure_counts(con) -> dict[str, Any]:
    """Per-year mean daily line counts of every flag and universe (published files)."""
    rows = con.execute(f"""
        WITH d AS (SELECT session_date, count(*) AS n_rows, count(*) FILTER (WHERE univ_all_common) AS all_common,
                          count(*) FILTER (WHERE univ_r3000p) AS r3000p, count(*) FILTER (WHERE univ_r1000p) AS r1000p,
                          count(*) FILTER (WHERE univ_r2000p) AS r2000p, count(*) FILTER (WHERE univ_sp500p) AS sp500p,
                          count(*) FILTER (WHERE univ_all_common AND univ_r3000p) AS r3000p_in_all_common,
                          count(*) FILTER (WHERE is_adr) AS is_adr, count(*) FILTER (WHERE is_reit) AS is_reit,
                          count(*) FILTER (WHERE is_spac) AS is_spac, count(*) FILTER (WHERE is_cef_etf) AS is_cef_etf,
                          count(*) FILTER (WHERE is_foreign_issuer) AS is_foreign_issuer,
                          count(*) FILTER (WHERE shrcd_like = 11) AS shrcd_11
                   FROM read_parquet('{out_glob()}', hive_partitioning = false) GROUP BY 1)
        SELECT year(session_date), count(*), {', '.join(f'round(avg({c}), 1)' for c in COUNT_COLS)}
        FROM d GROUP BY 1 ORDER BY 1""").fetchall()
    return {str(r[0]): {"sessions": r[1], **dict(zip(COUNT_COLS, r[2:]))} for r in rows}


COUNT_COLS = ("n_rows", "all_common", "r3000p", "r1000p", "r2000p", "sp500p", "r3000p_in_all_common", "is_adr",
              "is_reit", "is_spac", "is_cef_etf", "is_foreign_issuer", "shrcd_11")

# 30 core fields: (name, source, SQL truth over p = panel, u = universe, l = market_liquidity, b = borrow_proxy,
# h = return-history counts)
FIELDS: tuple[tuple[str, str, str], ...] = (
    ("ret", "panel ret, unguarded", "p.ret IS NOT NULL AND NOT coalesce(p.ret_guarded, true)"),
    ("prc", "panel raw_close", "p.raw_close > 0"),
    ("me", "universe me (shares_daily shrout x raw close)", "u.me > 0"),
    ("adv_63", "market_liquidity adv_63", "l.adv_63 IS NOT NULL"),
    ("sale_ttm", "panel sale_ttm (saleq, trailing 4 quarters)", "p.sale_ttm IS NOT NULL"),
    ("saleq", "panel sale_q", "p.sale_q IS NOT NULL"),
    ("atq", "panel at", 'p."at" IS NOT NULL'),
    ("ceqq", "panel seq (ceqq = seq - pstk)", "p.seq IS NOT NULL"),
    ("ibq", "panel ni_q (quarterly net income)", "p.ni_q IS NOT NULL"),
    ("oancf", "panel cfo_ttm", "p.cfo_ttm IS NOT NULL"),
    ("sic", "panel sic", "p.sic IS NOT NULL"),
    ("ff49", "panel grp_ff49", "p.grp_ff49 IS NOT NULL"),
    ("inst_ownership", "panel inst_shares (13F)", "p.inst_shares IS NOT NULL"),
    ("short_interest", "panel si_shares (FINRA)", "p.si_shares IS NOT NULL"),
    ("short_volume", "panel sv_short_volume (FINRA RegSHO daily)", "p.sv_short_volume IS NOT NULL"),
    ("ftd", "panel ftd_quantity (SEC fails-to-deliver)", "p.ftd_quantity IS NOT NULL"),
    ("earnings_date", "panel earn_last_utc (8-K 2.02)", "p.earn_last_utc IS NOT NULL"),
    ("iv_atm", "panel iv_atm_21d", "p.iv_atm_21d IS NOT NULL"),
    ("borrow_proxy", "borrow_proxy si_to_io", "b.si_to_io IS NOT NULL"),
    ("beta_input", ">= 126 unguarded daily returns in the line's last 252 panel sessions", "h.n252 >= 126"),
    ("idio_vol_input", ">= 15 unguarded daily returns in the line's last 21 panel sessions", "h.n21 >= 15"),
    ("cik", "universe cik (panel link, else link_table_v3 as of)", "u.cik IS NOT NULL"),
    ("be", "panel be (book equity)", "p.be IS NOT NULL"),
    ("ni_ttm", "panel ni_ttm", "p.ni_ttm IS NOT NULL"),
    ("gp_ttm", "panel gp_ttm", "p.gp_ttm IS NOT NULL"),
    ("oi_ttm", "panel oi_ttm", "p.oi_ttm IS NOT NULL"),
    ("debt", "panel debt", "p.debt IS NOT NULL"),
    ("che", "panel che", "p.che IS NOT NULL"),
    ("capx_ttm", "panel capx_ttm", "p.capx_ttm IS NOT NULL"),
    ("sue", "panel sue", "p.sue IS NOT NULL"),
)
COVERAGE_UNIVERSES = {"univ_all_common": "u.univ_all_common", "univ_r3000p": "u.univ_r3000p",
                      "member_equity": "coalesce(p.member_equity, false)"}


def coverage(con, years: range = GATE_YEARS) -> dict[str, Any]:
    """Share of cells with a value, per year and universe, for each of ``FIELDS``."""
    out: dict[str, Any] = {}
    for y in years:
        files = [_p("panel", f"year={k}", "*.parquet") for k in (y - 1, y)
                 if list((C.build_root() / "panel" / f"year={k}").glob("*.parquet"))]
        liq = _p("market", "liquidity", f"year={y}", "liquidity.parquet")
        bp = _p("borrow_proxy", f"year={y}", "borrow_proxy.parquet")
        uni = _p(STAGE, f"year={y}", "universe.parquet")
        if not Path(uni).exists():
            continue
        con.execute(f"""CREATE OR REPLACE TABLE h AS
            WITH r AS (SELECT security_id, session_date,
                              CAST(ret IS NOT NULL AND NOT coalesce(ret_guarded, true) AS INTEGER) AS ok
                       FROM read_parquet([{', '.join(f"'{f}'" for f in files)}], union_by_name = true,
                                         hive_partitioning = false))
            SELECT * FROM (SELECT security_id, session_date,
                                  sum(ok) OVER (PARTITION BY security_id ORDER BY session_date
                                                ROWS BETWEEN 251 PRECEDING AND CURRENT ROW) AS n252,
                                  sum(ok) OVER (PARTITION BY security_id ORDER BY session_date
                                                ROWS BETWEEN 20 PRECEDING AND CURRENT ROW) AS n21 FROM r)
            WHERE year(session_date) = {y}""")
        sel = []
        for uname, ucond in COVERAGE_UNIVERSES.items():
            sel.append(f"count(*) FILTER (WHERE {ucond}) AS \"{uname}|cells\"")
            sel += [f"avg(CASE WHEN {e} THEN 1.0 ELSE 0.0 END) FILTER (WHERE {ucond}) AS \"{uname}|{n}\""
                    for n, _, e in FIELDS]
        cur = con.execute(f"""
            SELECT {', '.join(sel)}
            FROM read_parquet('{_p('panel', f'year={y}', '*.parquet')}', union_by_name = true, hive_partitioning = false) p
            JOIN read_parquet('{uni}', hive_partitioning = false) u USING (session_date, security_id)
            LEFT JOIN read_parquet('{liq}', hive_partitioning = false) l USING (session_date, security_id)
            LEFT JOIN read_parquet('{bp}', hive_partitioning = false) b USING (session_date, security_id)
            LEFT JOIN h USING (session_date, security_id)""")
        names = [c[0] for c in cur.description]
        vals = cur.fetchone()
        yo: dict[str, Any] = {}
        for k, v in zip(names, vals):
            uname, field = k.split("|")
            yo.setdefault(uname, {})[field] = round(v, 4) if isinstance(v, float) else v
        out[str(y)] = yo
    out["fields"] = {n: src for n, src, _ in FIELDS}
    return out


def sample_hash(sample: list[dict[str, Any]]) -> str:
    fields = ("stratum", "session_date", "security_id", "shrcd_like", "exchcd_like", "filer_regime",
              "is_adr", "is_reit", "is_spac", "is_cef_etf", "is_foreign_issuer", "univ_all_common")
    payload = {"rule": RULE, "seed": HAND_SEED,
               "rows": [{k: r[k] for k in fields} for r in sample]}
    return hashlib.sha256(json.dumps(payload, sort_keys=True, default=str).encode()).hexdigest()


HAND_SEED = 20261002
HAND_STRATA = {"univ_all_common": ("u.univ_all_common", "2024-06-28", 40),
               "is_adr": ("u.is_adr", "2024-06-28", 12),
               "is_reit": ("u.is_reit", "2024-06-28", 12),
               "is_spac": ("u.is_spac", "2021-06-30", 12),
               "is_cef_etf": ("u.is_cef_etf", "2024-06-28", 12),
               "is_foreign_issuer": ("u.is_foreign_issuer AND NOT u.is_adr", "2024-06-28", 12)}


def hand_check_sample(con, seed: int = HAND_SEED) -> list[dict[str, Any]]:
    """The 100-line hand-check sample (``HAND_STRATA``: 40 all_common lines and 12 lines each flagged ADR, REIT,
    SPAC (on 2021-06-30: few remain in 2024), CEF / ETF and foreign non-ADR issuer), with the names to judge."""
    rng = random.Random(seed)
    rows: list[dict[str, Any]] = []
    for stratum, (cond, d, n) in HAND_STRATA.items():
        y = d[:4]
        cand = con.execute(f"""
            SELECT u.session_date, u.security_id, p.ticker, p.finra_issue_name, p.security_type, p.exchange, u.cik,
                   u.shrcd_like, u.exchcd_like, u.exchcd_basis, u.filer_regime, u.is_adr, u.is_reit, u.is_spac,
                   u.is_cef_etf, u.is_foreign_issuer, u.univ_all_common, round(u.me / 1e6, 1) AS me_musd, pr.name,
                   pr.sic AS profile_sic, pr.state_of_incorporation
            FROM read_parquet('{_p(STAGE, f'year={y}', 'universe.parquet')}') u
            JOIN read_parquet('{_p('panel', f'year={y}', '*.parquet')}', union_by_name = true) p
                 USING (session_date, security_id)
            LEFT JOIN (SELECT cik, any_value(name) AS name, any_value(sic) AS sic,
                              any_value(state_of_incorporation) AS state_of_incorporation
                       FROM read_parquet('{_p('sec_filings', 'issuer_profile.parquet')}') GROUP BY 1) pr ON pr.cik = u.cik
            WHERE u.session_date = DATE '{d}' AND {cond}
            ORDER BY u.security_id""")
        cols = [c[0] for c in cand.description]
        allrows = [dict(zip(cols, r)) for r in cand.fetchall()]
        for r in rng.sample(allrows, min(n, len(allrows))):
            rows.append({"stratum": stratum, **{k: (v.isoformat() if isinstance(v, dt.date) else v) for k, v in r.items()}})
    return rows


# ---------------------------------------------------------------- build
def build(years: range = YEARS) -> dict[str, Any]:
    inputs = M.input_manifests(*INPUTS)
    missing = [k for k, v in inputs.items() if v is None]
    if missing:
        raise RuntimeError(f"universe: input manifests missing: {missing}")
    receipt: dict[str, Any] = {"rule": RULE, "years": [years[0], years[-1]]}
    con = C.connect(memory="380MB", threads=2, db_file="universe.duckdb")
    with C.timed(receipt, "issuer_tables"):
        receipt["issuer_tables"] = load_issuer_tables(con)
    out = C.stage_dir(STAGE)
    # a guard stop resumes at the first year without its receipt (kept while input manifests and code are unchanged)
    work = M.resumable_dir("universe", inputs, MODULES)
    daily: dict[str, dict[str, int]] = {}
    receipt["rows"], receipt["resumed_years"] = {}, []
    for y in years:
        if not list((C.build_root() / "panel" / f"year={y}").glob("*.parquet")):
            continue
        dest, rec = out / f"year={y}" / "universe.parquet", work / f"daily_{y}.json"
        if dest.exists() and rec.exists():
            done = C.read_json(rec)
            daily.update(done["daily"])
            receipt["rows"][str(y)] = done["rows"]
            receipt["resumed_years"].append(y)
            continue
        with C.timed(receipt, f"year_{y}"):
            build_year(con, y)
            yd = {r[0].isoformat(): dict(zip(DAILY_COLS, r[1:])) for r in year_receipt(con)}
            n = C.copy_to_parquet(con, output_sql(), dest, row_group_size=131072)
            C.write_json_atomic(rec, {"daily": yd, "rows": n})
            daily.update(yd)
            receipt["rows"][str(y)] = n
        con.execute("DROP TABLE IF EXISTS t1; DROP TABLE IF EXISTS lk; DROP TABLE IF EXISTS t2; DROP TABLE IF EXISTS t3")
    with C.timed(receipt, "gates"):
        codes = {}
        for y in GATE_YEARS:
            ds = [v for d, v in daily.items() if d.startswith(str(y))]
            n = sum(v["all_common"] for v in ds)
            codes[str(y)] = round(sum(v["all_common_with_codes"] for v in ds) / n, 6) if n else None
        gates = {
            "daily_band": band_gate(daily),
            "daily_no_price_screen": band_gate(daily, col="all_common_no_price_screen"),
            "french_nyse": measure_french(con),
            "r2000": measure_r2000(con),
            "codes_on_all_common": {"criterion": f"shrcd_like and exchcd_like on >= "
                                                 f"{CODES_FLOOR:.0%} of all_common cells, each year "
                                                 f"{GATE_YEARS[0]}-{GATE_YEARS[-1]}",
                                    "per_year": codes, "pass": bool(codes) and all(
                                        v is not None and v >= CODES_FLOOR for v in codes.values())},
            "hand_check": hand_check_gate(),
        }
        receipt["counts"] = measure_counts(con)
    with C.timed(receipt, "coverage"):
        cov = coverage(con)
    with C.timed(receipt, "hand_check_sample"):
        sample = hand_check_sample(con)
    gates["hand_check"] = hand_check_gate(sample=sample)
    con.close()
    (C.build_root() / "_tmp" / "universe.duckdb").unlink(missing_ok=True)
    all_years = range(min(int(d[:4]) for d in daily), max(int(d[:4]) for d in daily) + 1)
    validation = {"rule": RULE, "gates": gates, "counts": receipt["counts"], "coverage": cov,
                  "daily_per_year": {c: band_gate(daily, years=all_years, col=c)["per_year"] for c in DAILY_COLS},
                  "input_manifests_sha256": {**inputs, **M.input_manifests(*COVERAGE_INPUTS)}}
    C.write_json_atomic(C.stage_dir("validation") / "universe.json", validation)
    C.write_json_atomic(C.stage_dir("validation") / "universe_hand_check_sample.json",
                        {"rule": RULE, "seed": HAND_SEED, "strata": HAND_STRATA, "sample_sha256": sample_hash(sample), "rows": sample})
    automated = ("daily_band", "french_nyse", "r2000", "codes_on_all_common")
    gate_status = {"pass": all(gates[k]["pass"] for k in automated) and gates["hand_check"]["pass"],
                   "gates": GATES, "per_gate": {k: g["pass"] for k, g in gates.items()},
                   "measured": {"daily_band_means": {y: v["mean"] for y, v in gates["daily_band"]["per_year"].items()},
                                "french_months_outside": gates["french_nyse"].get("months_outside"),
                                "french_max_abs_dev": gates["french_nyse"].get("max_abs_dev"),
                                "r2000_companies": gates["r2000"].get("companies"),
                                "codes_on_all_common": codes,
                                "hand_check_precision": gates["hand_check"].get("precision")},
                   "measured_by": "validation/universe.json", "ruling": "T2-a (daily band 3,200-4,800)"}
    receipt["gate_status"] = gate_status
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, {
        "rule": RULE, "rule_text": __doc__, "staleness": "daily; rebuilt with the panel",
        "input_manifests_sha256": inputs, "gate_status": gate_status, "receipt": receipt})
    return receipt


def _years(s: str) -> range:
    a, _, b = s.partition("-")
    return range(int(a), int(b or a) + 1)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", nargs="?", choices=("build",), default="build")
    ap.add_argument("--years", type=_years, default=YEARS)
    args = ap.parse_args(argv)
    rec = build(args.years)
    print(json.dumps(rec, default=str)[:8000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
