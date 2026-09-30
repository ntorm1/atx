"""S2.3 ``security_master/cusip_history.parquet``: dated CUSIP <-> security_id ranges, derived ISINs.

Evidence (each row keeps its own basis and clock):

* ``ftd_symbol``: SEC fails-to-deliver rows mapped to a line by the stage-``ftd`` symbol rule
  (``ftd-ticker-asof-settlement-v1``). Point in time: the FTD file's ``available_at``.
* ``openfigi_ticker``: OpenFIGI maps the CUSIP to a US composite ticker (``figi_lei`` landing, 2026-09-29
  snapshot); the ticker is mapped to the vendor line carrying it on the snapshot's last vendor session, and the
  range covers the CUSIP's 13F span. Not point in time: ``available_at`` = the OpenFIGI fetch time
  (``vintage_risk = 'snapshot_non_pit'``).
* ``openfigi_isin_fragment``: a 13F CUSIP field holding characters 3-11 of a non-US ISIN (Chubb reported as
  ``004432874`` = ISIN CH0044328745); the ISIN is reconstructed (candidate country + check digit), confirmed by
  OpenFIGI, and mapped like ``openfigi_ticker``. ``cusip_kind = 'isin_fragment'``.

Rule ``cusip-history-v1``:

1. Observations per (cusip, security_id) are grouped into runs: a gap longer than ``RUN_GAP_DAYS`` starts a new
   run. ``obs_from`` / ``obs_to`` are the first / last observation of the run.
2. Extended range: ``valid_from`` = the day after the previous run of the same line (any CUSIP) ends, else the
   line's first vendor session; ``valid_to`` = the day before the next run of the same line starts, else the
   line's last vendor session; both clipped to ``[obs_from - EXTEND_DAYS, obs_to + EXTEND_DAYS]``.
3. A CUSIP whose runs point to different lines at the same date: every run is kept (``n_lines_overlap`` > 1);
   :func:`resolve_sql` picks the run whose observations are nearest to the date, FTD before OpenFIGI.
4. ISIN: ``US`` (``CA`` when the linked issuer is incorporated in a Canadian province, SEC codes A0-B0) + the
   9-character CUSIP + the ISIN check digit (Luhn over the digits of the letters-as-numbers string). A CINS
   (first character a letter) has no derivable ISIN. ``cusip_check_ok`` tests the CUSIP's own check digit.

The 13F coverage measure (``measure``) reproduces the thirteenf stage's effective-holding value basis (unit
repair from ``filing_checks.parquet``, rule ``13f-row-sanity-v1`` price outliers excluded) and reports, per
quarter, the share of SH value (no put/call) that maps to a line under (a) the stage's PIT FTD map, (b) this
history with FTD evidence only, (c) all evidence; ``equity_only`` variants drop debt CUSIPs (letter in the issue
number) and the value of SH rows titled PUT / CALL (options some filers mis-type as SH; per row, so one filer's
mis-typed row does not drop the CUSIP's whole value).

Usage (under the memory guard)::

    python -m atx_db.alpha_panel.cusip_history collect     # 13F per-quarter CUSIP values + FTD runs (_tmp)
    python -m atx_db.alpha_panel.cusip_history build       # cusip_history.parquet (+ OpenFIGI when landed)
    python -m atx_db.alpha_panel.cusip_history measure     # 13F SH value coverage per quarter -> manifest
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import time
from pathlib import Path
from typing import Any

from . import common as C

STAGE = "security_master"
SCHEMA = "atx.alpha-panel.cusip-history/v1"
RULE = "cusip-history-v1"
RUN_GAP_DAYS = 400
COLLECT_MEMORY = "180MB"
EXTEND_DAYS = 730
MEMORY = "250MB"
MODULES = ("cusip_history", "common")
CANADA_STATE_CODES = tuple(f"A{i}" for i in range(10)) + ("B0",)

_ALNUM = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"


# ---------------------------------------------------------------- pure identifier rules
def _char_value(ch: str) -> int:
    """CUSIP character value: digits 0-9, letters A=10 .. Z=35, ``*`` 36, ``@`` 37, ``#`` 38."""
    if ch in "*@#":
        return 36 + "*@#".index(ch)
    v = _ALNUM.find(ch)
    if v < 0:
        raise ValueError(f"invalid CUSIP character {ch!r}")
    return v


def cusip_check_digit(base8: str) -> str:
    """Check digit of the first 8 CUSIP characters (modulus 10 double-add-double)."""
    if len(base8) != 8:
        raise ValueError("CUSIP base must have 8 characters")
    total = 0
    for i, ch in enumerate(base8.upper()):
        v = _char_value(ch)
        if i % 2 == 1:
            v *= 2
        total += v // 10 + v % 10
    return str((10 - total % 10) % 10)


