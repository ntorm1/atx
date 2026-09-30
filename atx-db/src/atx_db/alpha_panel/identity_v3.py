"""S2.5 identity link table v3: a dated filing-evidence tier, the full FINRA name tier, CCM-style codes.

Inputs (read only): ``identity/links_combined.parquet`` (r4 strict + ``snapshot-run-backfill-v1`` intervals),
the insider stage (Forms 3/4/5 ``issuer_cik`` + ``issuer_symbol``), the NOTES cover-page stage
(``identity_cover/cover_page.parquet``: ``dei:TradingSymbol`` per class, when published), the vendor ticker table
of the shortflow stages, FINRA short-interest names, FSDS SUB names, the panel core (sessions, ``adv63``), the
security master (``finra_names``, listing events) and the v2 panel (``member_equity`` for the audit).

Tiers and precedence per line-session (one CIK per line-day, so no ambiguous line-days): ``strict`` (r4) >
``dated`` (rule ``filing-symbol-evidence-v1``) > ``name`` (``finra-name-match-v1``, recomputed on every session
without a strict link, backfill days included) > ``backfill`` (``snapshot-run-backfill-v1``, not point in time).

Rule ``filing-symbol-evidence-v1`` (point in time):

1. Evidence: every Form 3/4/5 accession (``issuer_cik``, ``issuer_symbol``; weight 1) and every cover-page
   security row with a ``dei:TradingSymbol`` (weight 2: the issuer's own tagging), each at its ``available_at``.
   Symbols are split with :func:`split_symbols` (``'LEN, LEN.B'`` -> LEN, LEN.B; exchange prefixes and brackets
   dropped; spaced single letters joined, ``'N O G'`` -> NOG).
2. A symbol maps to the vendor line carrying its canonical form on the last session on or before the filing
   date (within 7 days; two lines carrying it -> unmapped): the shared ``map_symbols_asof`` rule.
3. Session d of line L (cutoff d 22:00 UTC, as for the strict and name tiers): candidates are the CIKs with
   evidence mapped to L in (cutoff - ``WINDOW_DAYS``, cutoff]. The CIK with the largest weight in the last
   ``RECENT_DAYS`` days wins (then the largest weight in the full window, then the latest evidence); it links
   the day only when its weight in the full window is at least ``MIN_WEIGHT`` (two filings, or one cover page).
4. ``available_at`` of a day = the latest evidence at or before the cutoff; a run's ``available_at`` is its first
   day's, so every run satisfies ``available_at <= valid_from 22:00``.

Listing events (S2.2) carry no ticker, so they cannot tie a line to a CIK point in time; they corroborate
(``listing_event_match``) and feed ``linktype``.

Link table v3 (``identity/link_table_v3.parquet``): the v1 columns plus ``linktype`` / ``linkprim`` (CCM style)
and ``dated_sources``. ``linktype``: ``LC`` = researched (strict, or dated with a cover-page or listing-event
corroboration, or weight >= 4); ``LU`` = unresearched (name, backfill, other dated runs). ``linkprim``: ``P`` =
the issuer's primary line (highest prior-63-session dollar volume), ``J`` = another common / ADR class of the
issuer, ``N`` = a non-common line (preferred, unit, warrant, right, note, fund) linked to the issuer.

``security_master/share_exchange_history.parquet``: CRSP-style dated ``exchcd`` (1 NYSE, 2 NYSE American, 3
Nasdaq, 4 NYSE Arca, 5 Cboe BZX, 6 IEX (5-6 are our extensions), 0 OTC / none) and ``shrcd`` (first digit 1
common, 3 ADR, 4 closed-end fund / trust, 7 unit or LP or ETP; second digit 1 US common, 2 foreign-incorporated
or FPI, 8 REIT, 3 ETP) runs per line from the v2 panel's point-in-time security flags.

Exports ``export/identity-bridge-v3-{strict,pit,all}/`` follow the v2 bridge contract (``atx.identity-bridge/v1``);
``pit`` = strict + dated + name.

Usage (under the memory guard)::

    python -m atx_db.alpha_panel.identity_v3 evidence      # evidence + symbol map -> _tmp/identity_v3
    python -m atx_db.alpha_panel.identity_v3 names         # full name tier days -> _tmp/identity_v3
    python -m atx_db.alpha_panel.identity_v3 dated         # dated tier days -> _tmp/identity_v3
    python -m atx_db.alpha_panel.identity_v3 table         # link_table_v3 + exports + audit + manifest
    python -m atx_db.alpha_panel.identity_v3 codes         # share_exchange_history.parquet
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import shutil
import sys
import time
from pathlib import Path
from typing import Any

from . import common as C
from . import identity_names as N
from . import shortflow_common as S

RULE = "identity-link-table-v3"
DATED_RULE = "filing-symbol-evidence-v1"
WINDOW_DAYS = 400
RECENT_DAYS = 120
MIN_WEIGHT = 2
MEMORY = "220MB"
MODULES = ("identity_v3", "identity_names", "shortflow_common", "common")
DEAD_BEFORE = "2026-09-01"
FIRST_SESSION = "2017-10-01"

_PREFIX = re.compile(r"^(NYSE\s*AMERICAN|NYSE\s*MKT|NYSE\s*ARCA|NYSEAMERICAN|NYSEMKT|NASDAQ(\s*(GS|GM|CM))?|NASD|NYSE|"
                     r"AMEX|OTCBB|OTCQB|OTCQX|OTC\s*MARKETS|OTC|PINK|TSXV|TSX|CSE|CBOE|BATS)\s*[:/\-]\s*")
_JUNK = frozenset({"", "NONE", "N/A", "NA", "N A", "NOT APPLICABLE", "NOT LISTED", "NOTLISTED", "NO SYMBOL", "NIL",
                   "NULL", "PRIVATE", "N.A.", "TBD", "UNLISTED", "-"})


def split_symbols(raw: str | None) -> list[str]:
    """Issuer trading symbols in one Form 3/4/5 ``issuerTradingSymbol`` or cover-page value (upper case)."""
    if raw is None:
        return []
    s = raw.upper().strip()
    s = re.sub(r"[\[\]\(\)\"']", " ", s).strip()
    if s in _JUNK:
        return []
    parts = re.split(r"\s*(?:,|;|\bAND\b|&|\s\+\s|_)\s*", s)
    out: list[str] = []
    for p in parts:
        p = _PREFIX.sub("", p.strip()).strip()
        if not p or p in _JUNK:
            continue
        toks = p.split()
        if len(toks) > 1:
            if all(len(t) == 1 for t in toks):          # 'N O G' -> NOG
                toks = ["".join(toks)]
            else:
                toks = [_PREFIX.sub("", t) for t in toks]
        for t in toks:
            if "/" in t:
                a, _, b = t.partition("/")
                if len(a) >= 2 and len(b) >= 2 and "/" not in b:   # 'JWA/JWB' -> both; 'BRK/B' stays a class
                    out.extend([a, b])
                    continue
            if re.fullmatch(r"[A-Z0-9][A-Z0-9./\-]{0,9}", t) and t not in _JUNK:
                out.append(t)
    seen: set[str] = set()
    return [t for t in out if not (t in seen or seen.add(t))]


def work_dir() -> Path:
    p = C.build_root() / "_tmp" / "identity_v3"
    p.mkdir(parents=True, exist_ok=True)
    return p


def cover_path() -> Path:
    return C.build_root() / "identity_cover" / "cover_page.parquet"


def _vendor_glob() -> str:
    return (C.build_root() / "_tmp" / "shortflow" / "vendor" / "vendor_tickers_*.parquet").as_posix()


# ---------------------------------------------------------------- evidence
def evidence() -> dict[str, Any]:
    root = C.build_root()
    wd = work_dir()
    con = C.connect(memory=MEMORY, threads=2, db_file="identity_v3_evidence.duckdb")
    receipt: dict[str, Any] = {"rule": DATED_RULE}
    tx = (root / "insider" / "transactions" / "year=*" / "*.parquet").as_posix()
    con.execute(f"""CREATE TABLE ev0 AS
        SELECT DISTINCT issuer_cik AS cik, issuer_symbol AS symbol_raw, accession, filing_date AS key_date,
               CAST(available_at AS TIMESTAMP) AS available_at, 'form345' AS source, 1 AS weight,
               CAST(NULL AS VARCHAR) AS exchange
        FROM read_parquet('{tx}', hive_partitioning = false)
        WHERE issuer_cik IS NOT NULL AND issuer_symbol IS NOT NULL AND available_at IS NOT NULL""")
    cover = cover_path()
    receipt["cover_page"] = cover.exists()
    if cover.exists():
        cols = {r[0] for r in con.execute(f"DESCRIBE SELECT * FROM read_parquet('{cover.as_posix()}')").fetchall()}
        # a co-registrant's row speaks for the co-registrant: its tagged CIK, else the row is skipped
        who = "CASE WHEN coreg IS NULL THEN cik ELSE entity_cik END" if {"coreg", "entity_cik"} <= cols else "cik"
        sym = "symbol_norm" if "symbol_norm" in cols else "trading_symbol"
        con.execute(f"""INSERT INTO ev0
            SELECT DISTINCT {who}, {sym}, adsh, filed, CAST(available_at AT TIME ZONE 'UTC' AS TIMESTAMP),
                   'cover_page', 2, security_exchange_name
            FROM read_parquet('{cover.as_posix()}')
            WHERE {sym} IS NOT NULL AND {who} IS NOT NULL AND available_at IS NOT NULL
              AND coalesce(is_equity_like, true)""")
        receipt["cover_manifest_sha256"] = C.sha256_file(cover.parent / "manifest.json") \
            if (cover.parent / "manifest.json").exists() else None
    receipt["evidence_rows"] = dict(con.execute("SELECT source, count(*) FROM ev0 GROUP BY 1").fetchall())
    raws = [r[0] for r in con.execute("SELECT DISTINCT symbol_raw FROM ev0").fetchall()]
    con.execute("CREATE TABLE sym (symbol_raw VARCHAR, symbol VARCHAR)")
    con.executemany("INSERT INTO sym VALUES (?, ?)", [(r, s) for r in raws for s in split_symbols(r)])
    con.execute("""CREATE TABLE ev AS
        SELECT e.* EXCLUDE (symbol_raw), e.symbol_raw, s.symbol FROM ev0 e JOIN sym s USING (symbol_raw)""")
    con.execute("DROP TABLE ev0")
    mdir = wd / "evidence_symbol_map"
    mdir.mkdir(exist_ok=True)
    for y in [r[0] for r in con.execute("SELECT DISTINCT year(key_date) FROM ev ORDER BY 1").fetchall()]:
        S.map_symbols_asof(con, f"SELECT key_date, symbol FROM ev WHERE year(key_date) = {y}", _vendor_glob(),
                           mdir / f"map_{y}.parquet")
    n = C.copy_to_parquet(con, f"""
        SELECT e.cik, m.candidate_security_id AS security_id, e.symbol, e.symbol_raw, e.key_date, e.available_at,
               e.source, e.weight, e.accession, e.exchange, m.outcome, m.exact
        FROM ev e LEFT JOIN read_parquet('{(mdir / 'map_*.parquet').as_posix()}') m
          ON m.key_date = e.key_date AND m.symbol = e.symbol
        ORDER BY security_id, available_at""", wd / "evidence.parquet")
    receipt["evidence_mapped_rows"] = n
    receipt["outcomes"] = {f"{s}:{o}": int(k) for s, o, k in con.execute(f"""
        SELECT source, coalesce(outcome, 'none'), count(*) FROM read_parquet('{(wd / 'evidence.parquet').as_posix()}')
        GROUP BY 1, 2 ORDER BY 1, 2""").fetchall()}
    con.close()
    (C.build_root() / "_tmp" / "identity_v3_evidence.duckdb").unlink(missing_ok=True)
    C.write_json_atomic(wd / "evidence_receipt.json", receipt)
    return receipt


# ---------------------------------------------------------------- full name tier
def names() -> dict[str, Any]:
    """Rule ``finra-name-match-v1`` (identity_names) on every session of the line; the v1 build dropped the
    strict and backfill days, v3 keeps backfill days so the point-in-time tier outranks the snapshot tier."""
    root = C.build_root()
    wd = work_dir()
    con = C.connect(memory=MEMORY, threads=2, db_file="identity_v3_names.duckdb")
    con.create_function("stem_finra", N.stem_finra, ["VARCHAR"], "VARCHAR")
    con.create_function("stem_sec", N.stem_sec, ["VARCHAR"], "VARCHAR")
    names_src = (root / "security_master" / "finra_names.parquet").as_posix()
    sub = (C.FSDS_DIR / "sub" / "*.parquet").as_posix()
    forms = ", ".join(f"'{f}'" for f in N.PERIODIC)
    # the security master's dated FINRA rows are the stage-S rows joined to their raw issue names (the v1 name
    # build re-read the raw CSVs; same join, one issue name per (symbol, settlement date))
    con.execute(f"""CREATE TABLE rows AS
        SELECT security_id, settlement_date, dissemination_date, issue_name,
               stem_finra(issue_name) AS fstem, length(issue_name) >= 30 AS truncated
        FROM read_parquet('{names_src}') WHERE security_id IS NOT NULL AND issue_name IS NOT NULL""")
    con.execute(f"""CREATE TABLE secnames AS
        SELECT DISTINCT CAST(cik AS BIGINT) AS cik, stem_sec(name) AS sstem, filed
        FROM read_parquet('{sub}') WHERE form IN ({forms})""")
    con.execute("CREATE TABLE fst AS SELECT DISTINCT fstem, truncated FROM rows WHERE length(fstem) >= 4")
    con.execute("""CREATE TABLE pairs AS
        SELECT DISTINCT f.fstem, f.truncated, n.sstem FROM fst f JOIN (SELECT DISTINCT sstem FROM secnames) n ON n.sstem = f.fstem
        UNION
        SELECT DISTINCT f.fstem, f.truncated, n.sstem
        FROM (SELECT * FROM fst WHERE truncated AND length(fstem) >= 12) f
        JOIN (SELECT DISTINCT sstem, substr(sstem, 1, 12) AS k FROM secnames WHERE length(sstem) >= 12) n
          ON n.k = substr(f.fstem, 1, 12) AND starts_with(n.sstem, f.fstem)""")
    con.execute("""CREATE TABLE m AS
        SELECT r.security_id, r.settlement_date, r.dissemination_date, r.issue_name, n.cik
        FROM rows r JOIN pairs p ON p.fstem = r.fstem AND p.truncated = r.truncated
        JOIN secnames n ON n.sstem = p.sstem
        WHERE n.filed <= r.dissemination_date AND n.filed > r.dissemination_date - INTERVAL 450 DAY""")
    con.execute("""CREATE TABLE uniq AS
        SELECT security_id, settlement_date, dissemination_date, any_value(cik) AS cik, any_value(issue_name) AS issue_name
        FROM (SELECT DISTINCT security_id, settlement_date, dissemination_date, cik, issue_name FROM m)
        GROUP BY 1, 2, 3 HAVING count(DISTINCT cik) = 1""")
    cal = C.calendar_path().as_posix()
    prices = (root / "prices" / "*" / "*.parquet").as_posix()
    n = C.copy_to_parquet(con, f"""
        WITH s AS (
            SELECT u.*, lead(dissemination_date) OVER (PARTITION BY security_id ORDER BY settlement_date) AS next_dissem
            FROM uniq u
        )
        SELECT s.security_id, c.session_date, s.cik,
               CAST(s.dissemination_date AS TIMESTAMP) + INTERVAL 22 HOUR AS available_at, s.issue_name
        FROM s JOIN read_parquet('{cal}') c
          ON c.session_date > s.dissemination_date AND c.session_date <= s.dissemination_date + INTERVAL 45 DAY
         AND (s.next_dissem IS NULL OR c.session_date <= s.next_dissem)
        SEMI JOIN (SELECT security_id, session_date FROM read_parquet('{prices}', hive_partitioning = false)) px
          ON px.security_id = s.security_id AND px.session_date = c.session_date
        ORDER BY 1, 2""", wd / "name_days_full.parquet")
    con.close()
    (C.build_root() / "_tmp" / "identity_v3_names.duckdb").unlink(missing_ok=True)
    rec = {"rule": "finra-name-match-v1 (all sessions)", "name_link_days": n}
    C.write_json_atomic(wd / "names_receipt.json", rec)
    return rec


# ---------------------------------------------------------------- dated tier
def dated() -> dict[str, Any]:
    """Rule ``filing-symbol-evidence-v1`` step 3 over every line-session of the panel core (one year at a time)."""
    root = C.build_root()
    wd = work_dir()
    con = C.connect(memory=MEMORY, threads=1, db_file="identity_v3_dated.duckdb")
    core = (root / "_tmp" / "panel_core" / "*.parquet").as_posix()
    ev = (wd / "evidence.parquet").as_posix()
    # evidence per (line, cik) with running weights; one row per (line, cik, available_at). Sources are carried as a
    # bit mask (1 = cover_page, 2 = anything else) so the aggregate stays fixed-width; expanded to a list on output.
    con.execute(f"""CREATE TABLE x AS
        SELECT security_id, cik, available_at, CAST(sum(weight) AS INTEGER) AS w,
               CAST(bit_or(CASE WHEN source = 'cover_page' THEN 1 ELSE 2 END) AS TINYINT) AS srcm
        FROM read_parquet('{ev}') WHERE security_id IS NOT NULL
        GROUP BY 1, 2, 3""")
    con.execute("""CREATE TABLE e AS
        SELECT *, CAST(sum(w) OVER (PARTITION BY security_id, cik ORDER BY available_at ROWS UNBOUNDED PRECEDING)
                       AS INTEGER) AS cw
        FROM x""")
    con.execute("DROP TABLE x")
    # candidate intervals per (line, cik): evidence runs with gaps <= WINDOW_DAYS, each open WINDOW_DAYS after its end
    con.execute(f"""CREATE TABLE iv AS
        WITH g AS (
            SELECT *, CASE WHEN available_at - lag(available_at) OVER w > INTERVAL {WINDOW_DAYS} DAY THEN 1 ELSE 0 END AS brk
            FROM e WINDOW w AS (PARTITION BY security_id, cik ORDER BY available_at)
        ), r AS (SELECT *, sum(brk) OVER (PARTITION BY security_id, cik ORDER BY available_at ROWS UNBOUNDED PRECEDING) AS k FROM g)
        SELECT security_id, cik, min(available_at) AS t_from, max(available_at) + INTERVAL {WINDOW_DAYS} DAY AS t_to
        FROM r GROUP BY security_id, cik, k""")
    # resumable per year: a finished year is a part file keyed by the evidence + panel-core identity, so a guard stop
    # (low host commit) resumes at the next year instead of from the start
    parts = wd / "dated_parts"
    key = {"evidence_sha256": C.sha256_file(wd / "evidence.parquet"), "code": C.code_identity(*MODULES),
           "core": {f.name: C.file_identity(f)["bytes"] for f in sorted((root / "_tmp" / "panel_core").glob("*.parquet"))}}
    kpath = parts / "key.json"
    if not kpath.exists() or json.loads(kpath.read_text(encoding="utf-8")) != json.loads(json.dumps(key, default=str)):
        shutil.rmtree(parts, ignore_errors=True)
        parts.mkdir(parents=True)
        C.write_json_atomic(kpath, key)
    con.execute("""CREATE TABLE days (security_id BIGINT, session_date DATE, cik BIGINT, available_at TIMESTAMP,
                                      w_window DOUBLE, w_recent DOUBLE, n_candidates BIGINT, srcm TINYINT)""")
    years = [r[0] for r in con.execute(f"SELECT DISTINCT year(session_date) FROM read_parquet('{core}') ORDER BY 1").fetchall()]
    for y in years:
        if (parts / f"y{y}.parquet").exists():
            continue
        con.execute("DELETE FROM days")
        con.execute(f"""CREATE OR REPLACE TEMP TABLE ls AS
            SELECT DISTINCT security_id, session_date, CAST(session_date AS TIMESTAMP) + INTERVAL {C.MARK_HOUR_UTC} HOUR AS cutoff
            FROM read_parquet('{core}') WHERE year(session_date) = {y}""")
        con.execute("""CREATE OR REPLACE TEMP TABLE cand AS
            SELECT ls.security_id, ls.session_date, ls.cutoff, iv.cik
            FROM ls JOIN iv ON iv.security_id = ls.security_id AND ls.cutoff >= iv.t_from AND ls.cutoff < iv.t_to""")
        # cumulative weight at the cutoff, at cutoff - RECENT_DAYS and at cutoff - WINDOW_DAYS (ASOF on (line, cik))
        # one ASOF join per step (each materialised) keeps the working set small; e restricted to this year's pairs
        con.execute(f"""CREATE OR REPLACE TEMP TABLE ey AS
            SELECT e.* FROM e SEMI JOIN (SELECT DISTINCT security_id, cik FROM cand) k
              ON k.security_id = e.security_id AND k.cik = e.cik
            WHERE e.available_at <= TIMESTAMP '{y}-12-31 23:59:59'""")
        con.execute("""CREATE OR REPLACE TEMP TABLE sa AS
            SELECT c.*, ey.cw AS cw_now, ey.available_at AS t_last, ey.srcm
            FROM cand c ASOF LEFT JOIN ey ON ey.security_id = c.security_id AND ey.cik = c.cik AND c.cutoff >= ey.available_at""")
        con.execute(f"""CREATE OR REPLACE TEMP TABLE sb AS
            SELECT sa.*, ey.cw AS cw_recent FROM sa ASOF LEFT JOIN ey
              ON ey.security_id = sa.security_id AND ey.cik = sa.cik AND sa.cutoff - INTERVAL {RECENT_DAYS} DAY >= ey.available_at""")
        con.execute(f"""CREATE OR REPLACE TEMP TABLE sc AS
            SELECT sb.*, ey.cw AS cw_window FROM sb ASOF LEFT JOIN ey
              ON ey.security_id = sb.security_id AND ey.cik = sb.cik AND sb.cutoff - INTERVAL {WINDOW_DAYS} DAY >= ey.available_at""")
        con.execute("DROP TABLE sa; DROP TABLE sb")
        con.execute(f"""INSERT INTO days
            SELECT security_id, session_date, cik, t_last, w_window, w_recent, n_candidates, srcm FROM (
                SELECT security_id, session_date, cik, t_last, srcm,
                       coalesce(cw_now, 0) - coalesce(cw_window, 0) AS w_window,
                       coalesce(cw_now, 0) - coalesce(cw_recent, 0) AS w_recent,
                       count(*) OVER (PARTITION BY security_id, session_date) AS n_candidates,
                       row_number() OVER (PARTITION BY security_id, session_date
                                          ORDER BY coalesce(cw_now, 0) - coalesce(cw_recent, 0) DESC,
                                                   coalesce(cw_now, 0) - coalesce(cw_window, 0) DESC,
                                                   t_last DESC, cik) AS rk
                FROM sc WHERE cw_now IS NOT NULL)
            WHERE rk = 1 AND w_window >= {MIN_WEIGHT}""")
        k = C.copy_to_parquet(con, """
            SELECT security_id, session_date, cik, available_at, w_window, w_recent, n_candidates,
                   CASE srcm WHEN 1 THEN ['cover_page'] WHEN 2 THEN ['form345'] ELSE ['cover_page', 'form345'] END AS src
            FROM days ORDER BY security_id, session_date""", parts / f"y{y}.parquet")
        print(f"[identity_v3] dated {y}: {k} line-days", flush=True)
    con.execute("DROP TABLE days")
    pg = (parts / "y*.parquet").as_posix()
    n = C.copy_to_parquet(con, f"SELECT * FROM read_parquet('{pg}') ORDER BY security_id, session_date",
                          wd / "dated_days.parquet")
    rec = {"rule": DATED_RULE, "dated_line_days": n,
           "per_year": {str(y): int(k) for y, k in con.execute(
               f"SELECT year(session_date), count(*) FROM read_parquet('{pg}') GROUP BY 1 ORDER BY 1").fetchall()},
           "conflict_days": int(con.execute(
               f"SELECT count(*) FROM read_parquet('{pg}') WHERE n_candidates > 1").fetchone()[0])}
    con.close()
    (C.build_root() / "_tmp" / "identity_v3_dated.duckdb").unlink(missing_ok=True)
    C.write_json_atomic(wd / "dated_receipt.json", rec)
    return rec


# ---------------------------------------------------------------- link table v3
def table() -> dict[str, Any]:
    root = C.build_root()
    wd = work_dir()
    con = C.connect(memory=MEMORY, threads=1, db_file="identity_v3_table.duckdb")
    p = {"combined": (root / "identity" / "links_combined.parquet").as_posix(),
         "core": (root / "_tmp" / "panel_core" / "*.parquet").as_posix(),
         "member": (root / "_tmp" / "panel_member" / "*.parquet").as_posix(),
         "names": (wd / "name_days_full.parquet").as_posix(), "dated": (wd / "dated_days.parquet").as_posix(),
         "cal": C.calendar_path().as_posix(),
         "finra": (root / "security_master" / "finra_names.parquet").as_posix(),
         "listing": (root / "security_master" / "line_listing.parquet").as_posix()}
    receipt: dict[str, Any] = {"rule": RULE, "dated_rule": DATED_RULE}
    con.execute(f"""CREATE TABLE core AS
        SELECT session_date, security_id, ticker, adv63, is_operating, is_index FROM read_parquet('{p["core"]}')""")
    con.execute("""CREATE TABLE days (session_date DATE, security_id BIGINT, cik BIGINT, link_tier VARCHAR, basis VARCHAR,
                                      link_available_at TIMESTAMP, adv63 DOUBLE, ticker VARCHAR, dated_srcm TINYINT,
                                      dated_w DOUBLE, strict_cik BIGINT, backfill_cik BIGINT)""")
    for year in range(2017, 2027):
        con.execute(f"""
            INSERT INTO days
            WITH c AS (SELECT * FROM core WHERE year(session_date) = {year}),
            l AS (SELECT * FROM read_parquet('{p["combined"]}')
                  WHERE start <= DATE '{year}-12-31' AND end_incl >= DATE '{year}-01-01' AND tier <> 'name'),
            st AS (SELECT c.session_date, c.security_id, l.cik, l.tier, l.basis, l.available_at
                   FROM c JOIN l ON l.security_id = c.security_id AND c.session_date BETWEEN l.start AND l.end_incl),
            d AS (SELECT * FROM read_parquet('{p["dated"]}') WHERE year(session_date) = {year}),
            n AS (SELECT security_id, session_date, arg_min(cik, available_at) AS cik, min(available_at) AS available_at
                  FROM read_parquet('{p["names"]}') WHERE year(session_date) = {year} GROUP BY 1, 2),
            u AS (
                SELECT c.session_date, c.security_id, c.adv63, c.ticker,
                       s1.cik AS strict_cik, s1.tier AS strict_tier, s1.basis AS strict_basis, s1.available_at AS strict_av,
                       d.cik AS dated_cik, d.available_at AS dated_av,
                       CAST(CASE WHEN list_contains(d.src, 'cover_page') THEN 1 ELSE 0 END
                            + CASE WHEN list_contains(d.src, 'form345') THEN 2 ELSE 0 END AS TINYINT) AS dated_srcm,
                       d.w_window AS dated_w,
                       n.cik AS name_cik, n.available_at AS name_av,
                       b.cik AS backfill_cik, b.available_at AS backfill_av
                FROM c
                LEFT JOIN (SELECT * FROM st WHERE tier <> 'backfill') s1 USING (session_date, security_id)
                LEFT JOIN (SELECT * FROM st WHERE tier = 'backfill') b USING (session_date, security_id)
                LEFT JOIN d USING (session_date, security_id)
                LEFT JOIN n USING (session_date, security_id)
            )
            SELECT session_date, security_id,
                   coalesce(strict_cik, dated_cik, name_cik, backfill_cik) AS cik,
                   CASE WHEN strict_cik IS NOT NULL THEN 'strict' WHEN dated_cik IS NOT NULL THEN 'dated'
                        WHEN name_cik IS NOT NULL THEN 'name' ELSE 'backfill' END AS link_tier,
                   CASE WHEN strict_cik IS NOT NULL THEN strict_basis WHEN dated_cik IS NOT NULL THEN 'sec_filing_symbol'
                        WHEN name_cik IS NOT NULL THEN 'finra_name_match' ELSE 'snapshot_run_backfill' END AS basis,
                   CASE WHEN strict_cik IS NOT NULL THEN strict_av WHEN dated_cik IS NOT NULL THEN dated_av
                        WHEN name_cik IS NOT NULL THEN name_av ELSE backfill_av END AS link_available_at,
                   adv63, ticker, CASE WHEN strict_cik IS NULL AND dated_cik IS NOT NULL THEN dated_srcm END,
                   CASE WHEN strict_cik IS NULL AND dated_cik IS NOT NULL THEN dated_w END, strict_cik, backfill_cik
            FROM u WHERE coalesce(strict_cik, dated_cik, name_cik, backfill_cik) IS NOT NULL""")
    # agreement of the dated rule with the strict and snapshot tiers where both exist (precision evidence)
    receipt["dated_agreement"] = _agreement(con, p)
    receipt["link_days"] = con.execute("SELECT count(*), count(DISTINCT security_id), count(DISTINCT cik) FROM days").fetchone()
    receipt["ambiguous_line_days"] = con.execute(
        "SELECT count(*) FROM (SELECT session_date, security_id FROM days GROUP BY 1, 2 HAVING count(DISTINCT cik) > 1)"
    ).fetchone()[0]
    con.execute(f"""CREATE TABLE fr_cls AS
        SELECT security_id, dissemination_date, min(share_class) AS cls FROM read_parquet('{p["finra"]}')
        WHERE security_id IS NOT NULL AND share_class IS NOT NULL GROUP BY 1, 2""")
    con.execute(f"""CREATE TABLE ftype AS
        SELECT security_id, dissemination_date, finra_type FROM read_parquet('{p["finra"]}')
        WHERE finra_type IS NOT NULL""")
    # per-year day state (primary flag and class are per session, so a year split is exact), then islands per
    # year, then runs that continue across a year boundary are merged
    con.execute(f"""CREATE TABLE cal AS
        SELECT session_date, row_number() OVER (ORDER BY session_date) AS k FROM read_parquet('{p["cal"]}')""")
    con.execute("""CREATE TABLE days2 (session_date DATE, security_id BIGINT, cik BIGINT, link_tier VARCHAR, basis VARCHAR,
                                       link_available_at TIMESTAMP, adv63 DOUBLE, ticker VARCHAR, dated_srcm TINYINT,
                                       dated_w DOUBLE, strict_cik BIGINT, backfill_cik BIGINT, share_class VARCHAR,
                                       is_issuer_primary BOOLEAN, issuer_lines BIGINT, finra_type VARCHAR, linkprim VARCHAR)""")
    con.execute("""CREATE TABLE isl0 (security_id BIGINT, cik BIGINT, link_tier VARCHAR, basis VARCHAR,
                                      is_issuer_primary BOOLEAN, share_class VARCHAR, linkprim VARCHAR, k_from BIGINT,
                                      k_to BIGINT, valid_from DATE, valid_to DATE, sessions BIGINT,
                                      link_available_at TIMESTAMP, max_issuer_lines BIGINT, dated_srcm TINYINT,
                                      dated_max_weight DOUBLE)""")
    common = "('common', 'common_unverified', 'ADR', 'REIT', 'LP')"
    for year in [r[0] for r in con.execute("SELECT DISTINCT year(session_date) FROM days ORDER BY 1").fetchall()]:
        con.execute(f"""INSERT INTO days2
            WITH a AS (
                SELECT d.*,
                       coalesce(CASE WHEN regexp_matches(d.ticker, '^[A-Z]+\\.[A-Z]$') THEN right(d.ticker, 1) END, f.cls) AS share_class,
                       row_number() OVER (PARTITION BY d.cik, d.session_date ORDER BY d.adv63 DESC NULLS LAST, d.security_id) = 1
                           AS is_issuer_primary,
                       count(*) OVER (PARTITION BY d.cik, d.session_date) AS issuer_lines
                FROM (SELECT * FROM days WHERE year(session_date) = {year}) d
                ASOF LEFT JOIN fr_cls f ON d.security_id = f.security_id AND d.session_date > f.dissemination_date
            )
            SELECT a.*, t.finra_type,
                   CASE WHEN a.is_issuer_primary THEN 'P' WHEN coalesce(t.finra_type IN {common}, true) THEN 'J' ELSE 'N' END
            FROM a ASOF LEFT JOIN ftype t ON a.security_id = t.security_id AND a.session_date > t.dissemination_date""")
        con.execute(f"""INSERT INTO isl0
            WITH x AS (
                SELECT d.*, c.k,
                       c.k - row_number() OVER (PARTITION BY d.security_id, d.cik, d.link_tier, d.basis, d.is_issuer_primary,
                                                coalesce(d.share_class, ''), d.linkprim ORDER BY c.k) AS grp
                FROM (SELECT * FROM days2 WHERE year(session_date) = {year}) d JOIN cal c USING (session_date)
            )
            SELECT security_id, cik, link_tier, basis, is_issuer_primary, share_class, linkprim, min(k), max(k),
                   min(session_date), max(session_date), count(*), min(link_available_at), max(issuer_lines),
                   CAST(bit_or(coalesce(dated_srcm, 0)) AS TINYINT), max(dated_w)
            FROM x GROUP BY security_id, cik, link_tier, basis, is_issuer_primary, share_class, linkprim, grp""")
    con.execute("DROP TABLE days")
    con.execute(f"""CREATE TABLE lst AS
        SELECT security_id, listing_basis = 'sec_confirmed' AS listing_confirmed, cik AS listing_cik
        FROM read_parquet('{p["listing"]}')""")
    con.execute("""CREATE TABLE isl AS
        WITH o AS (
            SELECT *, CASE WHEN k_from = lag(k_to) OVER w + 1 THEN 0 ELSE 1 END AS brk
            FROM isl0 WINDOW w AS (PARTITION BY security_id, cik, link_tier, basis, is_issuer_primary,
                                   coalesce(share_class, ''), linkprim ORDER BY k_from)
        ), g AS (
            SELECT *, sum(brk) OVER (PARTITION BY security_id, cik, link_tier, basis, is_issuer_primary,
                                     coalesce(share_class, ''), linkprim ORDER BY k_from ROWS UNBOUNDED PRECEDING) AS grp
            FROM o
        )
        SELECT security_id, cik, link_tier, basis, is_issuer_primary, share_class, linkprim,
               min(valid_from) AS valid_from, max(valid_to) AS valid_to, sum(sessions) AS sessions,
               min(link_available_at) AS link_available_at, max(max_issuer_lines) AS max_issuer_lines,
               CAST(bit_or(dated_srcm) AS TINYINT) AS dated_srcm, max(dated_max_weight) AS dated_max_weight
        FROM g GROUP BY security_id, cik, link_tier, basis, is_issuer_primary, share_class, linkprim, grp""")
    con.execute(f"CREATE TABLE ever AS SELECT DISTINCT security_id FROM read_parquet('{p['member']}') WHERE member")
    dest = root / "identity" / "link_table_v3.parquet"
    receipt["link_table_rows"] = C.copy_to_parquet(con, """
        SELECT i.security_id, i.cik, i.valid_from, i.valid_to, i.sessions, i.link_tier, i.basis, i.is_issuer_primary,
               i.share_class, i.max_issuer_lines, i.link_available_at AS available_at,
               CASE WHEN i.link_tier = 'backfill' THEN CAST(i.valid_from AS TIMESTAMP) + INTERVAL 22 HOUR
                    ELSE i.link_available_at END AS evidence_at,
               e.security_id IS NOT NULL AS ever_member,
               CASE WHEN i.link_tier = 'strict' THEN 'LC'
                    WHEN i.link_tier = 'dated' AND ((i.dated_srcm & 1) = 1
                         OR coalesce(l.listing_confirmed AND l.listing_cik = i.cik, false) OR i.dated_max_weight >= 4) THEN 'LC'
                    ELSE 'LU' END AS linktype,
               i.linkprim,
               CASE WHEN i.link_tier = 'dated' THEN list_concat(CASE WHEN (i.dated_srcm & 1) = 1 THEN ['cover_page'] ELSE [] END,
                                                          CASE WHEN (i.dated_srcm & 2) = 2 THEN ['form345'] ELSE [] END)
                    END AS dated_sources,
               coalesce(l.listing_confirmed AND l.listing_cik = i.cik, false) AS listing_event_match
        FROM isl i LEFT JOIN ever e USING (security_id) LEFT JOIN lst l USING (security_id)
        ORDER BY i.security_id, i.valid_from""", dest)
    receipt["link_table"] = [list(map(lambda v: v if isinstance(v, (int, float, str)) or v is None else str(v), r))
                             for r in con.execute(f"""
        SELECT link_tier, count(*) AS rows, count(DISTINCT security_id) AS lines, count(DISTINCT cik) AS ciks,
               sum(sessions) AS line_sessions,
               count(*) FILTER (WHERE available_at > CAST(valid_from AS TIMESTAMP) + INTERVAL 22 HOUR) AS rows_known_after_start
        FROM read_parquet('{dest.as_posix()}') GROUP BY 1 ORDER BY 1""").fetchall()]
    receipt["linktype_linkprim"] = {f"{a}/{b}": int(n) for a, b, n in con.execute(f"""
        SELECT linktype, linkprim, count(*) FROM read_parquet('{dest.as_posix()}') GROUP BY 1, 2 ORDER BY 1, 2""").fetchall()}
    receipt["exports"] = export_bridges(con, dest)
    receipt["audit"] = audit(con)
    con.close()
    (root / "_tmp" / "identity_v3_table.duckdb").unlink(missing_ok=True)
    inputs = {k: C.output_hashes(root / "identity", n) for k, n in (("links_combined", "links_combined.parquet"),)}
    inputs["work"] = C.output_hashes(wd, "dated_days.parquet") | C.output_hashes(wd, "name_days_full.parquet") \
        | C.output_hashes(wd, "evidence.parquet")
    ims = {k: C.sha256_file(v) for k, v in (
        ("identity_manifest_sha256", root / "identity" / "manifest.json"),
        ("identity_backfill_manifest_sha256", root / "identity" / "backfill_manifest.json"),
        ("insider_manifest_sha256", root / "insider" / "manifest.json"),
        ("identity_cover_manifest_sha256", root / "identity_cover" / "manifest.json"),
        ("security_master_manifest_sha256", root / "security_master" / "manifest.json"),
        ("listing_events_manifest_sha256", root / "security_master" / "listing_events_manifest.json"),
        ("short_interest_manifest_sha256", root / "short_interest" / "manifest.json"),
        ("prices_manifest_sha256", root / "prices" / "manifest.json"),
        ("panel_manifest_sha256", root / "panel" / "manifest.json")) if v.exists()}
    C.write_json_atomic(root / "identity" / "link_table_v3_manifest.json", {
        "schema": "atx.alpha-panel.identity-link-table/v3", "status": "complete", "rule": RULE,
        "code": C.code_identity(*MODULES), "files": C.output_hashes(root / "identity", "link_table_v3.parquet"),
        "inputs": inputs, "input_manifests_sha256": ims,
        "panel_core_files": {f.name: C.file_identity(f) for f in sorted((root / "_tmp" / "panel_core").glob("*.parquet"))},
        "rule_text": __doc__, "receipt": receipt})
    return receipt


def _agreement(con, p: dict[str, str]) -> dict[str, Any]:
    """Dated-rule CIK vs the strict and snapshot CIKs on the line-days where both exist."""
    d = p["dated"]
    rows = con.execute(f"""
        WITH x AS (SELECT a.session_date, a.strict_cik, a.backfill_cik, d.cik AS dated_cik
                   FROM (SELECT session_date, security_id, strict_cik, backfill_cik FROM days) a
                   JOIN read_parquet('{d}') d USING (session_date, security_id))
        SELECT year(session_date),
               count(*) FILTER (WHERE strict_cik IS NOT NULL), count(*) FILTER (WHERE strict_cik = dated_cik),
               count(*) FILTER (WHERE backfill_cik IS NOT NULL), count(*) FILTER (WHERE backfill_cik = dated_cik)
        FROM x GROUP BY 1 ORDER BY 1""").fetchall()
    out = {}
    for y, ns, ks, nb, kb in rows:
        out[str(y)] = {"strict_days": ns, "agree_strict": round(ks / ns, 5) if ns else None,
                       "backfill_days": nb, "agree_backfill": round(kb / nb, 5) if nb else None}
    return out


VARIANTS = {"strict": ("strict",), "pit": ("strict", "dated", "name"), "all": ("strict", "dated", "name", "backfill")}


def export_bridges(con, table: Path) -> dict[str, Any]:
    out: dict[str, Any] = {}
    root = C.build_root()
    for name, tiers in VARIANTS.items():
        d = root / "export" / f"identity-bridge-v3-{name}"
        d.mkdir(parents=True, exist_ok=True)
        lst = ", ".join(f"'{t}'" for t in tiers)
        avail = "evidence_at" if name == "all" else "available_at"
        C.copy_to_parquet(con, f"""
            SELECT security_id AS sr_id, cik, valid_from AS start, valid_to AS end_incl, {avail} AS available_at,
                   CASE WHEN is_issuer_primary THEN 'P' ELSE 'J' END AS "primary",
                   link_tier AS tier, basis, coalesce(share_class, 'common') AS class_status,
                   link_tier, share_class, available_at AS knowledge_at, linktype, linkprim
            FROM read_parquet('{table.as_posix()}') WHERE link_tier IN ({lst})
            ORDER BY sr_id, start""", d / "links.parquet")
        stats = con.execute(f"""
            SELECT count(*), count(DISTINCT sr_id), count(DISTINCT cik), min(start), max(end_incl),
                   count(*) FILTER (WHERE available_at > CAST(start AS TIMESTAMP) + INTERVAL 22 HOUR)
            FROM read_parquet('{(d / "links.parquet").as_posix()}')""").fetchone()
        manifest = {
            "schema": "atx.identity-bridge/v1", "status": "complete", "rule": f"{RULE}/{name}",
            "rehearsal_identity": True, "instrument_namespace": "spiderrock.securityID", "mark_utc": "22:00:00",
            "tiers_kept": list(tiers),
            "available_at_basis": ("evidence_at for backfill rows (knowledge_at = 2026-09-20 snapshot: survivorship-"
                                   "biased, not point in time); the link's own clock for strict, dated and name rows")
                                  if name == "all" else "the link's own knowledge clock (point in time)",
            "primary_rule": "P = the issuer's linked line with the highest prior-63-session dollar volume that session "
                            "(ties: smallest security_id); intervals split where the flag changes",
            "extra_columns": {"linktype": "CCM-style LC (researched) / LU (unresearched)",
                              "linkprim": "CCM-style P primary / J other common class / N non-common line"},
            "source": {"scope_complete": False, "link_table": C.output_hashes(table.parent, table.name)},
            "counts": {"rows": stats[0], "lines": stats[1], "ciks": stats[2], "min_start": str(stats[3]),
                       "max_end_incl": str(stats[4]), "rows_available_after_start_mark": stats[5]},
            "files": C.output_hashes(d, "links.parquet"),
            "code": C.code_identity(*MODULES),
        }
        C.write_json_atomic(d / "manifest.json", manifest)
        out[name] = manifest["counts"]
    return out


def audit(con) -> dict[str, Any]:
    """Per year: member_equity cells (v2 panel) by v3 tier; dead-line coverage; ambiguous line-days."""
    root = C.build_root()
    panel = (root / "panel" / "year=*" / "*.parquet").as_posix()
    con.execute(f"""CREATE OR REPLACE TABLE cells AS
        SELECT p.session_date, p.security_id, d.link_tier
        FROM (SELECT session_date, security_id FROM read_parquet('{panel}', hive_partitioning = false)
              WHERE member_equity) p
        LEFT JOIN days2 d USING (session_date, security_id)""")
    con.execute("CREATE OR REPLACE TABLE lastday AS SELECT security_id, max(session_date) AS last_session FROM core GROUP BY 1")
    rows = con.execute(f"""
        SELECT year(c.session_date), l.last_session < DATE '{DEAD_BEFORE}' AS dead, count(*),
               count(*) FILTER (WHERE link_tier = 'strict'), count(*) FILTER (WHERE link_tier = 'dated'),
               count(*) FILTER (WHERE link_tier = 'name'), count(*) FILTER (WHERE link_tier = 'backfill'),
               count(*) FILTER (WHERE link_tier IS NULL)
        FROM cells c LEFT JOIN lastday l USING (security_id) GROUP BY ALL ORDER BY ALL""").fetchall()
    per_year: dict[str, Any] = {}
    for y, dead, n, s, dd, nm, b, u in rows:
        slot = per_year.setdefault(str(y), {"cells": 0, "strict": 0, "dated": 0, "name": 0, "backfill": 0,
                                            "unlinked": 0})
        for k, v in (("cells", n), ("strict", s), ("dated", dd), ("name", nm), ("backfill", b), ("unlinked", u)):
            slot[k] += v
        if dead:
            slot["dead_line_cells"] = n
            slot["dead_line_cells_linked_pit"] = s + dd + nm
    for slot in per_year.values():
        c = max(slot["cells"], 1)
        slot["share"] = {"strict": round(slot["strict"] / c, 4),
                         "pit_strict_dated_name": round((slot["strict"] + slot["dated"] + slot["name"]) / c, 4),
                         "any_tier": round((c - slot["unlinked"]) / c, 4)}
    amb = con.execute("""SELECT count(*) FROM (SELECT session_date, security_id FROM days2
                         GROUP BY 1, 2 HAVING count(DISTINCT cik) > 1)""").fetchone()[0]
    result = {"basis": "member_equity cells of the v2 panel stage (panel/year=*)", "per_year": per_year,
              "ambiguous_line_days": amb}
    C.write_json_atomic(root / "identity" / "audit_v3.json", result)
    return result


# ---------------------------------------------------------------- CRSP-style codes
EXCHCD = {"XNYS": 1, "XASE": 2, "XNAS": 3, "ARCX": 4, "BATS": 5, "IEXG": 6, "OTC": 0}


def shrcd_sql() -> str:
    """CRSP-style share code from the v2 panel's point-in-time security flags (row alias ``p``)."""
    return """CASE
        WHEN p.is_etf THEN 73
        WHEN p.is_adr OR p.is_adr_likely THEN 31
        WHEN p.security_type = 'fund' THEN 14
        WHEN p.is_lp THEN 71
        WHEN p.is_reit AND p.security_type IN ('common', 'common_unverified', 'REIT') THEN 18
        WHEN p.security_type IN ('common', 'common_unverified') AND p.is_fpi THEN 12
        WHEN p.security_type IN ('common', 'common_unverified') THEN 11
        END"""


