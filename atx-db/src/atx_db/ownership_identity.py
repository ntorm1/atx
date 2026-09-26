"""Ownership and short-interest identity integration (task P9, fix round 1).

Two identity bridges keyed to the tier-1 owner (the CIK the research panel and the
owner bridge use), both built as bounded DuckDB relations on the caller's connection
(warehouse tables read, nothing written to the warehouse):

13F CUSIP -> owner (:func:`stage_13f_cusip_periods`, :func:`stage_cusip_owner_map`)
----------------------------------------------------------------------------------
**Eligible holdings** (the only rows that name a CUSIP-period or enter an IO numerator,
:func:`holding_exclusion_sql`): ``SH`` with no put/call, a common-equity ``title_of_class``
(:data:`COMMON_TITLE_PATTERN` and not :data:`NON_COMMON_TITLE_PATTERN`: preferreds, warrants,
units, rights, notes, ADRs are out), a CUSIP that is not a placeholder bucket (issuer code
``000000``/``999999`` or one repeated character) and whose mod-10 check digit validates
(zero-padded to 9), and a report period that is a calendar quarter end. Excluded rows are
counted by reason.

Per (CUSIP, report period) of eligible rows: the dominant normalized issuer name (upper
case, EDGAR state tags such as ``/DE/`` and a trailing ``DEL``/``NEW`` dropped,
punctuation folded, corporate designators INC/CORP/CO/LTD/PLC/LP/LLC/... removed), its share
of the rows (``name_share``; below :data:`NAME_DOMINANCE_MIN` the period is unmapped
``name_not_dominant``) and the number of distinct filers.

**Current CUSIP** (the only CUSIPs the current SEC snapshots may map): in the latest report
period it has at least :data:`MIN_CURRENT_FILERS` distinct filers, a dominant name, and that
name equals a current SEC title (``sec_company_tickers``). Current snapshots then apply only
to the CUSIP's **current name run**: the latest periods back to the last period whose
reported name differed. One stale filer never makes a delisted CUSIP current, and a current
CUSIP's older, differently named history is never remapped through today's titles.

Bases, per CUSIP-period:

``cusip_ticker_current``
    Current name run only: CUSIP -> ``security_identifier_history`` (``id_type='CUSIP'``,
    never the 13F loader's fallback seed, valid at the report date) -> the security's
    **dated** TICKER evidence valid at the report date (``valid_from`` set; several tickers
    -> ``ambiguous_ticker``, never an arbitrary pick) -> the current SEC ticker map (exactly
    one CIK). ``high`` when the name agrees, ``medium`` otherwise; a name that points only at
    other CIKs -> ``basis_conflict``.
``name_match``
    The name equals exactly one CIK's SEC entity name valid at the report date: a dated
    window (``security_identity_evidence`` issuer-name rows, namespace ``sec.issuer_name``;
    confidence by evidence status: ``verified_dated`` high, ``snapshot``/``reconstructed``
    medium, ``inferred`` low), or -- inside the current name run and only where no dated
    window of that name covers the date -- the current SEC title (``medium``).

The undated ``securities.primary_symbol`` hop is kept only as a labeled diagnostic
(``primary_symbol_undated``, confidence ``low``, reason ``primary_symbol_diagnostic_only``):
never a mapping, never a feature input. ``sample_conditioning`` labels every mapping:
``dated_name_window`` or ``cusip_survivor_conditioned`` (a current-snapshot basis: coverage
depends on the CUSIP surviving to the latest period). Unmapped reasons:
``no_identity_evidence``, ``ambiguous_name``, ``ambiguous_ticker``, ``basis_conflict``,
``name_not_dominant``. The whole map is reconstructed research identity
(``mapping_availability='modeled'``), never certified history.

FINRA short interest -> price line (:func:`stage_si_line_map`)
--------------------------------------------------------------
A FINRA row is (symbol, settlement date). The line is the price line whose bar on the
latest session in ``[settlement - 7 days, settlement]`` carried the same normalized
symbol (the owner bridge's normalization: upper case, ``.``/``/``/blank -> ``-``); two
lines on that session -> ``ambiguous_symbol_line``; none -> ``no_price_line_for_symbol``.
"""