def cusip_valid(cusip: str | None) -> bool:
    if not cusip or len(cusip) != 9:
        return False
    try:
        return cusip_check_digit(cusip[:8]) == cusip[8]
    except ValueError:
        return False


def isin_check_digit(base11: str) -> str:
    """ISIN check digit: letters -> numbers (A=10 .. Z=35), then Luhn over the digit string (the rightmost digit
    of the base is doubled because the check digit is appended to its right)."""
    if len(base11) != 11:
        raise ValueError("ISIN base must have 11 characters")
    digits = "".join(str(_ALNUM.index(ch)) for ch in base11.upper())
    total = 0
    for i, ch in enumerate(reversed(digits)):
        v = int(ch)
        if i % 2 == 0:
            v *= 2
        total += v // 10 + v % 10
    return str((10 - total % 10) % 10)


def isin_valid(isin: str | None) -> bool:
    if not isin or len(isin) != 12 or not isin[:2].isalpha() or not isin[:2].isupper():
        return False
    try:
        return isin_check_digit(isin[:11]) == isin[11]
    except ValueError:
        return False


def isin_from_cusip(cusip: str | None, country: str = "US") -> str | None:
    """``US`` / ``CA`` + CUSIP + check digit; None for a CINS (letter first) or a malformed CUSIP."""
    if not cusip or len(cusip) != 9 or not cusip[0].isdigit() or any(c not in _ALNUM for c in cusip.upper()):
        return None
    base = country + cusip.upper()
    return base + isin_check_digit(base)


def issue_kind(cusip: str | None) -> str:
    """Coarse CUSIP issue class from the issue number (characters 7-8): a letter marks fixed income
    (``debt``); numeric issue numbers are equity-like (``equity``); a failed check digit -> ``unverified`` (not a
    CUSIP: often an ISIN fragment); malformed -> ``invalid``."""
    if not cusip or len(cusip) != 9 or any(c not in _ALNUM for c in cusip.upper()):
        return "invalid"
    if not cusip_valid(cusip):
        return "unverified"
    issue = cusip[6:8].upper()
    return "debt" if any(c.isalpha() for c in issue) else "equity"


def isin_fragment_candidates(nsin: str, countries: tuple[str, ...]) -> list[str]:
    """ISINs whose characters 3-11 equal ``nsin`` (a 9-character national number) for each country code."""
    if len(nsin) != 9 or any(c not in _ALNUM for c in nsin.upper()):
        return []
    return [cc + nsin.upper() + isin_check_digit(cc + nsin.upper()) for cc in countries]


#: Home countries of US-listed foreign issuers whose ISIN fragments appear in 13F CUSIP fields, most common first.
FRAGMENT_COUNTRIES = ("IE", "CH", "NL", "GB", "JE", "BM", "KY", "LR", "IL", "LU", "PA", "CW", "BS", "VG", "MH",
                      "SG", "GG", "DE", "FR", "SE", "DK", "AU", "JP", "HK", "CN", "BE", "IT", "ES", "FI", "NO",
                      "AT", "CY", "GR", "MX", "BR", "AR", "CL", "ZA", "IN", "TW", "KR", "PR", "UY", "IM", "GI",
                      "MU", "MT", "TC", "AN")


# ---------------------------------------------------------------- paths
def work_dir() -> Path:
    p = C.build_root() / "_tmp" / "identity_v3"
    p.mkdir(parents=True, exist_ok=True)
    return p


def _p(path: Path) -> str:
    return path.as_posix()


def thirteenf_dir() -> Path:
    return C.build_root() / "thirteenf"


