"""Ownership and short-interest identity integration (task P9).

Two identity bridges keyed to the tier-1 owner (the CIK the research panel and the
owner bridge use), both built as bounded DuckDB relations on the caller's connection
(warehouse tables read, nothing written to the warehouse):

13F CUSIP -> owner (:func:`stage_cusip_owner_map`)
--------------------------------------------------
The 13F information table names a security by CUSIP and a free-text
``name_of_issuer``. For every (CUSIP, report period) that some 13F-HR filing holds, the
issuer name is the most frequent normalized name among that period's rows. Two bases:

``cusip_ticker_current``
    CUSIP -> ``security_identifier_history`` (``id_type='CUSIP'``, never the 13F loader's
    own fallback seed, valid at the report date) -> the security's current ticker (open
    ``TICKER`` interval, else ``securities.primary_symbol``) -> the current SEC ticker map
    (``sec_company_tickers``, exactly one CIK). The ticker map is a current snapshot, so the
    basis is only asserted for a *current* CUSIP (held in the latest report period in the
    data); a CUSIP that stopped being held is never re-attributed through today's tickers.
``name_match``
    Normalized issuer name (upper case, EDGAR state tags such as ``/DE/`` dropped,
    punctuation folded, corporate designators INC/CORP/CO/LTD/PLC/LP/LLC/... removed) equal
    to exactly one CIK's SEC entity name valid at the report date: a *dated* name window
    (``security_identity_evidence`` issuer-name rows, namespace ``sec.issuer_name``, when
    the warehouse has them) or, for a current CUSIP only, the current SEC title
    (``sec_company_tickers.title``). Exact match only: no fuzzy matching.

Decision per (CUSIP, report period): both bases agree -> ``cusip_ticker_current``,
confidence ``high``; ticker only (or the name ambiguous) -> ``cusip_ticker_current``
``medium``; name only -> ``name_match`` ``high`` (dated window) or ``medium`` (current
title); they disagree -> unmapped ``basis_conflict``; several CIKs through the ticker ->
``ambiguous_ticker`` (the name never overrides an ambiguous ticker). Unmapped rows are kept and counted by reason
(``no_identity_evidence``, ``ambiguous_name``, ``ambiguous_ticker``, ``basis_conflict``).
A mapping exists only for report periods in which the CUSIP is actually held, so a
delisted issuer's CUSIP maps only inside its holding (evidence) window, and a dated name
window never extends a mapping past the window's end. The whole map is reconstructed
research identity (current snapshots backcast, dated names from a later archive):
``mapping_availability='modeled'``, never certified history.

FINRA short interest -> price line (:func:`stage_si_line_map`)
--------------------------------------------------------------
A FINRA row is (symbol, settlement date). The line is the price line whose bar on the
latest session in ``[settlement - 7 days, settlement]`` carried the same normalized
symbol (the owner bridge's normalization: upper case, ``.``/``/``/blank -> ``-``); two
lines on that session -> ``ambiguous_symbol_line``; none -> ``no_price_line_for_symbol``.
The owner is then the owner-bridge owner of that line, which research consumers take from
the panel cohort of their run basis.
"""

from __future__ import annotations

import datetime as dt
from typing import Any

from .thirteenf import CUSIP_FALLBACK_SOURCE

IDENTITY_CUSIP_TICKER = "cusip_ticker_current"
IDENTITY_NAME_MATCH = "name_match"
IDENTITY_BASES = (IDENTITY_CUSIP_TICKER, IDENTITY_NAME_MATCH)
CONFIDENCE_HIGH = "high"
CONFIDENCE_MEDIUM = "medium"
MAPPED = "mapped"
UNMAPPED_NO_EVIDENCE = "no_identity_evidence"
UNMAPPED_AMBIGUOUS_NAME = "ambiguous_name"
UNMAPPED_AMBIGUOUS_TICKER = "ambiguous_ticker"
UNMAPPED_CONFLICT = "basis_conflict"
UNMAPPED_REASONS = (UNMAPPED_NO_EVIDENCE, UNMAPPED_AMBIGUOUS_NAME, UNMAPPED_AMBIGUOUS_TICKER, UNMAPPED_CONFLICT)
MAPPING_AVAILABILITY = "modeled"
NAME_NAMESPACE = "sec.issuer_name"
ACCEPTED_NAME_STATUSES = ("verified_dated", "snapshot", "reconstructed", "inferred")

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
    stripped = folded
    for _ in range(2):  # adjacent designators share a separator: two passes
        stripped = f"regexp_replace({stripped}, '(^| )({designators})( |$)', ' ', 'g')"
    return f"nullif(trim(regexp_replace({stripped}, ' +', ' ', 'g')), '')"


def symbol_key_sql(expression: str) -> str:
    """The owner bridge's symbol normalization in SQL (``market_owner_bridge.normalize_symbol``)."""
    return (f"nullif(trim(regexp_replace(upper(trim(CAST({expression} AS VARCHAR))), '[./\\s]+', '-', 'g'), '-'), "
            f"'')")