from __future__ import annotations

import datetime as dt
from typing import Any

from .thirteenf import CUSIP_FALLBACK_SOURCE

IDENTITY_CUSIP_TICKER = "cusip_ticker_current"
IDENTITY_NAME_MATCH = "name_match"
IDENTITY_PRIMARY_SYMBOL = "primary_symbol_undated"
IDENTITY_BASES = (IDENTITY_CUSIP_TICKER, IDENTITY_NAME_MATCH)
CONFIDENCE_HIGH = "high"
CONFIDENCE_MEDIUM = "medium"
CONFIDENCE_LOW = "low"
MAPPED = "mapped"
UNMAPPED_NO_EVIDENCE = "no_identity_evidence"
UNMAPPED_AMBIGUOUS_NAME = "ambiguous_name"
UNMAPPED_AMBIGUOUS_TICKER = "ambiguous_ticker"
UNMAPPED_CONFLICT = "basis_conflict"
UNMAPPED_NOT_DOMINANT = "name_not_dominant"
DIAGNOSTIC_PRIMARY_SYMBOL = "primary_symbol_diagnostic_only"
UNMAPPED_REASONS = (UNMAPPED_NO_EVIDENCE, UNMAPPED_AMBIGUOUS_NAME, UNMAPPED_AMBIGUOUS_TICKER, UNMAPPED_CONFLICT,
                    UNMAPPED_NOT_DOMINANT, DIAGNOSTIC_PRIMARY_SYMBOL)
CONDITIONING_DATED = "dated_name_window"
CONDITIONING_SURVIVOR = "cusip_survivor_conditioned"
MAPPING_AVAILABILITY = "modeled"
NAME_NAMESPACE = "sec.issuer_name"
#: Dated-name evidence statuses and their confidence rank (1 high, 2 medium, 3 low).
NAME_STATUS_RANK = {"verified_dated": 1, "snapshot": 2, "reconstructed": 2, "inferred": 3}
ACCEPTED_NAME_STATUSES = tuple(NAME_STATUS_RANK)

#: A CUSIP is current only with at least this many distinct filers in the latest period.
MIN_CURRENT_FILERS = 3
#: The dominant name must carry at least this share of a CUSIP-period's eligible rows.
NAME_DOMINANCE_MIN = 0.8
#: Common-equity title_of_class (declared list, word-bounded, upper case).
COMMON_TITLE_PATTERN = (r"\b(COM|COMMON|CL ?[A-E]|CLASS [A-E]|ORD|ORDINARY|SHS|SHARES|SH BEN INT|CAP STK|"
                        r"CAPITAL STOCK)\b")
#: Never common equity, even when a common word also appears.
NON_COMMON_TITLE_PATTERN = (r"\b(PFD|PREF|PREFERRED|PRF|WT|WTS|WARRANT|WARRANTS|UNIT|UNITS|RT|RTS|RIGHT|RIGHTS|"
                            r"NOTE|NOTES|DEB|DBCV|SDCV|BOND|BONDS|ADR|ADS|ETN)\b")
EXCLUDED_NOT_SH = "not_sh_or_option"
EXCLUDED_TITLE = "title_not_common"
EXCLUDED_PLACEHOLDER = "cusip_placeholder"
EXCLUDED_CHECK_DIGIT = "cusip_invalid_check_digit"
EXCLUSION_REASONS = (EXCLUDED_NOT_SH, EXCLUDED_TITLE, EXCLUDED_PLACEHOLDER, EXCLUDED_CHECK_DIGIT)

SI_MAPPED = "mapped"
SI_NO_LINE = "no_price_line_for_symbol"
SI_AMBIGUOUS = "ambiguous_symbol_line"
SI_LOOKBACK_DAYS = 7

THIRTEENF_FORMS = ("13F-HR", "13F-HR/A")
_DESIGNATORS = ("THE", "INC", "INCORPORATED", "CORP", "CORPORATION", "CO", "COMPANY", "CORPORATE", "LTD",
                "LIMITED", "PLC", "LP", "L P", "LLC", "L L C", "NV", "N V", "SA", "S A", "AG", "HLDGS")