# ---------------------------------------------------------------- collect
def collect(first_q: str = "2013-01-01") -> dict[str, Any]:
    """Per (period_q, cusip): 13F SH value on the thirteenf stage's final-version basis; FTD observation runs."""
    from . import thirteenf as T

    t0 = time.perf_counter()
    wd = work_dir()
    con = C.connect(memory=COLLECT_MEMORY, threads=1, db_file="identity_v3_collect.duckdb")
    td = thirteenf_dir()
    fl = _p(td / "filings.parquet")
    checks = _p(td / "filing_checks.parquet")
    hglob = _p(td / "parts" / "source=*" / "holdings.parquet")
    con.execute(f"CREATE VIEW fl AS SELECT * FROM read_parquet('{fl}')")
    periods = [r[0] for r in con.execute(
        f"SELECT DISTINCT period_q FROM fl WHERE period_q BETWEEN DATE '{first_q}' AND current_date ORDER BY 1").fetchall()]
    parts = wd / "thirteenf_cusip_q_parts"
    parts.mkdir(parents=True, exist_ok=True)
    receipt: dict[str, Any] = {"periods": len(periods)}
    for p in periods:
        dest = parts / f"q_{p}.parquet"
        if dest.exists():
            continue
        con.execute(f"""CREATE OR REPLACE TABLE fp AS
            SELECT accession, source_period, filer_cik FROM fl WHERE period_q = DATE '{p}'""")
        con.execute(f"""CREATE OR REPLACE TABLE e AS
            SELECT accession FROM ({T._effective_sql(None)}) WHERE period_q = DATE '{p}'""")
        con.execute(f"""CREATE OR REPLACE TABLE h AS
            SELECT h.accession, fp.filer_cik, h.cusip, h.shares, h.value_usd, h.nameofissuer, h.titleofclass,
                   (h.sshprnamt_type = 'SH' AND h.put_call IS NULL AND h.shares > 0) AS sh_row,
                   (h.sshprnamt_type = 'SH' AND h.put_call IS NULL) AS sh_non_option,
                   h.accession IN (SELECT accession FROM e) AS effective
            FROM read_parquet('{hglob}', hive_partitioning = false) h JOIN fp USING (accession, source_period)
            WHERE h.period_q = DATE '{p}'""")
        con.execute(f"""CREATE OR REPLACE TABLE cons AS
            SELECT cusip, median(value_usd / shares) AS px FROM h WHERE sh_row AND value_usd > 0
            GROUP BY 1 HAVING count(*) >= {T.CONSENSUS_MIN_ROWS}""")
        C.copy_to_parquet(con, f"""
            WITH u AS (SELECT accession, unit_factor FROM read_parquet('{checks}') WHERE period_q = DATE '{p}'),
            x AS (
                SELECT h.*, h.value_usd * coalesce(u.unit_factor, 1) AS v,
                       coalesce(h.sh_row AND h.value_usd > 0 AND c.px IS NOT NULL
                                AND ((h.value_usd * coalesce(u.unit_factor, 1) / h.shares) / c.px > {T.PRICE_OUTLIER_X}
                                     OR (h.value_usd * coalesce(u.unit_factor, 1) / h.shares) / c.px < 1.0 / {T.PRICE_OUTLIER_X}),
                                false)
                       OR coalesce(h.value_usd * coalesce(u.unit_factor, 1) > {T.IMPLAUSIBLE_VALUE_USD}, false)
                       OR coalesce(h.sh_row AND h.value_usd * coalesce(u.unit_factor, 1) / h.shares > {T.IMPLAUSIBLE_PRICE_USD}, false)
                         AS outlier
                FROM h LEFT JOIN cons c USING (cusip) LEFT JOIN u USING (accession)
                WHERE h.effective
            )
            SELECT DATE '{p}' AS period_q, cusip,
                   sum(v) FILTER (WHERE sh_non_option AND NOT outlier) AS value_sh,
                   sum(v) FILTER (WHERE sh_non_option) AS value_sh_incl_outliers,
                   sum(v) AS value_all,
                   count(*) FILTER (WHERE sh_non_option AND NOT outlier) AS rows_sh,
                   count(DISTINCT filer_cik) AS filers,
                   min(nameofissuer) AS name, min(upper(titleofclass)) AS title,
                   bool_or(upper(trim(titleofclass)) IN ('PUT', 'CALL', 'PUTS', 'CALLS')) AS option_title,
                   -- SH rows whose title says PUT / CALL (options mis-typed SH): per row, not per CUSIP
                   sum(v) FILTER (WHERE sh_non_option AND NOT outlier
                                  AND upper(trim(titleofclass)) IN ('PUT', 'CALL', 'PUTS', 'CALLS')) AS value_sh_option_title
            FROM x GROUP BY 2""", dest)
    C.copy_to_parquet(con, f"SELECT * FROM read_parquet('{_p(parts / 'q_*.parquet')}') ORDER BY period_q, cusip",
                      wd / "thirteenf_cusip_q.parquet")
    receipt["thirteenf_cusip_q_rows"] = con.execute(
        f"SELECT count(*), count(DISTINCT cusip) FROM read_parquet('{_p(wd / 'thirteenf_cusip_q.parquet')}')").fetchone()
    # FTD observation runs per (cusip, security_id): month buckets first (min/max settlement date per month), so the
    # gap windows run over ~0.4M rows instead of every settlement row
    ftd = _p(C.build_root() / "ftd" / "year=*" / "ftd.parquet")
    con.execute(f"""CREATE OR REPLACE TABLE om AS
        SELECT cusip, security_id, date_trunc('month', settlement_date) AS m, min(settlement_date) AS d0,
               max(settlement_date) AS d1, count(*) AS n, min(available_at) AS available_at,
               arg_max(symbol, settlement_date) AS symbol
        FROM read_parquet('{ftd}', hive_partitioning = false)
        WHERE security_id IS NOT NULL AND cusip IS NOT NULL AND length(cusip) = 9
        GROUP BY 1, 2, 3""")
    con.execute(f"""CREATE OR REPLACE TABLE fdesc AS
        SELECT cusip, security_id, arg_max(description, settlement_date) AS description
        FROM read_parquet('{ftd}', hive_partitioning = false)
        WHERE security_id IS NOT NULL AND cusip IS NOT NULL AND length(cusip) = 9 GROUP BY 1, 2""")
    receipt["ftd_runs"] = C.copy_to_parquet(con, f"""
        WITH g AS (SELECT *, CASE WHEN date_diff('day', lag(d1) OVER w, d0) > {RUN_GAP_DAYS} THEN 1 ELSE 0 END AS brk
                   FROM om WINDOW w AS (PARTITION BY cusip, security_id ORDER BY m)),
        r AS (SELECT *, sum(brk) OVER (PARTITION BY cusip, security_id ORDER BY m ROWS UNBOUNDED PRECEDING) AS run FROM g)
        SELECT r.cusip, r.security_id, r.run, min(r.d0) AS obs_from, max(r.d1) AS obs_to, sum(r.n) AS n_obs,
               min(r.available_at) AS available_at, arg_min(r.available_at, r.d0) AS first_available_at,
               arg_max(r.symbol, r.d1) AS symbol, any_value(f.description) AS description
        FROM r LEFT JOIN fdesc f USING (cusip, security_id)
        GROUP BY r.cusip, r.security_id, r.run ORDER BY r.cusip, obs_from""", wd / "ftd_cusip_runs.parquet")
    receipt["seconds"] = round(time.perf_counter() - t0, 1)
    con.close()
    (C.build_root() / "_tmp" / "identity_v3_collect.duckdb").unlink(missing_ok=True)
    C.write_json_atomic(wd / "collect_receipt.json", receipt)
    return receipt