def _cik_sql(expression: str) -> str:
    return f"lpad(regexp_replace(CAST({expression} AS VARCHAR), '[^0-9]', '', 'g'), 10, '0')"


def _has_table(con: Any, table: str) -> bool:
    row = con.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name=? AND NOT temporary",
                      [table]).fetchone()
    return bool(row and row[0])


def stage_13f_cusip_periods(con: Any, *, out: str = "_oi_cusip_periods",
                            start: dt.date | None = None, end: dt.date | None = None) -> int:
    """``out(cusip, report_period, issuer_name, name_key, holding_rows)``: one row per held CUSIP-period.

    Streams ``thirteenf_holdings`` once, aggregated (13F-HR / 13F-HR/A filings only).
    """
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE {out} AS
        WITH raw AS (  -- aggregate first: the name normalization runs once per distinct raw name
            SELECT upper(replace(replace(trim(h.cusip), ' ', ''), '-', '')) AS cusip,
                   s.period_of_report AS report_period, h.name_of_issuer, count(*) AS n
            FROM thirteenf_holdings h
            JOIN thirteenf_submissions s ON s.accession_number=h.accession_number AND s.source_period=h.source_period
            WHERE upper(trim(s.submission_type)) IN ('13F-HR', '13F-HR/A') AND s.period_of_report IS NOT NULL
              AND nullif(trim(h.cusip), '') IS NOT NULL
              AND (CAST(? AS DATE) IS NULL OR s.period_of_report >= CAST(? AS DATE))
              AND (CAST(? AS DATE) IS NULL OR s.period_of_report <= CAST(? AS DATE))
            GROUP BY 1, 2, 3
        ), named AS (
            SELECT cusip, report_period, {issuer_name_key_sql('name_of_issuer')} AS name_key,
                   any_value(name_of_issuer) AS issuer_name, sum(n) AS n
            FROM raw GROUP BY ALL
        )
        SELECT cusip, report_period, issuer_name, name_key, sum(n) OVER (PARTITION BY cusip, report_period)
                   AS holding_rows
        FROM named
        QUALIFY row_number() OVER (PARTITION BY cusip, report_period
                                   ORDER BY (name_key IS NULL), n DESC, name_key) = 1
    """, [start, start, end, end])
    return int(con.execute(f"SELECT count(*) FROM {out}").fetchone()[0])


def _stage_names(con: Any) -> dict[str, int]:
    """``_oi_names(cik, name_key, valid_from, valid_to, dated)``: SEC entity names."""
    parts = [f"""
        SELECT DISTINCT {_cik_sql('cik')} AS cik, {issuer_name_key_sql('title')} AS name_key,
               CAST(NULL AS DATE) AS valid_from, CAST(NULL AS DATE) AS valid_to, false AS dated
        FROM sec_company_tickers WHERE title IS NOT NULL"""]
    if _has_table(con, "security_identity_evidence"):
        parts.append(f"""
        SELECT DISTINCT {_cik_sql('cik')}, {issuer_name_key_sql("coalesce(json_extract_string(value_json, '$.issuer_name'), native_key)")},
               valid_from, valid_to, true
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
    """``out(cusip, report_period, name_key, owner_cik, identity_basis, confidence, reason)``.

    One row per row of ``periods`` (mapped or not). ``latest_period`` (default: the latest
    report period in ``periods``) defines a *current* CUSIP for the current-snapshot bases.
    Returns counts by basis, confidence and unmapped reason.
    """
    names = _stage_names(con)
    if latest_period is None:
        row = con.execute(f"SELECT max(report_period) FROM {periods}").fetchone()
        latest_period = row[0] if row else None
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _oi_current_cusips AS
        SELECT cusip FROM {periods} GROUP BY cusip HAVING max(report_period) >= CAST(? AS DATE)
    """, [latest_period])
    ticker_history = _has_table(con, "security_identifier_history")
    securities = _has_table(con, "securities")
    if ticker_history:
        primary = ("(SELECT max(s.primary_symbol) FROM securities s WHERE s.security_id=c.security_id)"
                   if securities else "CAST(NULL AS VARCHAR)")
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _oi_ticker_links AS
            WITH cusip_links AS (
                SELECT p.cusip, p.report_period, h.security_id
                FROM {periods} p SEMI JOIN _oi_current_cusips cc ON cc.cusip=p.cusip
                JOIN security_identifier_history h
                  ON h.id_type='CUSIP' AND upper(h.id_value)=p.cusip AND h.source <> ?
                 AND (h.valid_from IS NULL OR h.valid_from <= p.report_period)
                 AND (h.valid_to IS NULL OR p.report_period < h.valid_to)
            ), current_tickers AS (
                SELECT c.cusip, c.report_period, c.security_id,
                       coalesce((SELECT max(t.id_value) FROM security_identifier_history t
                                 WHERE t.security_id=c.security_id AND t.id_type='TICKER' AND t.valid_to IS NULL),
                                {primary}) AS ticker
                FROM cusip_links c
            ), sec_map AS (
                SELECT {symbol_key_sql('ticker')} AS ticker_key, {_cik_sql('cik')} AS cik FROM sec_company_tickers
            )
            SELECT t.cusip, t.report_period, count(DISTINCT m.cik) AS ciks, min(m.cik) AS cik
            FROM current_tickers t JOIN sec_map m ON m.ticker_key={symbol_key_sql('t.ticker')}
            GROUP BY t.cusip, t.report_period
        """, [CUSIP_FALLBACK_SOURCE])
    else:
        con.execute("CREATE OR REPLACE TEMP TABLE _oi_ticker_links (cusip VARCHAR, report_period DATE, "
                    "ciks BIGINT, cik VARCHAR)")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _oi_name_links AS
        WITH flagged AS (
            SELECT p.cusip, p.report_period, p.name_key, cc.cusip IS NOT NULL AS is_current
            FROM {periods} p LEFT JOIN _oi_current_cusips cc ON cc.cusip=p.cusip
        )
        SELECT p.cusip, p.report_period, count(DISTINCT n.cik) AS ciks, min(n.cik) AS cik, bool_or(n.dated) AS dated
        FROM flagged p JOIN _oi_names n ON n.name_key=p.name_key
         AND ((n.dated AND n.valid_from <= p.report_period AND (n.valid_to IS NULL OR p.report_period < n.valid_to))
              OR (NOT n.dated AND p.is_current))
        GROUP BY p.cusip, p.report_period
    """)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE {out} AS
        WITH joined AS (
            SELECT p.cusip, p.report_period, p.name_key,
                   t.ciks AS t_n, t.cik AS t_cik, n.ciks AS n_n, n.cik AS n_cik, coalesce(n.dated, false) AS n_dated
            FROM {periods} p
            LEFT JOIN _oi_ticker_links t ON t.cusip=p.cusip AND t.report_period=p.report_period
            LEFT JOIN _oi_name_links n ON n.cusip=p.cusip AND n.report_period=p.report_period
        ), decided AS (
            SELECT *,
                   CASE WHEN t_n = 1 AND n_n = 1 AND t_cik <> n_cik THEN '{UNMAPPED_CONFLICT}'
                        WHEN t_n > 1 THEN '{UNMAPPED_AMBIGUOUS_TICKER}'
                        WHEN t_n = 1 THEN '{MAPPED}'
                        WHEN n_n = 1 THEN '{MAPPED}'
                        WHEN n_n > 1 THEN '{UNMAPPED_AMBIGUOUS_NAME}'
                        ELSE '{UNMAPPED_NO_EVIDENCE}' END AS reason
            FROM joined
        )
        SELECT cusip, report_period, name_key,
               CASE WHEN reason='{MAPPED}' THEN coalesce(CASE WHEN t_n = 1 THEN t_cik END, n_cik) END AS owner_cik,
               CASE WHEN reason<>'{MAPPED}' THEN NULL
                    WHEN t_n = 1 THEN '{IDENTITY_CUSIP_TICKER}' ELSE '{IDENTITY_NAME_MATCH}' END AS identity_basis,
               CASE WHEN reason<>'{MAPPED}' THEN NULL
                    WHEN t_n = 1 AND n_n = 1 THEN '{CONFIDENCE_HIGH}'
                    WHEN t_n = 1 THEN '{CONFIDENCE_MEDIUM}'
                    WHEN n_dated THEN '{CONFIDENCE_HIGH}' ELSE '{CONFIDENCE_MEDIUM}' END AS confidence,
               reason
        FROM decided
    """)
    counts: dict[str, Any] = {"latest_period": None if latest_period is None else str(latest_period), **names}
    counts["cusip_periods"] = int(con.execute(f"SELECT count(*) FROM {out}").fetchone()[0])
    counts["mapped"] = {f"{basis}/{confidence}": int(n) for basis, confidence, n in con.execute(
        f"SELECT identity_basis, confidence, count(*) FROM {out} WHERE reason='{MAPPED}' GROUP BY 1, 2 ORDER BY 1, 2"
    ).fetchall()}
    counts["unmapped"] = {str(reason): int(n) for reason, n in con.execute(
        f"SELECT reason, count(*) FROM {out} WHERE reason<>'{MAPPED}' GROUP BY 1 ORDER BY 1").fetchall()}
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
    "CONFIDENCE_HIGH",
    "CONFIDENCE_MEDIUM",
    "IDENTITY_BASES",
    "IDENTITY_CUSIP_TICKER",
    "IDENTITY_NAME_MATCH",
    "MAPPED",
    "MAPPING_AVAILABILITY",
    "SI_AMBIGUOUS",
    "SI_MAPPED",
    "SI_NO_LINE",
    "UNMAPPED_REASONS",
    "issuer_name_key_sql",
    "stage_13f_cusip_periods",
    "stage_cusip_owner_map",
    "stage_si_line_map",
    "symbol_key_sql",
]