def issuer_name_key_sql(expression: str) -> str:
    """SQL normal form of an issuer name (see the module docstring); NULL for a blank name."""
    designators = "|".join(_DESIGNATORS)
    folded = (f"regexp_replace(regexp_replace(upper(coalesce(CAST({expression} AS VARCHAR), '')), "
              f"'/[A-Z]{{2,3}}/?', ' ', 'g'), '[^A-Z0-9]+', ' ', 'g')")
    stripped = f"regexp_replace(trim({folded}), ' (DEL|NEW)$', '')"  # CGS-style trailing markers
    for _ in range(2):  # adjacent designators share a separator: two passes
        stripped = f"regexp_replace({stripped}, '(^| )({designators})( |$)', ' ', 'g')"
    return f"nullif(trim(regexp_replace({stripped}, ' +', ' ', 'g')), '')"


def symbol_key_sql(expression: str) -> str:
    """The owner bridge's symbol normalization in SQL (``market_owner_bridge.normalize_symbol``)."""
    return (f"nullif(trim(regexp_replace(upper(trim(CAST({expression} AS VARCHAR))), '[./\\s]+', '-', 'g'), '-'), "
            f"'')")


def cusip_key_sql(expression: str) -> str:
    """A CUSIP as a 9-character key: upper case, blanks and dashes removed, zero-padded on the left."""
    return f"lpad(upper(regexp_replace(trim(CAST({expression} AS VARCHAR)), '[\\s-]', '', 'g')), 9, '0')"


def _char_value_sql(char: str) -> str:
    return (f"(CASE WHEN {char} BETWEEN '0' AND '9' THEN ascii({char}) - 48 "
            f"WHEN {char} BETWEEN 'A' AND 'Z' THEN ascii({char}) - 55 "
            f"WHEN {char} = '*' THEN 36 WHEN {char} = '@' THEN 37 ELSE 38 END)")


def cusip_status_sql(key: str) -> str:
    """NULL for a valid CUSIP key, else :data:`EXCLUDED_PLACEHOLDER` / :data:`EXCLUDED_CHECK_DIGIT`."""
    value = _char_value_sql(f"substr({key}, i_x, 1)")
    doubled = f"((CASE WHEN i_x % 2 = 0 THEN 2 ELSE 1 END) * {value})"
    check = f"((10 - list_sum(list_transform(range(1, 9), lambda i_x: {doubled} // 10 + {doubled} % 10)) % 10) % 10)"
    return (f"(CASE WHEN substr({key}, 1, 6) IN ('000000', '999999') OR {key} = repeat(substr({key}, 1, 1), 9) "
            f"THEN '{EXCLUDED_PLACEHOLDER}' "
            f"WHEN NOT regexp_full_match({key}, '[0-9A-Z*@#]{{8}}[0-9]') "
            f"OR {check} IS DISTINCT FROM try_cast(substr({key}, 9, 1) AS INTEGER) "
            f"THEN '{EXCLUDED_CHECK_DIGIT}' END)")


def holding_exclusion_sql(holding: str = "h") -> str:
    """NULL for a common-equity ``SH`` holding row without put/call, else its row exclusion reason.

    The CUSIP's own validity (:func:`cusip_status_sql`) is judged once per distinct CUSIP.
    """
    title = f"upper(coalesce({holding}.title_of_class, ''))"
    return (f"(CASE WHEN upper(coalesce({holding}.share_quantity_type, '')) <> 'SH' "
            f"OR coalesce(trim({holding}.put_call), '') <> '' THEN '{EXCLUDED_NOT_SH}' "
            f"WHEN NOT regexp_matches({title}, '{COMMON_TITLE_PATTERN}') "
            f"OR regexp_matches({title}, '{NON_COMMON_TITLE_PATTERN}') THEN '{EXCLUDED_TITLE}' END)")


def quarter_end_sql(expression: str) -> str:
    """Whether a report date is a calendar quarter end."""
    return f"({expression} = last_day({expression}) AND month({expression}) IN (3, 6, 9, 12))"


def _cik_sql(expression: str) -> str:
    return f"lpad(regexp_replace(CAST({expression} AS VARCHAR), '[^0-9]', '', 'g'), 10, '0')"