# ---------------------------------------------------------------- build
def _openfigi_path() -> Path:
    return C.build_root() / STAGE / "figi.parquet"


def resolve_sql(history: str, keys: str, date_col: str) -> str:
    """As-of resolution: ``keys`` (a relation with ``cusip`` and ``date_col``) -> the history run whose extended
    range contains the date, ties broken by the nearest observation, then FTD before OpenFIGI, then line id."""
    return f"""
        SELECT * EXCLUDE (rk) FROM (
            SELECT k.*, h.security_id, h.basis AS map_basis, h.available_at AS map_available_at,
                   row_number() OVER (PARTITION BY k.cusip, k.{date_col} ORDER BY
                       CASE WHEN k.{date_col} BETWEEN h.obs_from AND h.obs_to THEN 0
                            ELSE least(abs(date_diff('day', h.obs_from, k.{date_col})),
                                       abs(date_diff('day', h.obs_to, k.{date_col}))) END,
                       CASE WHEN h.basis = 'ftd_symbol' THEN 0 ELSE 1 END, h.security_id) AS rk
            FROM {keys} k JOIN {history} h
              ON h.cusip = k.cusip AND k.{date_col} BETWEEN h.valid_from AND h.valid_to
        ) WHERE rk = 1"""


def nport_parts() -> list[Path]:
    return sorted((C.build_root() / "nport" / "parts").glob("quarter=*/holdings.parquet"))


