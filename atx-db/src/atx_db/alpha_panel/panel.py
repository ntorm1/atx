"""Stage X: assemble the point-in-time daily panel with the scorecard's field names.

Sub-stages (resumable, each writes under the build root):

``core``        per security bucket: session index, 63-session prior dollar volume, A8 lagged
                shares (``shares_out``), earnings indicator. -> ``_tmp/panel_core/bucket_NN.parquet``
``membership``  per year: scorecard universe ``research-prior63-usd-adv-topn-v1`` and the
                equal-weight ``mkt_ret``. -> ``_tmp/panel_member/year=YYYY.parquet``
``assemble``    per year: joins identity, fundamentals, SIC industry, FINRA short interest and
                short volume (each optional: a missing stage leaves its columns NULL and is
                recorded in the manifest). -> ``panel/year=YYYY/panel.parquet``

Rules:
* ``member`` (research-prior63-usd-adv-topn-v1): at session d rank lines by mean dollar volume
  (raw close x volume) over the 63 sessions d-63..d-1; the line must have a bar on every one of
  them; raw close at d-1 > $5; mean > $5M; top 3,000, ties by security_id ascending.
* ``mkt_ret``: equal-weight mean of ``ret`` over lines that were members at d-1, present at d-1
  and d, and not ``ret_guarded``; broadcast to every present line.
* ``shares_out`` (A8-vendor-shares-lag90-restated): the line's last vendor share count dated
  <= d - 90 calendar days (and >= d - 490), restated by the product of the vendor return factors
  after it (factor-break-v1 repaired steps excluded), withheld from the line's first above-ceiling
  vendor row onward, NULL outside [1e5, 5e10] and when the trailing 21-session median volume
  exceeds 3 x shares_out or visible short interest exceeds 5 x shares_out.
* Issuer fields are attached to every line linked to the CIK (``is_issuer_primary`` marks the issuer's
  most liquid line that day by prior 63-session dollar volume, ties by security_id; consumers that
  want one line per issuer mask the others); an event is visible at d when
  ``clock_utc`` < 22:00 UTC of session d-1; items are stale (NULL) when d - period_end > 400
  days (``sue``: 200 days). SIC industry: latest SIC event with the same clock rule, 550-day
  staleness. ``me_company`` sums ``shares_out x raw_close`` over every line linked to the CIK.
* Vendor earnFlag is unreliable (owner ruling 2026-09-28): it drives nothing. ``earn_recent_vendor``,
  ``earn_flag_vendor`` and ``is_operating_vendor`` are carried for audit only. ``earn_recent`` is 1 on the reaction
  session of an SEC 8-K Item 2.02 announcement of the linked issuer and on the session after it (NULL when unlinked);
  ``is_operating`` = an equity-type line (security master) that is not an ETP, SPAC, preferred, unit, warrant, right,
  note or index line, whose linked issuer filed a periodic report within 400 days (unlinked lines of equity type are
  kept); ``member_equity`` = member and is_operating.
* Links come from ``identity/link_table.parquet`` and are used once known (strict / name: ``available_at`` <= the
  session's 22:00 UTC mark; backfill: ``evidence_at``).
* As-of producer outputs (``ASOF_SOURCES``): value visible at d when its clock < 22:00 UTC of session d-1.
* ``si_shares``/``si_dtc``: latest settlement with dissemination_date < d, age <= 45 days.
* Short volume for trade date T is visible from the next session: ``sv_*`` at d are T = d-1.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

from . import common as C
from .prices import BUCKETS

TOP_N = 3000
ADV_MIN = 5_000_000.0
PRICE_MIN = 5.0
LOOKBACK = 63
SHARES_LAG_DAYS = 90
SHARES_MAX_AGE_DAYS = 400
SHARES_DOMAIN = (1e5, 5e10)
TURNOVER_WINDOW = 21
TURNOVER_MIN_OBS = 11
TURNOVER_MAX = 3.0
SI_RATIO_MAX = 5.0
FUND_STALE_DAYS = 400
SUE_STALE_DAYS = 200
SIC_STALE_DAYS = 550
SI_STALE_DAYS = 45
OPERATING_DAYS = 400  # core's is_operating (panel column is_operating_vendor, audit only): vendor earnFlag 0 day within 400 days
INDEX_ID_MIN = 1_000_000_000_000  # vendor index namespace (NDX 1001001001003, RUT, DJX, ...)
PANEL_SCHEMA = "atx.alpha-panel.panel/v2"
# stage manifests the assembled panel reads besides ASOF_SOURCES (their SHA-256 is bound in the panel manifest)
INPUT_MANIFESTS = ("prices/manifest.json", "identity/link_table_manifest.json", "fundamentals/manifest.json",
                   "short_interest/manifest.json", "short_volume/manifest.json", "security_master/manifest.json",
                   "earnings_calendar/manifest.json")

FUND_ITEMS = (
    "at", "lt", "che", "debt", "be", "seq", "sale_q", "sale_ttm", "cogs_ttm", "xsga_ttm", "gp_ttm", "oi_ttm",
    "ni_q", "ni_ttm", "cfo_ttm", "capx_ttm", "xrd_ttm", "dvc_ttm", "prstkc_ttm", "sstk_ttm", "dp_ttm", "txt_q",
    "shrs_q", "noa", "invt", "rect", "ppe", "at_lag4", "be_lag1q", "be_lag1q_lag4", "ni_q_lag4", "txt_q_lag4",
    "shrs_q_lag4", "noa_lag4", "sale_q_lag4", "fscore",
)


def _tmp(name: str) -> Path:
    p = C.build_root() / "_tmp" / name
    p.mkdir(parents=True, exist_ok=True)
    return p


def _prices_glob() -> str:
    return (C.build_root() / "prices" / "*" / "*.parquet").as_posix()


def core_bucket(con, b: int) -> dict[str, Any]:
    dest = _tmp("panel_core") / f"bucket_{b:02d}.parquet"
    cal = C.calendar_path().as_posix()
    sql = f"""
    WITH cal AS (SELECT session_date, row_number() OVER (ORDER BY session_date) AS sidx FROM read_parquet('{cal}')),
    p AS (
        SELECT p.*, cal.sidx,
               CASE WHEN fb_action = 'repaired' OR NOT (return_factor > 0 AND isfinite(return_factor)) THEN 0.0
                    ELSE ln(return_factor) END AS lrf
        FROM read_parquet('{_prices_glob()}', hive_partitioning = false) p
        JOIN cal USING (session_date)
        WHERE p.security_id % {BUCKETS} = {b}
    ),
    w AS (
        SELECT *,
            sum(lrf) OVER (PARTITION BY security_id ORDER BY session_date ROWS UNBOUNDED PRECEDING) AS clf,
            avg(dollar_volume) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS BETWEEN {LOOKBACK} PRECEDING AND 1 PRECEDING) AS adv63,
            count(dollar_volume) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS BETWEEN {LOOKBACK} PRECEDING AND 1 PRECEDING) AS adv63_n,
            lag(sidx, {LOOKBACK}) OVER (PARTITION BY security_id ORDER BY session_date) AS sidx_lag63,
            lag(close) OVER (PARTITION BY security_id ORDER BY session_date) AS raw_close_prev,
            lag(sidx) OVER (PARTITION BY security_id ORDER BY session_date) AS sidx_prev,
            bool_or(shares_above_ceiling) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS UNBOUNDED PRECEDING) AS shares_withheld,
            max(CASE WHEN earn_flag = '0' THEN session_date END) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS UNBOUNDED PRECEDING) AS last_earn_date
        FROM p
    ),
    sh_obs AS (
        SELECT security_id, session_date AS obs_date, shares_vendor AS obs_shares, clf AS obs_clf
        FROM w WHERE shares_vendor IS NOT NULL
    ),
    sh AS (
        SELECT w.security_id, w.session_date, o.obs_date, o.obs_shares, o.obs_clf
        FROM w ASOF LEFT JOIN sh_obs o
          ON w.security_id = o.security_id AND (w.session_date - INTERVAL {SHARES_LAG_DAYS} DAY) >= o.obs_date
    ),
    j AS (
        SELECT w.*, sh.obs_date AS shares_obs_date,
            CASE WHEN sh.obs_date IS NULL OR sh.obs_date < w.session_date - INTERVAL {SHARES_LAG_DAYS + SHARES_MAX_AGE_DAYS} DAY
                      OR w.shares_withheld THEN NULL
                 ELSE sh.obs_shares * exp(-(w.clf - sh.obs_clf)) END AS shares_out_a8
        FROM w JOIN sh USING (security_id, session_date)
    ),
    t AS (
        SELECT *,
            -- trailing median daily volume in the session's share basis: volume / exp(clf) is basis-free
            median(volume * exp(-clf)) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS BETWEEN {TURNOVER_WINDOW - 1} PRECEDING AND CURRENT ROW) * exp(clf) AS med_vol21,
            count(volume) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS BETWEEN {TURNOVER_WINDOW - 1} PRECEDING AND CURRENT ROW) AS med_vol21_n
        FROM j
    )
    SELECT session_date, security_id, sidx, ticker, open, high, low, close AS raw_close, adj_close AS close,
           ret, ret_guarded, volume, dollar_volume, raw_close_prev, (sidx_prev = sidx - 1) AS present_prev,
           CASE WHEN adv63_n = {LOOKBACK} AND sidx_lag63 = sidx - {LOOKBACK} AND sidx_prev = sidx - 1
                     AND raw_close_prev > {PRICE_MIN} AND adv63 > {ADV_MIN} THEN adv63 END AS adv63_eligible,
           adv63,
           CASE WHEN shares_out_a8 BETWEEN {SHARES_DOMAIN[0]} AND {SHARES_DOMAIN[1]}
                     AND NOT (med_vol21_n >= {TURNOVER_MIN_OBS} AND med_vol21 > {TURNOVER_MAX} * shares_out_a8)
                THEN shares_out_a8 END AS shares_out_pre_si,
           shares_obs_date, shares_withheld,
           CASE earn_flag WHEN '0' THEN 1.0 WHEN '1' THEN 1.0 WHEN 'N' THEN 0.0 WHEN '-1' THEN 0.0 END AS earn_recent,
           earn_flag, gics,
           coalesce(session_date - last_earn_date <= {OPERATING_DAYS}, false) AS is_operating,
           security_id >= {INDEX_ID_MIN} AS is_index,
           iv_atm_5d, iv_atm_10d, iv_atm_21d, iv_atm_42d, iv_atm_63d, iv_atm_126d, iv_atm_252d
    FROM t
    """
    rows = C.copy_to_parquet(con, sql, dest)
    return {"rows": rows}


def membership_year(con, year: int) -> dict[str, Any]:
    dest = _tmp("panel_member") / f"year={year}.parquet"
    core = (_tmp("panel_core")).as_posix() + "/*.parquet"
    lo = f"DATE '{year}-01-01'"
    hi = f"DATE '{year}-12-31'"
    # include the previous session of the year for mkt_ret's d-1 membership
    sql = f"""
    WITH c AS (
        SELECT session_date, security_id, sidx, adv63_eligible, ret, ret_guarded, present_prev
        FROM read_parquet('{core}')
        WHERE session_date BETWEEN {lo} - INTERVAL 10 DAY AND {hi}
    ),
    r AS (
        SELECT *, CASE WHEN adv63_eligible IS NOT NULL THEN
                 row_number() OVER (PARTITION BY session_date ORDER BY adv63_eligible DESC NULLS LAST, security_id) END AS adv_rank
        FROM c
    ),
    m AS (
        SELECT *, coalesce(adv_rank <= {TOP_N}, false) AS member FROM r
    ),
    mm AS (
        SELECT m.*, lag(member) OVER (PARTITION BY security_id ORDER BY sidx) AS member_prev_row,
               lag(sidx) OVER (PARTITION BY security_id ORDER BY sidx) AS sidx_prev_row
        FROM m
    ),
    mk AS (
        SELECT session_date,
               avg(ret) FILTER (WHERE member_prev_row AND sidx_prev_row = sidx - 1 AND NOT ret_guarded AND ret IS NOT NULL) AS mkt_ret,
               count(*) FILTER (WHERE member_prev_row AND sidx_prev_row = sidx - 1 AND NOT ret_guarded AND ret IS NOT NULL) AS mkt_n,
               count(*) FILTER (WHERE member) AS members
        FROM mm GROUP BY 1
    )
    SELECT mm.session_date, mm.security_id, mm.member, mm.adv_rank, mk.mkt_ret, mk.mkt_n
    FROM mm JOIN mk USING (session_date)
    WHERE mm.session_date BETWEEN {lo} AND {hi}
    """
    rows = C.copy_to_parquet(con, sql, dest)
    stats = con.execute(
        f"""SELECT count(DISTINCT session_date), min(n), max(n), avg(n), min(k), avg(k)
            FROM (SELECT session_date, sum(member::INT) n, any_value(mkt_n) k FROM read_parquet('{dest.as_posix()}') GROUP BY 1)"""
    ).fetchone()
    return {"rows": rows, "sessions": stats[0], "members_min": stats[1], "members_max": stats[2],
            "members_mean": round(float(stats[3]), 1), "mkt_contributors_min": stats[4],
            "mkt_contributors_mean": round(float(stats[5] or 0), 1)}


EVENT_KEY_COLUMNS = frozenset({"cik", "accession", "form", "filed", "clock_utc", "clock_basis", "period_end",
                               "fiscal_year", "fiscal_period", "sue"})


def event_items(con, events: Path) -> list[str]:
    """Every item column of the fundamentals events file (all columns but the event keys), in file order."""
    cols = con.execute(f"DESCRIBE SELECT * FROM read_parquet('{events.as_posix()}')").fetchall()
    return [c[0] for c in cols if c[0] not in EVENT_KEY_COLUMNS]


# U3 flags, evaluated over the assembled row ``y`` (DuckDB resolves earlier aliases of the same SELECT list).
_NAMED = "NOT IN ('unknown', 'common_unverified')"
SECURITY_FLAGS = f"""
    coalesce(CASE WHEN y.finra_type {_NAMED} THEN y.finra_type END,
             CASE WHEN y.directory_type {_NAMED} THEN y.directory_type END,
             y.finra_type, y.directory_type) AS security_type,
    CASE WHEN y.finra_type {_NAMED} THEN 'finra_name'
         WHEN y.directory_type {_NAMED} THEN 'directory_snapshot'
         WHEN y.finra_type IS NOT NULL THEN 'finra_name'
         WHEN y.directory_type IS NOT NULL THEN 'directory_snapshot'
         ELSE 'vendor_only' END AS security_type_basis,
    coalesce(security_type IN ('ETF', 'fund', 'ETN'), false) OR coalesce(y.directory_etf, false) AS is_etf,
    coalesce(security_type = 'ADR', false) AS is_adr,
    coalesce(security_type = 'preferred', false) AS is_preferred,
    coalesce(security_type IN ('unit', 'warrant', 'right'), false) AS is_unit_warrant_right,
    coalesce(security_type = 'LP', false) AS is_lp,
    coalesce(security_type = 'note', false) AS is_note,
    coalesce(y.sic = 6798, false) OR coalesce(security_type = 'REIT', false) AS is_reit,
    coalesce(y.sic = 6792, false) OR coalesce(upper(y.finra_issue_name) LIKE '%ROYALTY TR%', false) AS is_royalty_trust,
    coalesce(security_type = 'spac', false)
        OR (coalesce(y.sic = 6770, false) AND coalesce(y.sale_ttm, 0) = 0) AS is_spac,  -- SIC 6770 outlives the de-SPAC
    y.is_index AS is_index_line,
    coalesce(y.filer_regime IN ('fpi_20f', 'canadian_40f'), false) AS is_fpi,
    is_adr OR (coalesce(y.filer_regime = 'fpi_20f', false) AND coalesce(security_type = 'common_unverified', false))
        AS is_adr_likely,
    CASE WHEN security_type IS NOT NULL
         THEN NOT y.is_index AND security_type IN ('common', 'common_unverified', 'REIT') AND NOT is_etf END AS is_common,
    coalesce(y.link_share_class, y.finra_share_class) AS share_class,
    y.cik AS share_class_group_id"""

# Operating company (SEC evidence, never the vendor earnings flags): an equity-type line (named common, ADR, REIT
# or LP; not an ETP, SPAC, preferred, unit, warrant, right or note; not a vendor index line) whose linked issuer filed
# a periodic report (10-K / 10-Q / 20-F / 40-F family) within 400 days, or that has no CIK link (identity gap:
# kept, so linked coverage is measured against it). ``member_equity`` = member and operating.
OPERATING_FLAGS = """
    NOT y.is_index AND coalesce(security_type IN ('common', 'common_unverified', 'ADR', 'REIT', 'LP'), false)
        AND NOT is_etf AND NOT is_spac
        AND (y.cik IS NULL OR coalesce(y.session_date - y.sec_last_periodic_date <= 400, false)) AS is_operating,
    y.member AND is_operating AS member_equity"""

# As-of joined producer outputs (request sections 1 and 3). Each entry: stage directory, parquet glob (relative
# to the stage), key (security_id | cik), clock column (UTC timestamp, the value's dissemination), staleness in
# calendar days after the clock (None = never stale), optional filter, tiebreak for equal clocks, and
# {source column: panel column}. A value is visible at session d when clock < 22:00 UTC of session d-1.
ASOF_SOURCES: list[dict[str, Any]] = [
    {"name": "filer", "stage": "sec_filings", "glob": "filer_regime.parquet", "key": "cik", "clock": "available_at",
     "stale": None, "tiebreak": "valid_from", "columns": {"regime": "filer_regime"},
     "types": {"filer_regime": "VARCHAR"}},
    # SEC fails-to-deliver: the latest settlement row of the latest visible half-month file (absence is not zero)
    {"name": "ftd", "stage": "ftd", "glob": "year=*/ftd.parquet", "key": "security_id",
     "clock": "CAST(available_at AS TIMESTAMP)", "stale": 30, "tiebreak": "settlement_date",
     "filter": "security_id IS NOT NULL",
     "columns": {"quantity": "ftd_quantity", "settlement_date": "ftd_settlement_date", "price": "ftd_price",
                 "vintage_risk": "ftd_vintage_risk"},
     "types": {"ftd_settlement_date": "DATE", "ftd_vintage_risk": "BOOLEAN"}},
    # latest periodic report of the linked issuer (operating-company evidence)
    {"name": "periodic", "stage": "sec_filings", "glob": "filings.parquet", "key": "cik", "clock": "available_at",
     "stale": 500, "tiebreak": "accession",
     "filter": "form IN ('10-K', '10-Q', '10-KT', '10-QT', '20-F', '40-F', '10-K/A', '10-Q/A', '20-F/A', '40-F/A')",
     "columns": {"form": "sec_last_periodic_form", "filing_date": "sec_last_periodic_date"},
     "types": {"sec_last_periodic_form": "VARCHAR", "sec_last_periodic_date": "DATE"}},
    # Form 4 open-market trades (non-derivative P / S): the issuer's latest visible one within 365 days
    {"name": "insider", "stage": "insider", "glob": "transactions/*/*.parquet", "key": "cik",
     "clock": "available_at", "stale": 365, "tiebreak": "transaction_date, accession, trans_sk",
     "filter": "table_type = 'non_derivative' AND transaction_code IN ('P', 'S') AND issuer_cik IS NOT NULL",
     "key_expr": "issuer_cik",
     "columns": {"transaction_code": "insider_last_code", "transaction_date": "insider_last_tx_date",
                 "shares": "insider_last_shares", "price": "insider_last_price"},
     "types": {"insider_last_code": "VARCHAR", "insider_last_tx_date": "DATE"}},
    # FINRA CNMS reporting facilities of the latest visible short-volume file row (exempt volume is sv_short_exempt)
    {"name": "svx", "stage": "short_volume_ext", "glob": "year=*/*.parquet", "key": "security_id",
     "clock": "CAST(available_at AS TIMESTAMP)", "stale": 7, "tiebreak": "trade_date", "filter": "security_id IS NOT NULL",
     "columns": {"n_facilities": "sv_n_facilities", "market": "sv_market"}, "types": {"sv_market": "VARCHAR"}},
    # Reg SHO threshold lists: the latest visible list row of the security within 7 days (absence = not listed)
    {"name": "regsho", "stage": "regsho_threshold", "glob": "year=*/*.parquet", "key": "security_id",
     "clock": "CAST(available_at AS TIMESTAMP)", "stale": 7, "tiebreak": "list_date, market",
     "filter": "security_id IS NOT NULL AND on_list",
     "columns": {"list_date": "regsho_last_list_date", "run_days": "regsho_run_days", "market": "regsho_market"},
     "types": {"regsho_last_list_date": "DATE", "regsho_market": "VARCHAR"}},
    # 8-K Item 2.02 earnings announcements: the issuer's latest visible primary announcement within 200 days
    {"name": "earn", "stage": "earnings_calendar", "glob": "announcements.parquet", "key": "cik",
     "clock": "available_at", "stale": 200, "tiebreak": "announcement_utc, accession", "filter": "is_primary",
     "columns": {"announcement_utc": "earn_last_utc", "timing": "earn_last_timing",
                 "reaction_session": "earn_last_reaction_session", "fiscal_period_end": "earn_last_period_end",
                 "next_expected_date": "earn_next_expected_date"},
     "types": {"earn_last_utc": "TIMESTAMP", "earn_last_timing": "VARCHAR", "earn_last_reaction_session": "DATE",
               "earn_last_period_end": "DATE", "earn_next_expected_date": "DATE"}},
    # 13F institutional holdings, as-of-45-days version (filings up to the deadline): the latest visible quarter;
    # stale 105 days after its clock (~150 days after the quarter end, the stage's staleness rule)
    {"name": "inst", "stage": "thirteenf", "glob": "agg_asof45.parquet", "key": "security_id",
     "clock": "available_at", "stale": 105, "tiebreak": "period_of_report",
     "columns": {"period_of_report": "inst_period", "inst_shares": "inst_shares", "n_holders": "inst_n_holders",
                 "top10_share": "inst_top10_share", "d_inst_shares": "inst_d_shares",
                 "pct_inst_shares": "inst_pct_change", "d_n_holders": "inst_d_holders"},
     "types": {"inst_period": "DATE"}},
]


def asof_sources(con, ctes: list[str], joins: list[str], root: Path, lo: str = "", hi: str = "") -> tuple[list[str], dict[str, bool]]:
    cols: list[str] = []
    have: dict[str, bool] = {}
    for spec in ASOF_SOURCES:
        name, stage_path = spec["name"], root / spec["stage"]
        files = sorted(stage_path.glob(spec["glob"])) if stage_path.exists() else []
        ok = bool(files) and (stage_path / "manifest.json").exists()
        have[f"src_{name}"] = ok
        out = spec["columns"]
        if not ok:
            cols += [f"CAST(NULL AS {spec.get('types', {}).get(c, 'DOUBLE')}) AS {c}" for c in out.values()]
            continue
        glob = (stage_path / spec["glob"]).as_posix()
        kx = spec.get("key_expr", spec["key"])
        conds = [spec["filter"]] if spec.get("filter") else []
        if hi:  # only rows that can be the latest visible value inside the month
            conds.append(f"{spec['clock']} < CAST({hi} AS TIMESTAMP) + INTERVAL 1 DAY")
            if spec["stale"] is not None and lo:
                conds.append(f"{spec['clock']} >= CAST({lo} AS TIMESTAMP) - INTERVAL {int(spec['stale']) + 10} DAY")
        where = ("WHERE " + " AND ".join(f"({c})" for c in conds)) if conds else ""
        ctes.append(
            f"""src_{name} AS (
                SELECT b.session_date, b.security_id, s.c AS {name}_clock, {", ".join(f's."{a}" AS "{a}"' for a in out)}
                FROM bk b ASOF JOIN (
                    SELECT {kx} AS k, {spec["clock"]} AS c, {", ".join(f'"{a}"' for a in out)}
                    FROM read_parquet('{glob}', union_by_name = true) {where}
                    QUALIFY row_number() OVER (PARTITION BY {kx}, {spec["clock"]}
                                               ORDER BY {", ".join(t.strip() + " DESC" for t in spec["tiebreak"].split(","))}) = 1
                ) s ON b.{spec["key"]} = s.k AND b.issuer_cutoff > s.c)"""
        )
        joins.append(f"LEFT JOIN src_{name} USING (session_date, security_id)")
        fresh = "TRUE" if spec["stale"] is None else \
            f"(b.session_date - CAST(src_{name}.{name}_clock AS DATE)) <= {spec['stale']}"
        cols += [f'CASE WHEN {fresh} THEN src_{name}."{a}" END AS {p}' for a, p in out.items()]
    return cols, have


def split_ctes(text: str) -> list[tuple[str, str]]:
    """Top-level ``name AS (body)`` definitions of a CTE list string (quotes and nested parentheses respected)."""
    out, i, n = [], 0, len(text)
    while i < n:
        while i < n and text[i] in " \t\r\n,":
            i += 1
        if i >= n:
            break
        j = text.index(" AS (", i)
        name = text[i:j].strip()
        k, depth, quote = j + 4, 0, None
        while k < n:
            ch = text[k]
            if quote:
                if ch == quote:
                    quote = None
            elif ch in "'\"":
                quote = ch
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        out.append((name, text[j + 5:k]))
        i = k + 1
    return out


_ALIAS = re.compile(r"\b(lk|fq|sq|sij|svv|fnm|sml|ernj|src_\w+)\.")
_OUT_NAME = re.compile(r'(?:\bAS\s+"?(\w+)"?|\.(\w+))\s*$')


def join_rows(con, base_cols: str, select_extra: list[str], joins: list[str]) -> None:
    """Table ``x`` = base columns + every select_extra column, adding one joined table at a time (each step is a
    disk-backed table, so no step holds more than one join's hash table)."""
    order = {re.search(r"JOIN (\w+)", j).group(1): k for k, j in enumerate(joins)}
    items: dict[str, list[str]] = {}
    for it in select_extra:  # an item goes to the step of the last-joined table it reads
        found = [a for a in _ALIAS.findall(it) if a in order]
        items.setdefault(max(found, key=order.get) if found else "", []).append(it)
    names = [(_OUT_NAME.search(it.strip()).group(1) or _OUT_NAME.search(it.strip()).group(2)) for it in select_extra]
    con.execute("CREATE OR REPLACE TABLE x0 AS SELECT b.* RENAME (earn_recent AS earn_recent_vendor, "
                "earn_flag AS earn_flag_vendor, is_operating AS is_operating_vendor), "
                f"{', '.join(items.get('', []) or ['NULL AS _none'])} FROM base b")
    prev = "x0"
    for k, j in enumerate(joins, start=1):
        alias = re.search(r"JOIN (\w+)", j).group(1)
        cols = items.get(alias, [])
        con.execute(f"CREATE OR REPLACE TABLE x{k} AS SELECT b.*{''.join(', ' + c for c in cols)} FROM {prev} b {j}")
        con.execute(f"DROP TABLE {prev}")
        prev = f"x{k}"
    final = ", ".join(f'b."{n}"' for n in names)
    con.execute(f"CREATE OR REPLACE TABLE x AS SELECT {base_cols}, {final} FROM {prev} b")
    con.execute(f"DROP TABLE {prev}")


def materialize(con, ctes: list[str]) -> None:
    """Create every CTE as a table, in order (later definitions read earlier ones by name)."""
    for entry in ctes:
        for name, body in split_ctes(entry):
            con.execute(f"CREATE OR REPLACE TABLE {name} AS {body}")


def _exists(path: Path) -> bool:
    return path.exists() and any(path.parent.glob(path.name)) if "*" in path.name else path.exists()


def assemble_month(con, year: int, month: int) -> dict[str, Any]:
    root = C.build_root()
    dest = C.stage_dir("panel") / f"year={year}" / f"panel-{month:02d}.parquet"
    core = (root / "_tmp" / "panel_core").as_posix() + "/*.parquet"
    member = (root / "_tmp" / "panel_member" / f"year={year}.parquet").as_posix()
    link_table = root / "identity" / "link_table.parquet"
    links = root / "identity" / "links_combined.parquet"
    if not links.exists():
        links = root / "identity" / "links.parquet"
    events = root / "fundamentals" / "events.parquet"
    sic = root / "fundamentals" / "sic_events.parquet"
    si = root / "short_interest" / "si.parquet"
    sv_dir = root / "short_volume"
    have = {
        "identity": links.exists(),
        "fundamentals": events.exists() and links.exists(),
        "industry": sic.exists() and links.exists(),
        "short_interest": si.exists(),
        "short_volume": sv_dir.exists() and any(sv_dir.glob("*/*.parquet")),
        "security_master": (root / "security_master" / "manifest.json").exists(),
        "link_table": link_table.exists(),
    }
    lo = f"DATE '{year}-{month:02d}-01'"
    hi = f"(DATE '{year}-{month:02d}-01' + INTERVAL 1 MONTH - INTERVAL 1 DAY)"
    cal = C.calendar_path().as_posix()
    ctes = [
        f"""cal AS (SELECT session_date, lag(session_date) OVER (ORDER BY session_date) AS prev_session
                    FROM read_parquet('{cal}'))""",
        f"""base AS (
            SELECT c.*, m.member, m.adv_rank, m.mkt_ret, cal.prev_session,
                   CAST(cal.prev_session AS TIMESTAMP) + INTERVAL {C.MARK_HOUR_UTC} HOUR AS issuer_cutoff
            FROM read_parquet('{core}') c
            JOIN read_parquet('{member}') m USING (session_date, security_id)
            JOIN cal USING (session_date)
            WHERE c.session_date BETWEEN {lo} AND {hi})""",
    ]
    select_extra: list[str] = []
    joins: list[str] = []
    if have["identity"]:
        if link_table.exists():
            # a link is used at session d once it is known (backfill: once its dated evidence exists)
            gate = ("(CASE WHEN l.link_tier = 'backfill' THEN l.evidence_at ELSE l.available_at END) "
                    f"<= CAST(b.session_date AS TIMESTAMP) + INTERVAL {C.MARK_HOUR_UTC} HOUR")
            ctes.append(
                f"""lk AS (SELECT b.session_date, b.security_id, l.cik, l.link_tier, l.share_class AS link_share_class
                    FROM base b JOIN (SELECT * FROM read_parquet('{link_table.as_posix()}')
                                      WHERE valid_from <= {hi} AND valid_to >= {lo}) l
                      ON b.security_id = l.security_id AND b.session_date BETWEEN l.valid_from AND l.valid_to
                    WHERE {gate})"""
            )
        else:
            ctes.append(
                f"""lk AS (SELECT b.session_date, b.security_id, l.cik,
                           CASE WHEN l.tier IN ('backfill', 'name') THEN l.tier ELSE 'strict' END AS link_tier,
                           CAST(NULL AS VARCHAR) AS link_share_class
                    FROM base b JOIN read_parquet('{links.as_posix()}') l
                      ON b.security_id = l.security_id AND b.session_date BETWEEN l.start AND l.end_incl)"""
            )
        ctes.append("bk AS (SELECT b.*, lk.cik FROM base b LEFT JOIN lk USING (session_date, security_id))")
        joins.append("LEFT JOIN lk USING (session_date, security_id)")
        select_extra += ["lk.cik", "lk.link_tier", "lk.link_share_class"]
    else:
        ctes.append("bk AS (SELECT b.*, CAST(NULL AS BIGINT) AS cik FROM base b)")
        select_extra += ["CAST(NULL AS BIGINT) AS cik", "CAST(NULL AS VARCHAR) AS link_tier",
                         "CAST(NULL AS VARCHAR) AS link_share_class"]
    fund_items = event_items(con, events) if have["fundamentals"] else list(FUND_ITEMS)
    if have["fundamentals"]:
        items = ", ".join(f'e."{c}"' for c in fund_items)
        ctes.append(
            f"""ev AS (SELECT cik, clock_utc, period_end, sue, form, clock_basis, {", ".join(f'"{c}"' for c in fund_items)}
                       FROM read_parquet('{events.as_posix()}')
                       QUALIFY row_number() OVER (PARTITION BY cik, clock_utc ORDER BY period_end DESC, accession DESC) = 1),
                fq AS (SELECT b.session_date, b.security_id, e.clock_utc AS fund_clock, e.period_end AS fund_period_end,
                              e.sue AS sue_raw, e.form AS fund_form, e.clock_basis AS fund_clock_basis, {items}
                       FROM (SELECT b.*, lk.cik FROM base b JOIN lk USING (session_date, security_id)) b
                       ASOF JOIN ev e ON b.cik = e.cik AND b.issuer_cutoff > e.clock_utc)"""
        )
        joins.append("LEFT JOIN fq USING (session_date, security_id)")
        stale = f"(b.session_date - fq.fund_period_end) <= {FUND_STALE_DAYS}"
        select_extra += ["fq.fund_clock", "fq.fund_period_end", "fq.fund_form", "fq.fund_clock_basis"]
        select_extra += [f'CASE WHEN {stale} THEN fq."{c}" END AS "{c}"' for c in fund_items]
        select_extra.append(f"CASE WHEN (b.session_date - fq.fund_period_end) <= {SUE_STALE_DAYS} THEN fq.sue_raw END AS sue")
    else:
        select_extra += [f'CAST(NULL AS DOUBLE) AS "{c}"' for c in (*FUND_ITEMS, "sue")]
    if have["industry"]:
        ctes.append(
            f"""sq AS (SELECT b.session_date, b.security_id, s.clock_utc AS sic_clock, s.sic, s.sic2, s.ff12, s.ff49
                       FROM (SELECT b.*, lk.cik FROM base b JOIN lk USING (session_date, security_id)) b
                       ASOF JOIN (SELECT * FROM read_parquet('{sic.as_posix()}')
                                  QUALIFY row_number() OVER (PARTITION BY cik, clock_utc ORDER BY sic) = 1) s
                         ON b.cik = s.cik AND b.issuer_cutoff > s.clock_utc)"""
        )
        joins.append("LEFT JOIN sq USING (session_date, security_id)")
        fresh = f"(b.issuer_cutoff - sq.sic_clock) <= INTERVAL {SIC_STALE_DAYS} DAY"
        select_extra += [f"CASE WHEN {fresh} THEN sq.sic END AS sic",
                         f"CASE WHEN {fresh} THEN sq.sic2 END AS grp_sic2",
                         f"CASE WHEN {fresh} THEN sq.ff12 END AS grp_ff12",
                         f"CASE WHEN {fresh} THEN sq.ff49 END AS grp_ff49"]
    else:
        select_extra += ["NULL AS sic", "NULL AS grp_sic2", "NULL AS grp_ff12", "NULL AS grp_ff49"]
    if have["short_interest"]:
        ctes.append(
            f"""siv AS (SELECT security_id, dissemination_date, settlement_date, si_shares, si_dtc
                        FROM read_parquet('{si.as_posix()}')),
                sij AS (SELECT b.session_date, b.security_id, s.dissemination_date AS si_dissemination,
                               s.settlement_date AS si_settlement, s.si_shares AS si_shares_raw, s.si_dtc AS si_dtc_raw
                        FROM base b ASOF JOIN siv s
                          ON b.security_id = s.security_id AND b.session_date > s.dissemination_date)"""
        )
        joins.append("LEFT JOIN sij USING (session_date, security_id)")
        fresh = f"(b.session_date - sij.si_dissemination) <= {SI_STALE_DAYS}"
        select_extra += ["sij.si_settlement", f"CASE WHEN {fresh} THEN sij.si_shares_raw END AS si_shares",
                         f"CASE WHEN {fresh} THEN sij.si_dtc_raw END AS si_dtc"]
    else:
        select_extra += ["CAST(NULL AS DATE) AS si_settlement", "CAST(NULL AS DOUBLE) AS si_shares",
                         "CAST(NULL AS DOUBLE) AS si_dtc"]
    if have["short_volume"]:
        ctes.append(
            f"""svv AS (SELECT security_id, trade_date, short_volume, short_exempt_volume, total_volume
                        FROM read_parquet('{sv_dir.as_posix()}/*/*.parquet', hive_partitioning = false)
                        WHERE trade_date BETWEEN {lo} - INTERVAL 10 DAY AND {hi})"""
        )
        joins.append("LEFT JOIN svv ON svv.security_id = b.security_id AND svv.trade_date = b.prev_session")
        select_extra += ["svv.short_volume AS sv_short_volume", "svv.short_exempt_volume AS sv_short_exempt",
                         "svv.total_volume AS sv_total_volume"]
    else:
        select_extra += ["CAST(NULL AS DOUBLE) AS sv_short_volume", "CAST(NULL AS DOUBLE) AS sv_short_exempt",
                         "CAST(NULL AS DOUBLE) AS sv_total_volume"]
    if have["security_master"]:
        sm = root / "security_master"
        ctes.append(
            f"""fnm AS (SELECT b.session_date, b.security_id, f.dissemination_date AS fn_dissem, f.finra_type,
                               f.exchange, f.share_class AS finra_share_class, f.issue_name
                        FROM base b ASOF JOIN (SELECT * FROM read_parquet('{(sm / "finra_names.parquet").as_posix()}')
                                               WHERE issue_name IS NOT NULL) f
                          ON b.security_id = f.security_id AND b.session_date > f.dissemination_date),
                sml AS (SELECT * FROM read_parquet('{(sm / "lines.parquet").as_posix()}'))"""
        )
        joins.append("LEFT JOIN fnm USING (session_date, security_id)")
        joins.append("LEFT JOIN sml ON sml.security_id = b.security_id")
        fresh = f"(b.session_date - fnm.fn_dissem) <= {SI_STALE_DAYS}"
        select_extra += [f"CASE WHEN {fresh} THEN fnm.finra_type END AS finra_type",
                         f"CASE WHEN {fresh} THEN fnm.exchange END AS exchange",
                         f"CASE WHEN {fresh} THEN fnm.issue_name END AS finra_issue_name",
                         f"CASE WHEN {fresh} THEN fnm.finra_share_class END AS finra_share_class",
                         "sml.directory_type", "sml.directory_etf",
                         "CASE WHEN NOT sml.left_censored THEN sml.first_session END AS listing_date",
                         "sml.delisting_date"]
    else:
        select_extra += ["CAST(NULL AS VARCHAR) AS finra_type", "CAST(NULL AS VARCHAR) AS exchange",
                         "CAST(NULL AS VARCHAR) AS finra_issue_name", "CAST(NULL AS VARCHAR) AS finra_share_class",
                         "CAST(NULL AS VARCHAR) AS directory_type", "CAST(NULL AS BOOLEAN) AS directory_etf",
                         "CAST(NULL AS DATE) AS listing_date", "CAST(NULL AS DATE) AS delisting_date"]
    ann = root / "earnings_calendar" / "announcements.parquet"
    have["earnings_calendar"] = ann.exists() and (root / "earnings_calendar" / "manifest.json").exists()
    if have["earnings_calendar"]:
        # earn_recent from SEC 8-K Item 2.02 announcements: 1 on the reaction session and the session after it
        # (the same two sessions the vendor flag codes 0 / 1 meant), only once the announcement is known
        ctes.append(
            f"""ern AS (SELECT a.cik, a.reaction_session AS d0, c.next_session AS d1
                        FROM read_parquet('{ann.as_posix()}') a
                        JOIN (SELECT session_date, lead(session_date) OVER (ORDER BY session_date) AS next_session
                              FROM read_parquet('{cal}')) c ON c.session_date = a.reaction_session
                        WHERE a.is_primary AND a.reaction_session BETWEEN {lo} - INTERVAL 10 DAY AND {hi}
                          AND a.available_at <= CAST(a.reaction_session AS TIMESTAMP) + INTERVAL {C.MARK_HOUR_UTC} HOUR),
                ernj AS (SELECT session_date, security_id, min(k) AS earn_day_offset FROM (
                             SELECT b.session_date, b.security_id, 0 AS k FROM bk b JOIN ern e ON b.cik = e.cik AND b.session_date = e.d0
                             UNION ALL
                             SELECT b.session_date, b.security_id, 1 FROM bk b JOIN ern e ON b.cik = e.cik AND b.session_date = e.d1)
                         GROUP BY 1, 2)"""
        )
        joins.append("LEFT JOIN ernj USING (session_date, security_id)")
        select_extra += ["CASE WHEN b.cik IS NULL THEN NULL WHEN ernj.earn_day_offset IS NOT NULL THEN 1.0 ELSE 0.0 END AS earn_recent",
                         "ernj.earn_day_offset"]
    else:
        select_extra += ["CAST(NULL AS DOUBLE) AS earn_recent", "CAST(NULL AS INTEGER) AS earn_day_offset"]
    src_cols, src_have = asof_sources(con, ctes, joins, root, lo, hi)
    select_extra += src_cols
    have.update(src_have)

    materialize(con, ctes)
    base_cols = """b.session_date, b.security_id, b.ticker, b.member, b.adv_rank, b.adv63, b.mkt_ret,
               b.open, b.high, b.low, b.raw_close, b.close, b.ret, b.ret_guarded, b.volume, b.dollar_volume,
               b.shares_out_pre_si, b.shares_obs_date, b.gics,
               b.earn_recent_vendor, b.earn_flag_vendor, b.is_operating_vendor,
               b.is_index,
               b.iv_atm_5d, b.iv_atm_10d, b.iv_atm_21d, b.iv_atm_42d, b.iv_atm_63d, b.iv_atm_126d, b.iv_atm_252d"""
    join_rows(con, base_cols, select_extra, joins)
    con.execute(f"""
    CREATE OR REPLACE TABLE y AS
        SELECT * EXCLUDE (shares_out_pre_si),
               CASE WHEN si_shares IS NOT NULL AND shares_out_pre_si IS NOT NULL
                         AND si_shares > {SI_RATIO_MAX} * shares_out_pre_si THEN NULL
                    ELSE shares_out_pre_si END AS shares_out
        FROM x
    """)
    con.execute("DROP TABLE x")
    # issuer aggregates on the (small) linked subset: market equity over the issuer's lines, the primary line
    con.execute("""
    CREATE OR REPLACE TABLE iss AS
        SELECT session_date, security_id,
               sum(shares_out * raw_close) OVER (PARTITION BY session_date, cik) AS me_company,
               row_number() OVER (PARTITION BY session_date, cik ORDER BY adv63 DESC NULLS LAST, security_id) = 1
                   AS is_issuer_primary
        FROM (SELECT session_date, security_id, cik, shares_out, raw_close, adv63 FROM y WHERE cik IS NOT NULL)
    """)
    pre = f"""
    SELECT y.*, y.shares_out * y.raw_close AS me_line,
           CASE WHEN y.open > 0 AND y.raw_close > 0 THEN y.raw_close / y.open - 1 END AS ret_intraday,
           CASE WHEN y.open > 0 AND y.raw_close > 0 AND NOT y.ret_guarded
                THEN (1 + y.ret) / (y.raw_close / y.open) - 1 END AS ret_overnight,
           {SECURITY_FLAGS},
           {OPERATING_FLAGS},
           iss.me_company, iss.is_issuer_primary
    FROM y LEFT JOIN iss USING (session_date, security_id)
    ORDER BY session_date, security_id
    """
    rows = C.copy_to_parquet(con, pre, dest, row_group_size=32768)
    return {"rows": rows, "inputs": have}


def run(stages: list[str], years: list[int]) -> dict[str, Any]:
    receipt: dict[str, Any] = {"stage": "panel"}
    con = C.connect(memory="600MB", threads=1 if "assemble" in stages else 2)
    if "core" in stages:
        with C.timed(receipt, "core"):
            receipt["core"] = {str(b): core_bucket(con, b) for b in range(BUCKETS)}
    if "membership" in stages:
        with C.timed(receipt, "membership"):
            receipt["membership"] = {str(y): membership_year(con, y) for y in years}
            print(receipt["membership"], flush=True)
    if "assemble" in stages:
        with C.timed(receipt, "assemble"):
            receipt["assemble"] = {}
            for y in years:
                ydir = C.stage_dir("panel") / f"year={y}"
                for old in ydir.glob("panel.parquet"):
                    old.unlink()  # superseded single-file layout
                months = {}
                for mth in range(1, 13):
                    has = con.execute(f"""SELECT count(*) FROM read_parquet('{C.calendar_path().as_posix()}')
                        WHERE year(session_date) = {y} AND month(session_date) = {mth}""").fetchone()[0]
                    if has:
                        # fresh file-backed scratch database per month: every join step is materialised and spills
                        name = f"panel_{y}_{mth:02d}.duckdb"
                        mcon = C.connect(memory="450MB", threads=1, db_file=name)
                        try:
                            months[str(mth)] = assemble_month(mcon, y, mth)
                        finally:
                            mcon.close()
                            for f in (C.build_root() / "_tmp").glob(name + "*"):
                                f.unlink(missing_ok=True)
                receipt["assemble"][str(y)] = {"rows": sum(v["rows"] for v in months.values()),
                                               "inputs": next(iter(months.values()))["inputs"] if months else {}}
                print(y, receipt["assemble"][str(y)], flush=True)
    manifest = C.stage_dir("panel") / "manifest.json"
    prior = C.read_json(manifest) if manifest.exists() else {}
    for k in ("schema", "status", "stage", "code", "files"):
        prior.pop(k, None)
    for k, v in receipt.items():  # per-year receipts merge, so year ranges can be assembled by separate runs
        prior[k] = {**prior.get(k, {}), **v} if k in ("assemble", "membership", "timings_s") else v
    prior["rules"] = __doc__
    root = C.build_root()
    inputs = {}
    for rel in INPUT_MANIFESTS + tuple(f"{s['stage']}/manifest.json" for s in ASOF_SOURCES):
        p = root / rel
        if p.exists() and rel not in inputs:
            inputs[rel] = C.sha256_file(p)
    prior["input_manifests_sha256"] = inputs
    C.write_stage_manifest("panel", PANEL_SCHEMA, ("panel", "common"), prior)
    return receipt


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stages", default="core,membership,assemble")
    ap.add_argument("--years", default="2018-2026")
    args = ap.parse_args(argv)
    a, _, b = args.years.partition("-")
    years = list(range(int(a), int(b or a) + 1))
    run([s.strip() for s in args.stages.split(",") if s.strip()], years)
    return 0


if __name__ == "__main__":
    sys.exit(main())