def codes() -> dict[str, Any]:
    root = C.build_root()
    con = C.connect(memory=MEMORY, threads=2, db_file="identity_v3_codes.duckdb")
    panel = (root / "panel" / "year=*" / "*.parquet").as_posix()
    cal = C.calendar_path().as_posix()
    ex = " ".join(f"WHEN '{k}' THEN {v}" for k, v in EXCHCD.items())
    con.execute(f"""CREATE TABLE d AS
        SELECT p.session_date, p.security_id, p.exchange, CASE p.exchange {ex} END AS exchcd,
               {shrcd_sql()} AS shrcd, p.security_type
        FROM read_parquet('{panel}', hive_partitioning = false) p""")
    dest = root / "security_master" / "share_exchange_history.parquet"
    n = C.copy_to_parquet(con, f"""
        WITH c AS (SELECT session_date, row_number() OVER (ORDER BY session_date) AS k FROM read_parquet('{cal}')),
        x AS (SELECT d.*, c.k - row_number() OVER (PARTITION BY security_id, exchange, exchcd, shrcd, security_type
                                                   ORDER BY c.k) AS grp
              FROM d JOIN c USING (session_date))
        SELECT security_id, min(session_date) AS valid_from, max(session_date) AS valid_to, count(*) AS sessions,
               exchcd, exchange, shrcd, security_type,
               CAST(min(session_date) AS TIMESTAMP) - INTERVAL 2 HOUR AS available_at
        FROM x GROUP BY security_id, exchange, exchcd, shrcd, security_type, grp
        ORDER BY security_id, valid_from""", dest)
    stats = dict(con.execute(f"""SELECT coalesce(CAST(shrcd AS VARCHAR), 'null'), sum(sessions)
                                 FROM read_parquet('{dest.as_posix()}') GROUP BY 1 ORDER BY 1""").fetchall())
    con.close()
    (root / "_tmp" / "identity_v3_codes.duckdb").unlink(missing_ok=True)
    rec = {"rows": n, "line_sessions_by_shrcd": {k: int(v) for k, v in stats.items()},
           "available_at_rule": "the panel's flags on session d use data visible before d-1 22:00 UTC; a run is "
                                "stamped at its first session's 22:00 UTC of the previous day (first session - 2 h)"}
    C.write_json_atomic(root / "security_master" / "share_exchange_history_manifest.json", {
        "schema": "atx.alpha-panel.share-exchange-history/v1", "status": "complete", "rule_text": __doc__,
        "code": C.code_identity(*MODULES), "files": C.output_hashes(root / "security_master", "share_exchange_history.parquet"),
        "input_manifests_sha256": {"panel_manifest_sha256": C.sha256_file(root / "panel" / "manifest.json")}
        if (root / "panel" / "manifest.json").exists() else {},
        "exchcd": EXCHCD, "shrcd_rule": shrcd_sql(), "receipt": rec})
    return rec


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=("evidence", "names", "dated", "table", "codes", "all"))
    args = ap.parse_args(argv)
    steps = {"evidence": evidence, "names": names, "dated": dated, "table": table, "codes": codes}
    for name in (("evidence", "names", "dated", "table") if args.cmd == "all" else (args.cmd,)):
        t0 = time.perf_counter()
        rec = steps[name]()
        print(name, round(time.perf_counter() - t0, 1), "s", json.dumps(rec, default=str)[:6000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