def _nport_evidence(con, wd: Path) -> int:
    """Optional source (lane OWN's ``nport`` stage, when published): the fund-reported ticker of each N-PORT
    equity holding mapped to the vendor line as of the report date (``nport_ticker``, point in time at the
    filing's ``available_at``)."""
    from . import shortflow_common as S

    parts = nport_parts()
    if not parts:
        return 0
    glob = (C.build_root() / "nport" / "parts" / "quarter=*" / "holdings.parquet").as_posix()
    con.execute(f"""CREATE TABLE np AS
        SELECT cusip, report_date AS key_date,
               regexp_replace(upper(trim(ticker)), '\\s+(US|UN|UW|UQ|UA|UR|UP|UV)(\\s+EQUITY)?$', '') AS symbol,
               min(CAST(available_at AS TIMESTAMP)) AS available_at, count(*) AS n
        FROM read_parquet('{glob}', hive_partitioning = false)
        WHERE cusip IS NOT NULL AND length(cusip) = 9 AND ticker IS NOT NULL AND report_date IS NOT NULL
        GROUP BY 1, 2, 3""")
    mapped = wd / "nport_ticker_map.parquet"
    S.map_symbols_asof(con, "SELECT key_date, symbol FROM np", _vendor_glob(), mapped)
    return con.execute(f"""INSERT INTO ev
        WITH o AS (
            SELECT np.cusip, m.candidate_security_id AS security_id, np.key_date AS d, min(np.available_at) AS available_at,
                   any_value(np.symbol) AS symbol
            FROM np JOIN read_parquet('{mapped.as_posix()}') m ON m.key_date = np.key_date AND m.symbol = np.symbol
            WHERE m.candidate_security_id IS NOT NULL GROUP BY 1, 2, 3
        ),
        g AS (SELECT *, CASE WHEN date_diff('day', lag(d) OVER w, d) > {RUN_GAP_DAYS} THEN 1 ELSE 0 END AS brk
              FROM o WINDOW w AS (PARTITION BY cusip, security_id ORDER BY d)),
        r AS (SELECT *, sum(brk) OVER (PARTITION BY cusip, security_id ORDER BY d ROWS UNBOUNDED PRECEDING) AS run FROM g)
        SELECT cusip, 'cusip', security_id, min(d), max(d), count(*), min(available_at), 'nport_ticker', false,
               arg_max(symbol, d), NULL
        FROM r GROUP BY cusip, security_id, run""").fetchone()[0]


ALIAS_MIN_FILERS = 5
ALIAS_MIN_STEM = 4


def _name_alias(con, q: str) -> int:
    """Rule ``thirteenf-name-alias-v1``: a 13F CUSIP field that is not a CUSIP (failed check digit: an ISIN
    fragment, a SEDOL or a filer's own code) and that no evidence maps takes the line of the unique mapped CUSIP
    whose 13F issuer-name stem (``identity_names.stem_finra``: cut at the class word, corporate-form words dropped)
    equals its own in the same quarter, among CUSIPs held by >= ``ALIAS_MIN_FILERS`` filers that quarter. Valid
    CUSIPs are never aliased (another class of the same issuer would match). Point in time: the quarter's 13F
    names; ``available_at`` = the quarter end + 46 days (the filing deadline floor)."""
    from . import identity_names as N

    con.create_function("stem13f", N.stem_finra, ["VARCHAR"], "VARCHAR", null_handling="special")
    con.create_function("alias_cusip_ok", cusip_valid, ["VARCHAR"], "BOOLEAN", null_handling="special")
    con.execute(f"""CREATE OR REPLACE TABLE qk AS
        SELECT period_q, cusip, value_sh, filers, stem13f(name) AS stem, alias_cusip_ok(cusip) AS ok
        FROM read_parquet('{q}') WHERE value_sh > 0""")
    con.execute(f"CREATE OR REPLACE TABLE qm AS {resolve_sql('hist2', 'qk', 'period_q')}")
    con.execute(f"""CREATE OR REPLACE TABLE alias AS
        WITH src AS (SELECT period_q, stem, security_id FROM qm
                     WHERE filers >= {ALIAS_MIN_FILERS} AND length(coalesce(stem, '')) >= {ALIAS_MIN_STEM}),
        uniq AS (SELECT period_q, stem, min(security_id) AS security_id FROM src GROUP BY 1, 2
                 HAVING count(DISTINCT security_id) = 1),
        tgt AS (SELECT k.* FROM qk k ANTI JOIN qm m ON m.period_q = k.period_q AND m.cusip = k.cusip
                WHERE NOT k.ok AND length(coalesce(k.stem, '')) >= {ALIAS_MIN_STEM})
        SELECT t.cusip, u.security_id, t.period_q, t.stem FROM tgt t JOIN uniq u USING (period_q, stem)""")
    return con.execute("""INSERT INTO hist2
        SELECT cusip, 'thirteenf_alias', security_id, 'thirteenf_name_alias', false, min(period_q), max(period_q),
               count(*), CAST(min(period_q) AS TIMESTAMP) + INTERVAL 46 DAY, NULL, any_value(stem),
               min(period_q), max(period_q)
        FROM alias GROUP BY cusip, security_id""").fetchone()[0]


def _vendor_glob() -> str:
    return (C.build_root() / "_tmp" / "shortflow" / "vendor" / "vendor_tickers_*.parquet").as_posix()