def _has_table(con: Any, table: str) -> bool:
    row = con.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name=? AND NOT temporary",
                      [table]).fetchone()
    return bool(row and row[0])


def stage_13f_cusip_periods(con: Any, *, out: str = "_oi_cusip_periods",
                            start: dt.date | None = None, end: dt.date | None = None) -> dict[str, Any]:
    """``out(cusip, report_period, issuer_name, name_key, holding_rows, name_share, filers)``.

    One row per CUSIP-period of eligible holdings (13F-HR / 13F-HR/A, quarter-end periods).
    Returns the row count, the excluded holding rows by reason and the rejected
    non-quarter-end report periods.
    """
    base = f"""
        FROM thirteenf_holdings h
        JOIN thirteenf_submissions s ON s.accession_number=h.accession_number AND s.source_period=h.source_period
        WHERE upper(trim(s.submission_type)) IN ('13F-HR', '13F-HR/A') AND s.period_of_report IS NOT NULL
          AND nullif(trim(h.cusip), '') IS NOT NULL AND {quarter_end_sql('s.period_of_report')}
          AND (CAST(? AS DATE) IS NULL OR s.period_of_report >= CAST(? AS DATE))
          AND (CAST(? AS DATE) IS NULL OR s.period_of_report <= CAST(? AS DATE))"""
    bounds = [start, start, end, end]
    # One aggregated pass over the holdings; CUSIP validity is then judged per distinct CUSIP.
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _oi_raw AS
        SELECT {cusip_key_sql('h.cusip')} AS cusip, s.period_of_report AS report_period, h.name_of_issuer,
               {_cik_sql('s.cik')} AS filer, {holding_exclusion_sql('h')} AS row_excluded, count(*) AS n
        {base}
        GROUP BY ALL
    """, bounds)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _oi_cusip_status AS
        SELECT cusip, {cusip_status_sql('cusip')} AS status FROM (SELECT DISTINCT cusip FROM _oi_raw)
    """)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _oi_eligible AS
        SELECT r.cusip, r.report_period, r.name_of_issuer, r.filer, r.n
        FROM _oi_raw r JOIN _oi_cusip_status s ON s.cusip=r.cusip
        WHERE r.row_excluded IS NULL AND s.status IS NULL
    """)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE {out} AS
        WITH filers AS (
            SELECT cusip, report_period, count(DISTINCT filer) AS filers FROM _oi_eligible GROUP BY ALL
        ), named AS (  -- the name normalization runs once per distinct raw name
            SELECT cusip, report_period, {issuer_name_key_sql('name_of_issuer')} AS name_key,
                   any_value(name_of_issuer) AS issuer_name, sum(n) AS n
            FROM _oi_eligible GROUP BY ALL
        ), ranked AS (
            SELECT *, sum(n) OVER (PARTITION BY cusip, report_period) AS holding_rows
            FROM named
            QUALIFY row_number() OVER (PARTITION BY cusip, report_period
                                       ORDER BY (name_key IS NULL), n DESC, name_key) = 1
        )
        SELECT r.cusip, r.report_period, r.issuer_name, r.name_key, r.holding_rows,
               CAST(r.n AS DOUBLE) / r.holding_rows AS name_share, coalesce(f.filers, 0) AS filers
        FROM ranked r LEFT JOIN filers f ON f.cusip=r.cusip AND f.report_period=r.report_period
    """)
    excluded = {str(reason): int(rows) for reason, rows in con.execute("""
        SELECT coalesce(r.row_excluded, s.status) AS reason, sum(r.n)
        FROM _oi_raw r JOIN _oi_cusip_status s ON s.cusip=r.cusip
        WHERE r.row_excluded IS NOT NULL OR s.status IS NOT NULL GROUP BY 1 ORDER BY 1""").fetchall()}
    rejected = con.execute(f"""
        SELECT count(*) FROM thirteenf_submissions s
        WHERE upper(trim(s.submission_type)) IN ('13F-HR', '13F-HR/A') AND s.period_of_report IS NOT NULL
          AND NOT {quarter_end_sql('s.period_of_report')}""").fetchone()
    return {"cusip_periods": int(con.execute(f"SELECT count(*) FROM {out}").fetchone()[0]),
            "excluded_rows": excluded, "rejected_non_quarter_end_filings": int(rejected[0])}


def _stage_names(con: Any) -> dict[str, int]:
    """``_oi_names(cik, name_key, valid_from, valid_to, dated, status_rank)``: SEC entity names."""
    ranks = " ".join(f"WHEN '{status}' THEN {rank}" for status, rank in NAME_STATUS_RANK.items())
    parts = [f"""
        SELECT DISTINCT {_cik_sql('cik')} AS cik, {issuer_name_key_sql('title')} AS name_key,
               CAST(NULL AS DATE) AS valid_from, CAST(NULL AS DATE) AS valid_to, false AS dated,
               CAST(NULL AS INTEGER) AS status_rank
        FROM sec_company_tickers WHERE title IS NOT NULL"""]
    if _has_table(con, "security_identity_evidence"):
        parts.append(f"""
        SELECT DISTINCT {_cik_sql('cik')}, {issuer_name_key_sql("coalesce(json_extract_string(value_json, '$.issuer_name'), native_key)")},
               valid_from, valid_to, true, CASE evidence_status {ranks} END
        FROM security_identity_evidence
        WHERE fact_kind='issuer_link' AND native_key_namespace='{NAME_NAMESPACE}' AND cik IS NOT NULL
          AND evidence_status IN ({", ".join(f"'{s}'" for s in ACCEPTED_NAME_STATUSES)})
          AND coalesce(is_latest_revision, true) AND valid_from IS NOT NULL""")
    con.execute("CREATE OR REPLACE TEMP TABLE _oi_names AS SELECT * FROM (" + " UNION ALL ".join(parts)
                + ") WHERE name_key IS NOT NULL")
    dated, current = con.execute("SELECT count(*) FILTER (WHERE dated), count(*) FILTER (WHERE NOT dated) "
                                 "FROM _oi_names").fetchone()
    return {"dated_name_windows": int(dated), "current_titles": int(current)}