def build() -> dict[str, Any]:
    t0 = time.perf_counter()
    wd = work_dir()
    out = C.stage_dir(STAGE)
    con = C.connect(memory=MEMORY, threads=2)
    root = C.build_root()
    lines = _p(root / STAGE / "lines.parquet")
    runs = _p(wd / "ftd_cusip_runs.parquet")
    q = _p(wd / "thirteenf_cusip_q.parquet")
    receipt: dict[str, Any] = {"rule": RULE}
    con.execute(f"""CREATE TABLE ev AS
        SELECT cusip, 'cusip' AS cusip_kind, security_id, obs_from, obs_to, n_obs, available_at, 'ftd_symbol' AS basis,
               false AS snapshot, symbol, description AS name
        FROM read_parquet('{runs}')""")
    receipt["nport_runs_added"] = _nport_evidence(con, wd)
    figi = _openfigi_path()
    fig_rows = 0
    if figi.exists():
        # CUSIP -> US ticker (snapshot) -> the vendor line carrying it on the last vendor session; range = 13F span
        fp = _p(figi)
        con.execute(f"""CREATE TABLE fig AS
            SELECT cusip_field AS cusip, any_value(query_kind) AS query_kind, security_id, any_value(ticker) AS ticker,
                   any_value(name) AS name, min(fetched_at) AS fetched_at
            FROM read_parquet('{fp}')
            WHERE query_kind IN ('cusip', 'isin_fragment') AND security_id IS NOT NULL AND is_us_composite
            GROUP BY cusip_field, security_id""")
        # the 13F span is clipped to the line's own trading span (a re-keyed or later line must not claim the
        # quarters before it existed)
        fig_rows = con.execute(f"""INSERT INTO ev
            SELECT * FROM (
                SELECT f.cusip, CASE WHEN f.query_kind = 'isin_fragment' THEN 'isin_fragment' ELSE 'cusip' END,
                       f.security_id, greatest(s.first_q, l.first_session) AS a,
                       least(s.last_q, CAST(l.last_session + INTERVAL 92 DAY AS DATE)) AS b, s.n_q, f.fetched_at,
                       CASE WHEN f.query_kind = 'isin_fragment' THEN 'openfigi_isin_fragment' ELSE 'openfigi_ticker' END,
                       true, f.ticker, f.name
                FROM fig f
                JOIN (SELECT cusip, min(period_q) AS first_q, max(period_q) AS last_q, count(*) AS n_q
                      FROM read_parquet('{q}') GROUP BY 1) s USING (cusip)
                JOIN read_parquet('{lines}') l USING (security_id)
                ANTI JOIN (SELECT DISTINCT cusip, security_id FROM ev) e ON e.cusip = f.cusip AND e.security_id = f.security_id
            ) WHERE a <= b
        """).fetchone()[0]
    receipt["openfigi_runs_added"] = int(fig_rows or 0)
    # extended ranges per line (rule step 2)
    con.execute(f"""CREATE TABLE hist AS
        WITH l AS (SELECT security_id, first_session, last_session FROM read_parquet('{lines}')),
        o AS (
            SELECT ev.*, lag(obs_to) OVER w AS prev_to, lead(obs_from) OVER w AS next_from
            FROM ev WINDOW w AS (PARTITION BY security_id ORDER BY obs_from, obs_to, cusip)
        )
        SELECT o.cusip, o.cusip_kind, o.security_id, o.basis, o.snapshot, o.obs_from, o.obs_to, o.n_obs,
               greatest(coalesce(CAST(o.prev_to + INTERVAL 1 DAY AS DATE), l.first_session, o.obs_from),
                        CAST(o.obs_from - INTERVAL {EXTEND_DAYS} DAY AS DATE)) AS vf0,
               least(coalesce(CAST(o.next_from - INTERVAL 1 DAY AS DATE), l.last_session, o.obs_to),
                     CAST(o.obs_to + INTERVAL {EXTEND_DAYS} DAY AS DATE)) AS vt0,
               o.available_at, o.symbol, o.name
        FROM o LEFT JOIN l USING (security_id)""")
    # a range never excludes its own observations
    con.execute("""CREATE TABLE hist2 AS
        SELECT * EXCLUDE (vf0, vt0), least(vf0, obs_from) AS valid_from, greatest(vt0, obs_to) AS valid_to FROM hist""")
    receipt["thirteenf_name_alias_rows"] = _name_alias(con, q)
    prof = _p(root / "sec_filings" / "issuer_profile.parquet")
    link = _p(root / "identity" / "link_table.parquet")
    cc = ", ".join(f"'{c}'" for c in CANADA_STATE_CODES)
    con.create_function("isin_us", lambda c: isin_from_cusip(c, "US"), ["VARCHAR"], "VARCHAR", null_handling="special")
    con.create_function("isin_ca", lambda c: isin_from_cusip(c, "CA"), ["VARCHAR"], "VARCHAR", null_handling="special")
    con.create_function("cusip_ok", cusip_valid, ["VARCHAR"], "BOOLEAN", null_handling="special")
    con.create_function("issue_kind", issue_kind, ["VARCHAR"], "VARCHAR", null_handling="special")
    dest = out / "cusip_history.parquet"
    receipt["rows"] = C.copy_to_parquet(con, f"""
        WITH lk AS (SELECT security_id, arg_max(cik, valid_to) AS cik FROM read_parquet('{link}') GROUP BY 1),
        ca AS (SELECT cik FROM read_parquet('{prof}') WHERE state_of_incorporation IN ({cc})),
        ov AS (
            SELECT a.cusip, a.security_id, a.obs_from, count(DISTINCT b.security_id) AS n_lines_overlap
            FROM hist2 a JOIN hist2 b ON a.cusip = b.cusip AND a.valid_from <= b.valid_to AND b.valid_from <= a.valid_to
            GROUP BY 1, 2, 3
        )
        SELECT h.cusip, h.cusip_kind, left(h.cusip, 6) AS issuer6, h.security_id, lk.cik,
               h.valid_from, h.valid_to, h.obs_from, h.obs_to, h.n_obs, h.basis,
               CAST(h.available_at AS TIMESTAMP) AS available_at,
               CASE WHEN h.snapshot THEN 'snapshot_non_pit' END AS vintage_risk,
               h.symbol, h.name, ov.n_lines_overlap,
               cusip_ok(h.cusip) AS cusip_check_ok, issue_kind(h.cusip) AS issue_kind,
               CASE WHEN h.cusip_kind <> 'cusip' THEN NULL
                    WHEN ca.cik IS NOT NULL THEN isin_ca(h.cusip) ELSE isin_us(h.cusip) END AS isin,
               CASE WHEN h.cusip_kind <> 'cusip' OR NOT regexp_matches(h.cusip, '^[0-9]') THEN NULL
                    WHEN ca.cik IS NOT NULL THEN 'CA' ELSE 'US' END AS isin_country
        FROM hist2 h LEFT JOIN lk USING (security_id) LEFT JOIN ca USING (cik)
        LEFT JOIN ov ON ov.cusip = h.cusip AND ov.security_id = h.security_id AND ov.obs_from = h.obs_from
        ORDER BY h.cusip, h.valid_from, h.security_id""", dest)
    d = _p(dest)
    receipt["summary"] = dict(zip(
        ("cusips", "lines", "rows_ftd", "rows_openfigi", "overlap_rows", "isins", "cusip_check_fail"),
        con.execute(f"""SELECT count(DISTINCT cusip), count(DISTINCT security_id),
                               count(*) FILTER (WHERE basis = 'ftd_symbol'), count(*) FILTER (WHERE basis <> 'ftd_symbol'),
                               count(*) FILTER (WHERE n_lines_overlap > 1), count(DISTINCT isin),
                               count(*) FILTER (WHERE cusip_kind = 'cusip' AND NOT cusip_check_ok)
                        FROM read_parquet('{d}')""").fetchone()))
    con.create_function("isin_ok", isin_valid, ["VARCHAR"], "BOOLEAN", null_handling="special")
    receipt["isin_check_valid"] = con.execute(
        f"SELECT count(*) FILTER (WHERE isin_ok(isin)), count(isin) FROM read_parquet('{d}')").fetchone()
    receipt["seconds"] = round(time.perf_counter() - t0, 1)
    con.close()
    return receipt