def stage_cusip_owner_map(con: Any, *, periods: str = "_oi_cusip_periods", out: str = "_oi_cusip_owner",
                          latest_period: dt.date | None = None) -> dict[str, Any]:
    """``out(cusip, report_period, name_key, owner_cik, identity_basis, confidence, reason, sample_conditioning)``.

    One row per row of ``periods`` (mapped or not). ``latest_period`` (default: the latest
    report period in ``periods``) defines a *current* CUSIP. Returns counts by basis,
    confidence, unmapped reason and sample conditioning.
    """
    names = _stage_names(con)
    if latest_period is None:
        row = con.execute(f"SELECT max(report_period) FROM {periods}").fetchone()
        latest_period = row[0] if row else None
    # Current CUSIPs and their current name run (periods after the last differently named one).
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _oi_current_cusips AS
        WITH current AS (
            SELECT p.cusip, p.name_key AS current_key
            FROM {periods} p
            WHERE p.report_period = CAST(? AS DATE) AND p.filers >= {MIN_CURRENT_FILERS}
              AND p.name_share >= {NAME_DOMINANCE_MIN} AND p.name_key IS NOT NULL
              AND p.name_key IN (SELECT name_key FROM _oi_names WHERE NOT dated)
        ), breaks AS (
            SELECT p.cusip, max(p.report_period) AS last_break
            FROM {periods} p JOIN current c ON c.cusip=p.cusip
            WHERE NOT coalesce(p.name_key = c.current_key, false)
            GROUP BY p.cusip
        )
        SELECT c.cusip, c.current_key, b.last_break FROM current c LEFT JOIN breaks b ON b.cusip=c.cusip
    """, [latest_period])
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _oi_flagged AS
        SELECT p.*, cc.cusip IS NOT NULL AND (cc.last_break IS NULL OR p.report_period > cc.last_break) AS in_run
        FROM {periods} p LEFT JOIN _oi_current_cusips cc ON cc.cusip=p.cusip
    """)
    ticker_history = _has_table(con, "security_identifier_history")
    if ticker_history:
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _oi_ticker_links AS
            WITH cusip_links AS (
                SELECT DISTINCT p.cusip, p.report_period, h.security_id
                FROM _oi_flagged p
                JOIN security_identifier_history h
                  ON h.id_type='CUSIP' AND {cusip_key_sql('h.id_value')}=p.cusip AND h.source <> ?
                 AND (h.valid_from IS NULL OR h.valid_from <= p.report_period)
                 AND (h.valid_to IS NULL OR p.report_period < h.valid_to)
                WHERE p.in_run
            ), dated_tickers AS (  -- TICKER evidence dated and valid at the report date only
                SELECT c.cusip, c.report_period, {symbol_key_sql('t.id_value')} AS ticker_key
                FROM cusip_links c
                JOIN security_identifier_history t
                  ON t.security_id=c.security_id AND t.id_type='TICKER' AND t.valid_from IS NOT NULL
                 AND t.valid_from <= c.report_period AND (t.valid_to IS NULL OR c.report_period < t.valid_to)
            ), per_period AS (
                SELECT cusip, report_period, count(DISTINCT ticker_key) AS tickers, min(ticker_key) AS ticker_key
                FROM dated_tickers WHERE ticker_key IS NOT NULL GROUP BY ALL
            ), sec_map AS (
                SELECT {symbol_key_sql('ticker')} AS ticker_key, {_cik_sql('cik')} AS cik FROM sec_company_tickers
            )
            SELECT t.cusip, t.report_period,
                   CASE WHEN t.tickers > 1 THEN 2 ELSE count(DISTINCT m.cik) END AS ciks, min(m.cik) AS cik
            FROM per_period t LEFT JOIN sec_map m ON m.ticker_key=t.ticker_key
            GROUP BY t.cusip, t.report_period, t.tickers
            HAVING t.tickers > 1 OR count(DISTINCT m.cik) > 0
        """, [CUSIP_FALLBACK_SOURCE])
    else:
        con.execute("CREATE OR REPLACE TEMP TABLE _oi_ticker_links (cusip VARCHAR, report_period DATE, "
                    "ciks BIGINT, cik VARCHAR)")
    if ticker_history and _has_table(con, "securities"):
        # Labeled diagnostic only: the undated primary symbol of the CUSIP's security.
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _oi_primary_diag AS
            SELECT p.cusip, p.report_period, min(m.cik) AS cik
            FROM _oi_flagged p
            JOIN security_identifier_history h
              ON h.id_type='CUSIP' AND {cusip_key_sql('h.id_value')}=p.cusip AND h.source <> ?
            JOIN securities s ON s.security_id=h.security_id
            JOIN (SELECT {symbol_key_sql('ticker')} AS ticker_key, {_cik_sql('cik')} AS cik FROM sec_company_tickers) m
              ON m.ticker_key={symbol_key_sql('s.primary_symbol')}
            WHERE p.in_run
            GROUP BY p.cusip, p.report_period HAVING count(DISTINCT m.cik) = 1
        """, [CUSIP_FALLBACK_SOURCE])
    else:
        con.execute("CREATE OR REPLACE TEMP TABLE _oi_primary_diag (cusip VARCHAR, report_period DATE, cik VARCHAR)")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _oi_name_links AS
        WITH dated AS (
            SELECT p.cusip, p.report_period, list_sort(list(DISTINCT n.cik)) AS ciks, min(n.status_rank) AS rank
            FROM _oi_flagged p JOIN _oi_names n
              ON n.dated AND n.name_key=p.name_key AND n.valid_from <= p.report_period
             AND (n.valid_to IS NULL OR p.report_period < n.valid_to)
            GROUP BY p.cusip, p.report_period
        ), undated AS (  -- current titles: current name run only, never where a dated window covers the name
            SELECT p.cusip, p.report_period, list_sort(list(DISTINCT n.cik)) AS ciks
            FROM _oi_flagged p JOIN _oi_names n ON NOT n.dated AND n.name_key=p.name_key
            WHERE p.in_run AND NOT EXISTS (SELECT 1 FROM dated d WHERE d.cusip=p.cusip
                                                                 AND d.report_period=p.report_period)
            GROUP BY p.cusip, p.report_period
        )
        SELECT cusip, report_period, ciks, len(ciks) AS ciks_n, ciks[1] AS cik, true AS dated, rank FROM dated
        UNION ALL
        SELECT cusip, report_period, ciks, len(ciks), ciks[1], false, NULL FROM undated
    """)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE {out} AS
        WITH joined AS (
            SELECT p.cusip, p.report_period, p.name_key, p.name_share,
                   t.ciks AS t_n, t.cik AS t_cik, n.ciks AS n_list, coalesce(n.ciks_n, 0) AS n_n, n.cik AS n_cik,
                   coalesce(n.dated, false) AS n_dated, n.rank AS n_rank, d.cik AS d_cik
            FROM _oi_flagged p
            LEFT JOIN _oi_ticker_links t ON t.cusip=p.cusip AND t.report_period=p.report_period
            LEFT JOIN _oi_name_links n ON n.cusip=p.cusip AND n.report_period=p.report_period
            LEFT JOIN _oi_primary_diag d ON d.cusip=p.cusip AND d.report_period=p.report_period
        ), decided AS (
            SELECT *,
                   CASE WHEN name_share < {NAME_DOMINANCE_MIN} THEN '{UNMAPPED_NOT_DOMINANT}'
                        WHEN t_n > 1 THEN '{UNMAPPED_AMBIGUOUS_TICKER}'
                        WHEN t_n = 1 AND n_n >= 1 AND NOT list_contains(n_list, t_cik) THEN '{UNMAPPED_CONFLICT}'
                        WHEN t_n = 1 THEN '{MAPPED}'
                        WHEN n_n = 1 THEN '{MAPPED}'
                        WHEN n_n > 1 THEN '{UNMAPPED_AMBIGUOUS_NAME}'
                        WHEN d_cik IS NOT NULL THEN '{DIAGNOSTIC_PRIMARY_SYMBOL}'
                        ELSE '{UNMAPPED_NO_EVIDENCE}' END AS reason
            FROM joined
        )
        SELECT cusip, report_period, name_key,
               CASE WHEN reason='{MAPPED}' THEN coalesce(CASE WHEN t_n = 1 THEN t_cik END, n_cik)
                    WHEN reason='{DIAGNOSTIC_PRIMARY_SYMBOL}' THEN d_cik END AS owner_cik,
               CASE WHEN reason='{DIAGNOSTIC_PRIMARY_SYMBOL}' THEN '{IDENTITY_PRIMARY_SYMBOL}'
                    WHEN reason<>'{MAPPED}' THEN NULL
                    WHEN t_n = 1 THEN '{IDENTITY_CUSIP_TICKER}' ELSE '{IDENTITY_NAME_MATCH}' END AS identity_basis,
               CASE WHEN reason='{DIAGNOSTIC_PRIMARY_SYMBOL}' THEN '{CONFIDENCE_LOW}'
                    WHEN reason<>'{MAPPED}' THEN NULL
                    WHEN t_n = 1 AND n_n >= 1 THEN '{CONFIDENCE_HIGH}'
                    WHEN t_n = 1 THEN '{CONFIDENCE_MEDIUM}'
                    WHEN n_dated THEN CASE n_rank WHEN 1 THEN '{CONFIDENCE_HIGH}' WHEN 2 THEN '{CONFIDENCE_MEDIUM}'
                                                  ELSE '{CONFIDENCE_LOW}' END
                    ELSE '{CONFIDENCE_MEDIUM}' END AS confidence,
               reason,
               CASE WHEN reason NOT IN ('{MAPPED}', '{DIAGNOSTIC_PRIMARY_SYMBOL}') THEN NULL
                    WHEN reason='{MAPPED}' AND t_n IS DISTINCT FROM 1 AND n_dated THEN '{CONDITIONING_DATED}'
                    ELSE '{CONDITIONING_SURVIVOR}' END AS sample_conditioning
        FROM decided
    """)
    counts: dict[str, Any] = {"latest_period": None if latest_period is None else str(latest_period), **names}
    counts["cusip_periods"] = int(con.execute(f"SELECT count(*) FROM {out}").fetchone()[0])
    counts["current_cusips"] = int(con.execute("SELECT count(*) FROM _oi_current_cusips").fetchone()[0])
    counts["mapped"] = {f"{basis}/{confidence}": int(n) for basis, confidence, n in con.execute(
        f"SELECT identity_basis, confidence, count(*) FROM {out} WHERE reason='{MAPPED}' GROUP BY 1, 2 ORDER BY 1, 2"
    ).fetchall()}
    counts["unmapped"] = {str(reason): int(n) for reason, n in con.execute(
        f"SELECT reason, count(*) FROM {out} WHERE reason<>'{MAPPED}' GROUP BY 1 ORDER BY 1").fetchall()}
    counts["sample_conditioning"] = {str(label): int(n) for label, n in con.execute(
        f"SELECT sample_conditioning, count(*) FROM {out} WHERE reason='{MAPPED}' GROUP BY 1 ORDER BY 1").fetchall()}
    return counts