# ---------------------------------------------------------------- measure
def measure(con=None) -> dict[str, Any]:
    """13F SH value share mapped to a line per quarter (see module doc)."""
    own = con is None
    con = con or C.connect(memory=MEMORY, threads=2)
    wd = work_dir()
    q = _p(wd / "thirteenf_cusip_q.parquet")
    pit = _p(thirteenf_dir() / "cusip_map_pit.parquet")
    hist = _p(C.build_root() / STAGE / "cusip_history.parquet")
    con.execute(f"CREATE OR REPLACE TEMP VIEW hq AS SELECT * FROM read_parquet('{hist}')")
    con.execute(f"CREATE OR REPLACE TEMP VIEW hq_ftd AS SELECT * FROM read_parquet('{hist}') WHERE basis = 'ftd_symbol'")
    con.execute(f"""CREATE OR REPLACE TEMP TABLE k AS
        SELECT period_q, cusip, value_sh, coalesce(value_sh_option_title, 0) AS v_opt FROM read_parquet('{q}')
        WHERE value_sh > 0""")
    con.create_function("issue_kind", issue_kind, ["VARCHAR"], "VARCHAR", null_handling="special")
    con.execute(f"CREATE OR REPLACE TEMP TABLE m_all AS {resolve_sql('hq', 'k', 'period_q')}")
    con.execute(f"CREATE OR REPLACE TEMP TABLE m_ftd AS {resolve_sql('hq_ftd', 'k', 'period_q')}")
    rows = con.execute(f"""
        SELECT k.period_q, sum(k.value_sh) AS v,
               sum(k.value_sh) FILTER (WHERE p.security_id IS NOT NULL) AS v_pit_stage,
               sum(k.value_sh) FILTER (WHERE mf.security_id IS NOT NULL) AS v_ftd,
               sum(k.value_sh) FILTER (WHERE ma.security_id IS NOT NULL) AS v_all,
               sum(k.v_eq) AS v_eq,
               sum(k.v_eq) FILTER (WHERE ma.security_id IS NOT NULL) AS v_eq_all,
               sum(k.v_eq) FILTER (WHERE mf.security_id IS NOT NULL) AS v_eq_ftd
        FROM (SELECT *, CASE WHEN issue_kind(cusip) <> 'debt' THEN value_sh - v_opt ELSE 0 END AS v_eq FROM k) k
        LEFT JOIN read_parquet('{pit}') p ON p.period_q = k.period_q AND p.cusip = k.cusip AND p.security_id IS NOT NULL
        LEFT JOIN m_ftd mf ON mf.period_q = k.period_q AND mf.cusip = k.cusip
        LEFT JOIN m_all ma ON ma.period_q = k.period_q AND ma.cusip = k.cusip
        GROUP BY 1 ORDER BY 1""").fetchall()
    out = {}
    for p, v, vp, vf, va, ve, vea, vef in rows:
        out[str(p)] = {"value_sh_usd": v, "stage_pit_map": _r(vp, v), "history_ftd": _r(vf, v), "history_all": _r(va, v),
                       "equity_only_share_of_value": _r(ve, v), "equity_only_history_ftd": _r(vef, ve),
                       "equity_only_history_all": _r(vea, ve)}
    if own:
        con.close()
    return out