def stage_si_line_map(con: Any, *, short_interest: str = "finra_short_interest", out: str = "_oi_si_line",
                      lookback_days: int = SI_LOOKBACK_DAYS) -> dict[str, int]:
    """``out(symbol, symbol_key, settlement_date, security_id, reason)``: FINRA row -> price line by symbol-date."""
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _oi_si_keys AS
        SELECT DISTINCT symbol, {symbol_key_sql('symbol')} AS symbol_key, settlement_date
        FROM {short_interest} WHERE symbol IS NOT NULL AND settlement_date IS NOT NULL
    """)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE {out} AS
        WITH hits AS (
            SELECT k.symbol, k.symbol_key, k.settlement_date, b.security_id, b.trade_date
            FROM _oi_si_keys k JOIN equity_daily_bars b
              ON b.trade_date BETWEEN k.settlement_date - CAST(? AS INTEGER) AND k.settlement_date
             AND {symbol_key_sql('b.symbol')}=k.symbol_key
        ), latest AS (
            SELECT symbol, symbol_key, settlement_date, max(trade_date) AS last_day FROM hits GROUP BY 1, 2, 3
        ), lines AS (
            SELECT h.symbol, h.symbol_key, h.settlement_date, count(DISTINCT h.security_id) AS n,
                   min(h.security_id) AS security_id
            FROM hits h JOIN latest l ON l.symbol=h.symbol AND l.settlement_date=h.settlement_date
             AND h.trade_date=l.last_day
            GROUP BY 1, 2, 3
        )
        SELECT k.symbol, k.symbol_key, k.settlement_date,
               CASE WHEN l.n = 1 THEN l.security_id END AS security_id,
               CASE WHEN l.n IS NULL THEN '{SI_NO_LINE}' WHEN l.n > 1 THEN '{SI_AMBIGUOUS}'
                    ELSE '{SI_MAPPED}' END AS reason
        FROM _oi_si_keys k LEFT JOIN lines l ON l.symbol=k.symbol AND l.settlement_date=k.settlement_date
    """, [lookback_days])
    return {str(reason): int(n) for reason, n in con.execute(
        f"SELECT reason, count(*) FROM {out} GROUP BY 1 ORDER BY 1").fetchall()}


__all__ = [
    "COMMON_TITLE_PATTERN",
    "CONDITIONING_DATED",
    "CONDITIONING_SURVIVOR",
    "CONFIDENCE_HIGH",
    "CONFIDENCE_LOW",
    "CONFIDENCE_MEDIUM",
    "EXCLUSION_REASONS",
    "IDENTITY_BASES",
    "IDENTITY_CUSIP_TICKER",
    "IDENTITY_NAME_MATCH",
    "MAPPED",
    "MAPPING_AVAILABILITY",
    "MIN_CURRENT_FILERS",
    "NAME_DOMINANCE_MIN",
    "NON_COMMON_TITLE_PATTERN",
    "SI_AMBIGUOUS",
    "SI_MAPPED",
    "SI_NO_LINE",
    "UNMAPPED_REASONS",
    "cusip_key_sql",
    "cusip_status_sql",
    "holding_exclusion_sql",
    "issuer_name_key_sql",
    "quarter_end_sql",
    "stage_13f_cusip_periods",
    "stage_cusip_owner_map",
    "stage_si_line_map",
    "symbol_key_sql",
]