def _r(a: float | None, b: float | None) -> float | None:
    return round(float(a or 0) / float(b), 5) if b else None


def publish(receipt: dict[str, Any]) -> Path:
    root = C.build_root()
    wd = work_dir()
    inputs = {name: C.sha256_file(path) for name, path in (
        ("ftd_manifest_sha256", root / "ftd" / "manifest.json"),
        ("thirteenf_manifest_sha256", root / "thirteenf" / "manifest.json"),
        ("security_master_manifest_sha256", root / STAGE / "manifest.json"),
        ("identity_link_table_manifest_sha256", root / "identity" / "link_table_manifest.json"),
        ("sec_filings_manifest_sha256", root / "sec_filings" / "manifest.json"),
    ) if path.exists()}
    fm = root / STAGE / "figi_manifest.json"
    if fm.exists():
        inputs["figi_manifest_sha256"] = C.sha256_file(fm)
    manifest = {
        "schema": SCHEMA, "status": "complete", "stage": STAGE, "rule": RULE, "rule_text": __doc__,
        "code": C.code_identity(*MODULES), "files": C.output_hashes(root / STAGE, "cusip_history.parquet"),
        "input_manifests_sha256": inputs,
        "work_files": C.output_hashes(wd, "thirteenf_cusip_q.parquet") | C.output_hashes(wd, "ftd_cusip_runs.parquet"),
        "staleness": "static identifier ranges; consumers resolve as of a date with the extended range",
        "receipt": receipt,
    }
    path = root / STAGE / "cusip_history_manifest.json"
    C.write_json_atomic(path, manifest)
    return path


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=("collect", "build", "measure", "all"))
    args = ap.parse_args(argv)
    if args.cmd in ("collect", "all"):
        print(json.dumps(collect(), default=str), flush=True)
    if args.cmd in ("build", "all"):
        rec = build()
        rec["thirteenf_coverage"] = measure()
        print(json.dumps({k: v for k, v in rec.items() if k != "thirteenf_coverage"}, default=str), flush=True)
        for k, v in rec["thirteenf_coverage"].items():
            print(k, v, flush=True)
        print(publish(rec), flush=True)
    if args.cmd == "measure":
        for k, v in measure().items():
            print(k, v, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
