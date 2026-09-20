from __future__ import annotations

import contextlib
import datetime as dt
import hashlib
import json
import logging
import re
import time
import zipfile
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any

import pandas as pd

from ._companyfacts_resume import reopen_companyfacts_store, verify_companyfacts_resume
from .clock import resolve_as_of_date
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .fundamental_statements import (
    default_companyfacts_concepts,
    refresh_fundamental_periods,
    refresh_fundamental_statement_points,
    refresh_fundamental_ttm_points,
)
from .identifier_resolution import candidate_id_for
from .security_master import (
    SEC_USER_AGENT,
    sec_session,
    security_ids_for_symbols,
)
from .warehouse import cik_security_id, insert_frame, json_dumps, quality_check, record_source_file, symbol_key

SEC_COMPANY_FACTS_URL = "https://data.sec.gov/api/xbrl/companyfacts/CIK{cik}.json"
SEC_COMPANY_FACTS_ZIP_URL = "https://www.sec.gov/Archives/edgar/daily-index/xbrl/companyfacts.zip"
SOURCE_NAME = "SEC companyfacts"
# PF-S5 S5-3: match_method tag for resolution-ledger candidates written when a
# sec_company_facts CIK does not resolve to a security_id/entity_id through the
# S5-0 spine as of the fact's own available_at. Mirrors identifiers_figi.py's
# unmatched-cusip "proposed" routing -- the fact still loads (keeping its
# best-effort passthrough security_id, entity_id left NULL); it is never
# silently dropped.
FACT_IDENTIFIER_MATCH_METHOD = "companyfacts_cik_spine_asof"
CANONICAL_CONCEPTS = default_companyfacts_concepts()
# S3-0: the default fetch filter is derived from the active, loadable statement-map
# projection committed as db/seeds/concept_map.csv. Widening this tuple is inert until
# an operator re-runs companyfacts over the loaded universe (for example
# symbol_source='loaded_facts', optionally backed by a local companyfacts_zip); pytest
# only exercises the offline projection and never performs that re-fetch.
DEFAULT_CONCEPTS = CANONICAL_CONCEPTS
# Only taxonomies the statement map understands are loaded. The fundamentals pipeline
# is us-gaap (+ dei cover-page) based; IFRS foreign private issuers report under
# ``ifrs-full`` with no us-gaap map, which both leaves catalog concepts unmapped and
# collides on canonical metric names (e.g. ifrs-full Assets vs us-gaap Assets) — so
# non-supported taxonomies are dropped at load.
SUPPORTED_FACT_TAXONOMIES = ("us-gaap", "dei")
COMPANY_FACT_SYMBOL_SOURCES = ("symbols", "universe", "sec_company_tickers", "loaded_facts", "archive_members")
COMPANYFACTS_MEMBER_PATTERN = re.compile(r"CIK([0-9]{10})\.json")
# This is a source issuer identity, never a security-master/bar identifier.
# SEC-CIK-* is already used by actual traded securities and cannot be a fallback.
UNRESOLVED_COMPANYFACTS_CIK_PREFIX = "SEC-COMPANYFACTS-UNRESOLVED-CIK-"
LOGGER = logging.getLogger(__name__)
# Ten issuer commits keep retained PK/index state far below the 3,750-issuer
# COMMIT failure. Recycling happens only after all per-issuer work is committed.
_COMPANYFACTS_REOPEN_TARGETS = 10


@dataclass(frozen=True)
class SecCompanyFactsOptions:
    symbols: tuple[str, ...] = ("AAPL",)
    concepts: tuple[str, ...] = DEFAULT_CONCEPTS
    symbol_source: str = "symbols"
    symbol_limit: int | None = None
    symbol_offset: int = 0
    # Append-missing only: any fact row is NOT completion evidence for this archive/allowlist.
    skip_loaded_targets: bool = False
    universe_id: str | None = None
    as_of_date: dt.date | None = None
    request_timeout: int = 120
    user_agent: str = SEC_USER_AGENT
    # Resilience knobs for large backfills (defaults preserve single-shot behavior):
    # skip_failed_targets keeps going past a CIK that 404s/times out; request_delay_seconds
    # throttles to stay polite under SEC's ~10 req/s; max_attempts retries transient errors.
    skip_failed_targets: bool = False
    request_delay_seconds: float = 0.0
    max_attempts: int = 1
    # S45: when set, backfill reads companyfacts JSON from the local SEC bulk archive
    # (companyfacts.zip) instead of the per-CIK network endpoint — one download replaces
    # N throttled round trips. The zip is a one-time operator download; never fetched in tests.
    companyfacts_zip: Path | None = None
    # Large backfills can commit raw facts in bounded resumable batches and rebuild
    # the global catalog/revision/statement/period surfaces once after all batches.
    refresh_derived_surfaces: bool = True
    progress_every_targets: int = 0
    # Verified full-archive resume from a failed or source-incomplete dataset UUID.
    resume_from_run_id: str | None = None
    run_id: str | None = None


def ciks_for_symbols(store: DuckDBStore, symbols: tuple[str, ...]) -> list[tuple[str, str, str]]:
    normalized = sorted({symbol_key(symbol) for symbol in symbols})
    if not normalized:
        return []
    frame = pd.DataFrame({"ticker": normalized})
    store.con.register("companyfacts_symbol_lookup", frame)
    try:
        rows = store.con.execute(
            """
            SELECT l.ticker, t.cik, t.security_id
            FROM companyfacts_symbol_lookup l
            JOIN sec_company_tickers t ON t.ticker = l.ticker
            QUALIFY row_number() OVER (
                PARTITION BY l.ticker
                ORDER BY t.source_loaded_at DESC, t.cik
            ) = 1
            """
        ).fetchall()
    finally:
        store.con.unregister("companyfacts_symbol_lookup")
    return [(ticker, cik, security_id) for ticker, cik, security_id in rows]


def _apply_limit(rows: list[tuple[str, str, str]], limit: int | None) -> list[tuple[str, str, str]]:
    if limit is None:
        return rows
    if limit < 1:
        raise ValueError("symbol_limit must be positive when provided")
    return rows[:limit]


def ciks_for_universe(
    store: DuckDBStore,
    *,
    universe_id: str | None = None,
    as_of_date: dt.date | None = None,
    limit: int | None = None,
) -> list[tuple[str, str, str]]:
    """Resolve one representative SEC companyfacts target per universe security."""

    conditions: list[str] = []
    params: list[Any] = []
    if universe_id:
        conditions.append("u.universe_id = ?")
        params.append(universe_id)
    if as_of_date:
        conditions.append("u.as_of_date <= ?")
        params.append(as_of_date)
    where_clause = f"WHERE {' AND '.join(conditions)}" if conditions else ""
    limit_clause = "LIMIT ?" if limit is not None else ""
    if limit is not None:
        if limit < 1:
            raise ValueError("symbol_limit must be positive when provided")
        params.append(limit)

    rows = store.con.execute(
        f"""
        WITH latest_memberships AS (
            SELECT
                u.security_id,
                max(u.as_of_date) AS latest_as_of_date
            FROM universe_memberships u
            {where_clause}
            GROUP BY u.security_id
        ),
        candidate_targets AS (
            SELECT
                t.ticker,
                t.cik,
                m.security_id,
                nullif(s.primary_symbol, '') AS primary_symbol,
                max(CASE WHEN l.source LIKE 'tbltickerhistory%' THEN 1 ELSE 0 END) AS has_history_listing
            FROM latest_memberships m
            JOIN sec_company_tickers t
              ON t.security_id = m.security_id
            LEFT JOIN v_security_master_current s
              ON s.security_id = m.security_id
            LEFT JOIN exchange_listings l
              ON l.security_id = m.security_id
             AND l.ticker = t.ticker
            WHERE t.ticker IS NOT NULL
              AND t.ticker <> ''
              AND t.cik IS NOT NULL
              AND t.cik <> ''
            GROUP BY t.ticker, t.cik, m.security_id, s.primary_symbol
        ),
        ranked AS (
            SELECT
                ticker,
                cik,
                security_id,
                row_number() OVER (
                    PARTITION BY security_id
                    ORDER BY
                        has_history_listing DESC,
                        CASE WHEN ticker = primary_symbol THEN 1 ELSE 0 END DESC,
                        CASE WHEN strpos(ticker, '-') = 0 THEN 1 ELSE 0 END DESC,
                        length(ticker),
                        ticker
                ) AS rn
            FROM candidate_targets
        )
        SELECT ticker, cik, security_id
        FROM ranked
        WHERE rn = 1
        ORDER BY ticker, cik, security_id
        {limit_clause}
        """,
        params,
    ).fetchall()
    return [(ticker, cik, security_id) for ticker, cik, security_id in rows]


def ciks_from_sec_company_tickers(store: DuckDBStore, *, limit: int | None = None) -> list[tuple[str, str, str]]:
    """Resolve one representative ticker per SEC CIK-backed security."""

    params: list[Any] = []
    limit_clause = "LIMIT ?" if limit is not None else ""
    if limit is not None:
        if limit < 1:
            raise ValueError("symbol_limit must be positive when provided")
        params.append(limit)
    rows = store.con.execute(
        f"""
        WITH candidate_targets AS (
            SELECT
                t.ticker,
                t.cik,
                t.security_id,
                max(t.source_loaded_at) AS latest_source_loaded_at,
                max(CASE WHEN l.source LIKE 'tbltickerhistory%' THEN 1 ELSE 0 END) AS has_history_listing
            FROM sec_company_tickers t
            LEFT JOIN exchange_listings l
              ON l.security_id = t.security_id
             AND l.ticker = t.ticker
            WHERE t.ticker IS NOT NULL
              AND t.ticker <> ''
              AND t.cik IS NOT NULL
              AND t.cik <> ''
              AND t.security_id IS NOT NULL
              AND t.security_id <> ''
            GROUP BY t.ticker, t.cik, t.security_id
        ),
        ranked AS (
            SELECT
                ticker,
                cik,
                security_id,
                row_number() OVER (
                    PARTITION BY security_id
                    ORDER BY
                        has_history_listing DESC,
                        latest_source_loaded_at DESC,
                        CASE WHEN strpos(ticker, '-') = 0 THEN 1 ELSE 0 END DESC,
                        length(ticker),
                        ticker
                ) AS rn
            FROM candidate_targets
        )
        SELECT ticker, cik, security_id
        FROM ranked
        WHERE rn = 1
        ORDER BY ticker, cik, security_id
        {limit_clause}
        """,
        params,
    ).fetchall()
    return [(ticker, cik, security_id) for ticker, cik, security_id in rows]


def ciks_from_loaded_facts(store: DuckDBStore, *, limit: int | None = None) -> list[tuple[str, str, str]]:
    """Resolve targets for every security already present in ``sec_company_facts``.

    Use to re-fetch the loaded fundamentals universe verbatim — e.g. after widening
    the concept set — without re-deriving it from tickers/universe screens. The set is
    taken exactly from what was previously loaded, so the refresh is deterministic and
    scope-stable across runs even as the universe/ticker tables drift.
    """
    params: list[Any] = []
    limit_clause = "LIMIT ?" if limit is not None else ""
    if limit is not None:
        if limit < 1:
            raise ValueError("symbol_limit must be positive when provided")
        params.append(limit)
    rows = store.con.execute(
        f"""
        WITH loaded AS (
            SELECT DISTINCT security_id, cik
            FROM sec_company_facts
            WHERE security_id IS NOT NULL AND security_id <> ''
              AND cik IS NOT NULL AND cik <> ''
        ),
        ranked AS (
            SELECT
                coalesce(nullif(t.ticker, ''), nullif(s.primary_symbol, ''), l.cik) AS ticker,
                l.cik,
                l.security_id,
                row_number() OVER (
                    PARTITION BY l.security_id
                    ORDER BY
                        CASE WHEN t.ticker IS NOT NULL AND t.ticker <> '' THEN 0 ELSE 1 END,
                        CASE WHEN strpos(coalesce(t.ticker, ''), '-') = 0 THEN 0 ELSE 1 END,
                        length(coalesce(t.ticker, '')),
                        t.ticker
                ) AS rn
            FROM loaded l
            LEFT JOIN sec_company_tickers t
              ON t.security_id = l.security_id AND t.cik = l.cik
            LEFT JOIN v_security_master_current s
              ON s.security_id = l.security_id
        )
        SELECT ticker, cik, security_id
        FROM ranked
        WHERE rn = 1
        ORDER BY ticker, cik, security_id
        {limit_clause}
        """,
        params,
    ).fetchall()
    return [(ticker, cik, security_id) for ticker, cik, security_id in rows]


def resolve_companyfacts_targets(
    store: DuckDBStore,
    options: SecCompanyFactsOptions,
) -> list[tuple[str, str, str]]:
    if options.symbol_offset < 0:
        raise ValueError("symbol_offset cannot be negative")
    if options.symbol_limit is not None and options.symbol_limit < 1:
        raise ValueError("symbol_limit must be positive when provided")
    # Derive and deduplicate by CIK before paging, including dual-class ticker aliases.
    source = options.symbol_source.lower()
    if source == "symbols":
        rows = ciks_for_symbols(store, options.symbols)
    elif source == "universe":
        rows = ciks_for_universe(
            store,
            universe_id=options.universe_id,
            as_of_date=options.as_of_date,
        )
    elif source == "sec_company_tickers":
        rows = ciks_from_sec_company_tickers(store)
    elif source == "loaded_facts":
        rows = ciks_from_loaded_facts(store)
    elif source == "archive_members":
        if options.companyfacts_zip is None:
            raise ValueError("archive_members requires a local companyfacts_zip; HTTP fallback is disabled")
        with zipfile.ZipFile(options.companyfacts_zip) as archive:
            ciks = sorted({
                match.group(1) for name in archive.namelist()
                if (match := COMPANYFACTS_MEMBER_PATTERN.fullmatch(name)) is not None
            })
        rows = [(f"CIK{cik}", cik, f"{UNRESOLVED_COMPANYFACTS_CIK_PREFIX}{cik}") for cik in ciks]
    else:
        raise ValueError(
            f"Unsupported SEC companyfacts symbol_source {options.symbol_source!r}; "
            f"expected one of {', '.join(COMPANY_FACT_SYMBOL_SOURCES)}"
        )
    by_cik: dict[str, tuple[str, str, str]] = {}
    for symbol, raw_cik, security_id in sorted(rows):
        cik = f"{int(str(raw_cik).strip()):010d}"
        by_cik.setdefault(cik, (symbol, cik, security_id))
    rows = list(by_cik.values())
    if options.skip_loaded_targets:
        loaded_ciks = {
            f"{int(str(row[0]).strip()):010d}"
            for row in store.con.execute(
                "SELECT DISTINCT cik FROM sec_company_facts WHERE cik IS NOT NULL AND cik <> ''"
            ).fetchall()
        }
        rows = [row for row in rows if str(row[1]) not in loaded_ciks]
    start = options.symbol_offset
    stop = None if options.symbol_limit is None else start + options.symbol_limit
    return rows[start:stop]


def _date(value: Any) -> dt.date | None:
    if not value:
        return None
    return dt.date.fromisoformat(str(value))


def resolve_company_facts_identifiers(
    store: DuckDBStore, facts: pd.DataFrame, *, allow_current_fallback: bool = True
) -> tuple[pd.DataFrame, pd.DataFrame]:
    """PF-S5 S5-3: resolve security_id/entity_id per fact through the S5-0 spine.

    Archive ingestion sets ``allow_current_fallback=False``: undated current
    ticker/security rows cannot establish a historical security or entity link.

    ``facts`` must have ``cik``, ``security_id`` (the loader's existing
    best-effort passthrough, used as a stable fallback), and ``available_at``
    columns. Every fact is resolved independently AT ITS OWN available_at via
    ``security_and_entity_ids_for_ciks_asof`` -- two facts for the same CIK filed
    years apart can legitimately resolve through different identifier state, and
    a fact must never resolve through spine knowledge the warehouse did not yet
    have at that fact's own filing availability (no lookahead).

    Returns ``(resolved, unresolved)``:
      - ``resolved`` is ``facts`` with an ``entity_id`` column added (and
        ``security_id`` overwritten with the spine-resolved value where the CIK
        resolves at that row's available_at). Rows whose CIK does not resolve at
        all keep their original passthrough ``security_id`` and get
        ``entity_id = NaN`` -- the fact is never dropped.
      - ``unresolved`` has one row per DISTINCT cik that failed to resolve at
        any of its available_at buckets, for the caller to route into
        ``identifier_resolution_candidates``.
    """
    if facts is None or facts.empty:
        return facts, pd.DataFrame(columns=["cik", "security_id", "available_at"])

    out = facts.copy()
    if "entity_id" not in out.columns:
        out["entity_id"] = pd.NA

    out["__cik_key"] = out["cik"].astype("string").str.strip()
    out["__available_key"] = pd.to_datetime(out["available_at"], errors="coerce")
    valid = out["__cik_key"].notna() & out["__available_key"].notna() & out["__cik_key"].ne("")
    lookup = (
        out.loc[valid, ["__cik_key", "__available_key"]]
        .drop_duplicates()
        .reset_index(drop=True)
        .rename(columns={"__cik_key": "cik", "__available_key": "available_at"})
    )
    lookup["resolution_id"] = range(len(lookup))
    resolved_by_key: dict[tuple[str, pd.Timestamp], tuple[str, str | None]] = {}
    if not lookup.empty:
        store.con.register("companyfacts_resolution_lookup", lookup)
        try:
            resolved_rows = store.con.execute(
                """
                WITH cik_history_presence AS (
                    SELECT DISTINCT id_value AS cik
                    FROM security_identifier_history WHERE id_type='CIK'
                ),
                security_candidates AS (
                    SELECT
                        l.resolution_id,l.cik,l.available_at,h.security_id,
                        1 AS priority,h.available_at AS mapping_available_at,
                        h.valid_from,h.source_loaded_at
                    FROM companyfacts_resolution_lookup l
                    JOIN security_identifier_history h
                      ON h.id_type='CIK' AND h.id_value=l.cik
                     AND h.valid_from<=CAST(l.available_at AS DATE)
                     AND coalesce(h.valid_to,DATE '9999-12-31')>CAST(l.available_at AS DATE)
                     AND (h.available_at IS NULL OR h.available_at<=l.available_at)
                    UNION ALL
                    SELECT
                        l.resolution_id,l.cik,l.available_at,t.security_id,
                        2 AS priority,NULL AS mapping_available_at,
                        DATE '1900-01-01' AS valid_from,t.source_loaded_at
                    FROM companyfacts_resolution_lookup l
                    JOIN sec_company_tickers t ON t.cik=l.cik
                    LEFT JOIN cik_history_presence hp ON hp.cik=l.cik
                    WHERE hp.cik IS NULL
                      AND ?
                      AND t.security_id IS NOT NULL AND t.security_id<>''
                ),
                resolved_security AS (
                    SELECT resolution_id,cik,available_at,security_id
                    FROM security_candidates
                    QUALIFY row_number() OVER (
                        PARTITION BY resolution_id
                        ORDER BY priority,mapping_available_at DESC NULLS LAST,
                                 valid_from DESC,source_loaded_at DESC NULLS LAST,security_id
                    )=1
                ),
                entity_history_presence AS (
                    SELECT DISTINCT security_id
                    FROM security_identifier_history WHERE id_type='ENTITY_ID'
                ),
                entity_candidates AS (
                    SELECT
                        r.resolution_id,h.id_value AS entity_id,
                        1 AS priority,h.available_at AS mapping_available_at,
                        h.valid_from,h.source_loaded_at
                    FROM resolved_security r
                    JOIN security_identifier_history h
                      ON h.security_id=r.security_id AND h.id_type='ENTITY_ID'
                     AND h.valid_from<=CAST(r.available_at AS DATE)
                     AND coalesce(h.valid_to,DATE '9999-12-31')>CAST(r.available_at AS DATE)
                     AND (h.available_at IS NULL OR h.available_at<=r.available_at)
                    UNION ALL
                    SELECT
                        r.resolution_id,s.entity_id,2 AS priority,NULL AS mapping_available_at,
                        coalesce(s.first_seen_date,DATE '1900-01-01') AS valid_from,
                        s.source_loaded_at
                    FROM resolved_security r
                    JOIN securities s ON s.security_id=r.security_id
                    LEFT JOIN entity_history_presence hp ON hp.security_id=r.security_id
                    WHERE hp.security_id IS NULL
                      AND ?
                      AND s.entity_id IS NOT NULL AND s.entity_id<>''
                ),
                resolved_entity AS (
                    SELECT resolution_id,entity_id
                    FROM entity_candidates
                    QUALIFY row_number() OVER (
                        PARTITION BY resolution_id
                        ORDER BY priority,mapping_available_at DESC NULLS LAST,
                                 valid_from DESC,source_loaded_at DESC NULLS LAST,entity_id
                    )=1
                )
                SELECT r.cik,r.available_at,r.security_id,e.entity_id
                FROM resolved_security r
                LEFT JOIN resolved_entity e USING (resolution_id)
                """,
                [allow_current_fallback, allow_current_fallback],
            ).fetchall()
        finally:
            store.con.unregister("companyfacts_resolution_lookup")
        resolved_by_key = {
            (str(cik), pd.Timestamp(available_at)): (security_id, entity_id)
            for cik, available_at, security_id, entity_id in resolved_rows
        }

    unresolved_rows: list[dict[str, Any]] = []
    for idx in out.index:
        cik = str(out.at[idx, "__cik_key"]).strip()
        available_at = out.at[idx, "__available_key"]
        match = None if pd.isna(available_at) else resolved_by_key.get((cik, pd.Timestamp(str(available_at))))
        if match is None:
            unresolved_rows.append(
                {"cik": cik, "security_id": out.at[idx, "security_id"], "available_at": available_at,
                 "security_unresolved_count": 1, "entity_unresolved_count": 1}
            )
            continue
        out.at[idx, "security_id"], out.at[idx, "entity_id"] = match
        if not allow_current_fallback and match[1] is None:
            unresolved_rows.append(
                {"cik": cik, "security_id": match[0], "available_at": available_at,
                 "security_unresolved_count": 0, "entity_unresolved_count": 1}
            )
    out = out.drop(columns=["__cik_key", "__available_key"])

    if unresolved_rows:
        unresolved = pd.DataFrame(unresolved_rows)
        unresolved["fact_count"] = unresolved.groupby("cik")["cik"].transform("size")
        for column in ("security_unresolved_count", "entity_unresolved_count"):
            unresolved[column] = unresolved.groupby("cik")[column].transform("sum")
        unresolved = unresolved.drop_duplicates(subset=["cik"]).reset_index(drop=True)
    else:
        unresolved = pd.DataFrame(columns=["cik", "security_id", "available_at"])
    return out, unresolved


def _unresolved_cik_candidates(
    unresolved: pd.DataFrame,
    *,
    run_id: str | None,
    as_of_date: dt.date,
) -> pd.DataFrame:
    """Build identifier_resolution_candidates rows for unresolved companyfacts CIKs.

    Mirrors identifiers_figi.py's unmatched-cusip routing: status ``proposed``
    (a no-match problem, not a conflict among known entities), with a real
    ``source_security_id``/``target_security_id`` -- the fact's own best-effort
    passthrough security_id -- so the candidate is still auditable even though
    no spine entity was found for it.
    """
    if unresolved is None or unresolved.empty:
        return pd.DataFrame()
    available_at = dt.datetime.combine(as_of_date, dt.time(22))
    rows: list[dict[str, Any]] = []
    for row in unresolved.itertuples(index=False):
        security_id = str(row.security_id)
        rows.append(
            {
                "candidate_id": candidate_id_for(
                    source_dataset_id="sec_company_facts",
                    source_period=None,
                    source_key_type="CIK",
                    source_key_value=str(row.cik),
                    target_security_id=security_id,
                    match_method=FACT_IDENTIFIER_MATCH_METHOD,
                ),
                "source_dataset_id": "sec_company_facts",
                "source_table": "sec_company_facts",
                "source_period": None,
                "source_key_type": "CIK",
                "source_key_value": row.cik,
                "source_security_id": security_id,
                "source_name": None,
                "source_normalized_name": None,
                "target_security_id": security_id,
                "target_id_type": "CIK",
                "target_id_value": row.cik,
                "target_name": None,
                "target_normalized_name": None,
                "match_method": FACT_IDENTIFIER_MATCH_METHOD,
                "confidence": 0.0,
                "candidate_status": "proposed",
                "as_of_date": as_of_date,
                "available_at": available_at,
                "details_json": json_dumps({
                    "status_reason": "unresolved_cik_or_entity_at_fact_available_at",
                    "unresolved_security_fact_rows": row.security_unresolved_count,
                    "unresolved_entity_fact_rows": row.entity_unresolved_count,
                    "fact_available_at": str(row.available_at),
                }),
                "run_id": run_id,
            }
        )
    return pd.DataFrame(rows)


def _companyfacts_candidates(unresolved: pd.DataFrame, options: SecCompanyFactsOptions) -> pd.DataFrame:
    if unresolved.empty:
        return pd.DataFrame()
    return _unresolved_cik_candidates(
        unresolved, run_id=options.run_id,
        as_of_date=resolve_as_of_date(
            options.as_of_date,
            source_max_date=pd.to_datetime(unresolved["available_at"]).max().date(),
        ),
    )


def _replace_companyfacts_candidates(store: DuckDBStore, cik: str, candidates: pd.DataFrame) -> int:
    """Caller owns the issuer transaction; no relation survives this function."""
    store.con.execute(
        """DELETE FROM identifier_resolution_candidates
        WHERE source_dataset_id = 'sec_company_facts' AND match_method = ?
          AND source_key_type = 'CIK' AND regexp_full_match(trim(source_key_value), '[0-9]+')
          AND try_cast(source_key_value AS BIGINT) = cast(? AS BIGINT)""",
        [FACT_IDENTIFIER_MATCH_METHOD, cik],
    )
    return insert_frame(store, candidates, "identifier_resolution_candidates",
                        "sec_company_facts_unresolved_candidates_insert")


def _validate_archive_payload_cik(payload: dict[str, Any], cik: str) -> str:
    """Only an absent field can borrow an exact validated archive member CIK."""
    if "cik" not in payload:
        return "validated_archive_member_missing_payload_cik"
    raw_cik = payload["cik"]
    if (isinstance(raw_cik, bool) or not isinstance(raw_cik, (str, int))
            or re.fullmatch(r"[0-9]{1,10}", str(raw_cik)) is None):
        raise ValueError("companyfacts payload CIK is invalid")
    if f"{int(raw_cik):010d}" != cik:
        raise ValueError("payload CIK does not match archive member CIK")
    return "payload_cik_matches_archive_member"


def normalize_companyfacts(
    payload: dict[str, Any],
    *,
    symbol: str,
    security_id: str,
    cik: str,
    source_url: str,
    concepts: set[str],
    run_id: str | None,
) -> tuple[pd.DataFrame, pd.DataFrame]:
    facts = payload.get("facts", {})
    rows: list[dict[str, Any]] = []
    points: list[dict[str, Any]] = []
    for taxonomy, taxonomy_facts in facts.items():
        if taxonomy not in SUPPORTED_FACT_TAXONOMIES:
            continue
        if not isinstance(taxonomy_facts, dict):
            raise ValueError(f"{taxonomy} facts must be an object")
        for concept, concept_payload in taxonomy_facts.items():
            if concepts and concept not in concepts:
                continue
            if not isinstance(concept_payload, dict) or not isinstance(concept_payload.get("units"), dict):
                raise ValueError(f"{taxonomy}:{concept} requires a units object")
            label = concept_payload.get("label")
            description = concept_payload.get("description")
            units = concept_payload["units"]
            for unit, unit_rows in units.items():
                if not isinstance(unit_rows, list):
                    raise ValueError(f"{taxonomy}:{concept} unit {unit!r} requires a fact list")
                for item in unit_rows:
                    if not isinstance(item, dict):
                        raise ValueError(f"{taxonomy}:{concept} unit {unit!r} contains a non-object fact")
                    end_date = _date(item.get("end"))
                    filed_date = _date(item.get("filed"))
                    if end_date is None or filed_date is None:
                        continue
                    if end_date > filed_date:
                        continue
                    row = {
                        "source": SOURCE_NAME,
                        "security_id": security_id,
                        "cik": cik,
                        "taxonomy": taxonomy,
                        "concept": concept,
                        "label": label,
                        "description": description,
                        "unit": unit,
                        "period_start": _date(item.get("start")),
                        "period_end": end_date,
                        "filed_date": filed_date,
                        "fiscal_year": item.get("fy"),
                        "fiscal_period": item.get("fp"),
                        "form": item.get("form"),
                        "accession_number": item.get("accn"),
                        "frame": item.get("frame"),
                        "value": item.get("val"),
                        "available_at": pd.Timestamp(filed_date) + pd.Timedelta(hours=22),
                        "run_id": run_id,
                        "source_url": source_url,
                    }
                    rows.append(row)
                    points.append(
                        {
                            "source": SOURCE_NAME,
                            "security_id": security_id,
                            "symbol": symbol,
                            "metric": concept,
                            "taxonomy": taxonomy,
                            "unit": unit,
                            "period_start": row["period_start"],
                            "period_end": row["period_end"],
                            "as_of_date": filed_date,
                            "fiscal_year": row["fiscal_year"],
                            "fiscal_period": row["fiscal_period"],
                            "form": row["form"],
                            "accession_number": row["accession_number"],
                            "value": row["value"],
                            "available_at": row["available_at"],
                            "run_id": run_id,
                        }
                    )
    facts_frame = pd.DataFrame(rows)
    points_frame = pd.DataFrame(points)
    for frame in (facts_frame, points_frame):
        if not frame.empty and "value" in frame.columns:
            frame["value"] = pd.to_numeric(frame["value"], errors="coerce")
    return facts_frame, points_frame


def refresh_xbrl_concept_catalog(store: DuckDBStore) -> int:
    """Refresh concept-level metadata from loaded SEC companyfacts."""
    from .xbrl_catalog import refresh_concept_catalog

    return refresh_concept_catalog(store)


def refresh_fundamental_fact_revisions(
    store: DuckDBStore,
    concepts: tuple[str, ...] | None = None,
) -> int:
    """Refresh accession-level revision chains, optionally for selected concepts."""

    selected = tuple(sorted({str(concept) for concept in concepts or () if concept}))
    registered = False
    concept_predicate = ""
    if selected:
        store.con.register(
            "fundamental_revision_concept_filter",
            pd.DataFrame({"concept": selected}),
        )
        registered = True
        concept_predicate = (
            "AND f.concept IN ("
            "SELECT concept FROM fundamental_revision_concept_filter)"
        )
    try:
        with store.transaction():
            if selected:
                store.con.execute(
                    """
                    DELETE FROM fundamental_fact_revisions
                    WHERE concept IN (
                        SELECT concept FROM fundamental_revision_concept_filter
                    )
                    """
                )
            else:
                store.con.execute("DELETE FROM fundamental_fact_revisions")
            store.con.execute(
                f"""
            INSERT INTO fundamental_fact_revisions (
                fact_revision_id,
                revision_group_id,
                source,
                security_id,
                cik,
                taxonomy,
                concept,
                unit,
                period_start,
                period_end,
                accession_number,
                filed_date,
                available_at,
                form,
                fiscal_year,
                fiscal_period,
                frame,
                value,
                revision_sequence,
                revision_count,
                is_latest_revision,
                is_value_changed,
                previous_accession_number,
                previous_filed_date,
                previous_available_at,
                previous_value,
                value_delta,
                value_delta_percent,
                first_filed_date,
                latest_filed_date,
                first_available_at,
                latest_available_at,
                run_id,
                source_url,
                source_loaded_at
            )
            WITH base AS (
                SELECT
                    sha256(
                        concat_ws(
                            '|',
                            source,
                            security_id,
                            taxonomy,
                            concept,
                            unit,
                            coalesce(CAST(period_start AS VARCHAR), ''),
                            CAST(period_end AS VARCHAR)
                        )
                    ) AS revision_group_id,
                    sha256(
                        concat_ws(
                            '|',
                            source,
                            security_id,
                            taxonomy,
                            concept,
                            unit,
                            coalesce(CAST(period_start AS VARCHAR), ''),
                            CAST(period_end AS VARCHAR),
                            accession_number
                        )
                    ) AS fact_revision_id,
                    source,
                    security_id,
                    cik,
                    taxonomy,
                    concept,
                    unit,
                    period_start,
                    period_end,
                    accession_number,
                    filed_date,
                    available_at,
                    form,
                    fiscal_year,
                    fiscal_period,
                    frame,
                    value,
                    run_id,
                    source_url,
                    source_loaded_at
                FROM sec_company_facts f
                WHERE source IS NOT NULL
                  AND source <> ''
                  AND security_id IS NOT NULL
                  AND security_id <> ''
                  AND taxonomy IS NOT NULL
                  AND taxonomy <> ''
                  AND concept IS NOT NULL
                  AND concept <> ''
                  AND unit IS NOT NULL
                  AND unit <> ''
                  AND period_end IS NOT NULL
                  AND accession_number IS NOT NULL
                  AND accession_number <> ''
                  AND filed_date IS NOT NULL
                  {concept_predicate}
            ),
            sequenced AS (
                SELECT
                    base.*,
                    row_number() OVER fact_window AS revision_sequence,
                    count(*) OVER fact_window AS revision_count,
                    lag(accession_number) OVER fact_window AS previous_accession_number,
                    lag(filed_date) OVER fact_window AS previous_filed_date,
                    lag(available_at) OVER fact_window AS previous_available_at,
                    lag(value) OVER fact_window AS previous_value,
                    min(filed_date) OVER fact_window AS first_filed_date,
                    max(filed_date) OVER fact_window AS latest_filed_date,
                    min(available_at) OVER fact_window AS first_available_at,
                    max(available_at) OVER fact_window AS latest_available_at
                FROM base
                WINDOW fact_window AS (
                    PARTITION BY revision_group_id
                    ORDER BY
                        coalesce(available_at, CAST(filed_date AS TIMESTAMP)),
                        filed_date,
                        coalesce(source_loaded_at, TIMESTAMP '1970-01-01'),
                        accession_number
                    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
                )
            )
            SELECT
                fact_revision_id,
                revision_group_id,
                source,
                security_id,
                cik,
                taxonomy,
                concept,
                unit,
                period_start,
                period_end,
                accession_number,
                filed_date,
                available_at,
                form,
                fiscal_year,
                fiscal_period,
                frame,
                value,
                revision_sequence,
                revision_count,
                revision_sequence = revision_count AS is_latest_revision,
                CASE
                    WHEN revision_sequence = 1 THEN false
                    ELSE value IS DISTINCT FROM previous_value
                END AS is_value_changed,
                previous_accession_number,
                previous_filed_date,
                previous_available_at,
                previous_value,
                CASE
                    WHEN previous_value IS NULL OR value IS NULL THEN NULL
                    ELSE value - previous_value
                END AS value_delta,
                CASE
                    WHEN previous_value IS NULL OR previous_value = 0 OR value IS NULL THEN NULL
                    ELSE (value - previous_value) / abs(previous_value)
                END AS value_delta_percent,
                first_filed_date,
                latest_filed_date,
                first_available_at,
                latest_available_at,
                run_id,
                source_url,
                source_loaded_at
            FROM sequenced
            """
            )
    finally:
        if registered:
            store.con.unregister("fundamental_revision_concept_filter")
    count_row = store.con.execute("SELECT count(*) FROM fundamental_fact_revisions").fetchone()
    assert count_row is not None
    return int(count_row[0])


@dataclass(frozen=True)
class _CompanyFactsArchivePlaceholder:
    """Observed source absence; never authority to replace retained issuer facts."""

    member: str
    byte_count: int


class _CompanyFactsZipFetcher:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self._archive = zipfile.ZipFile(self.path)
        self._names = set(self._archive.namelist())

    def __call__(self, cik: str | int) -> dict[str, Any] | _CompanyFactsArchivePlaceholder | None:
        try:
            member = f"CIK{int(str(cik).strip()):010d}.json"
        except (TypeError, ValueError):
            return None
        if member not in self._names:
            return None
        with self._archive.open(member) as handle:
            raw_payload = handle.read()
        # The inspected SEC bulk archive contains exact two-byte placeholders.
        # Do not generalize this to missing facts, whitespace variants, or HTTP.
        if raw_payload == b"{}" and COMPANYFACTS_MEMBER_PATTERN.fullmatch(member):
            return _CompanyFactsArchivePlaceholder(member=member, byte_count=len(raw_payload))
        payload: object = json.loads(raw_payload)
        if not isinstance(payload, dict):
            raise ValueError("companyfacts payload requires an object")
        return payload

    def close(self) -> None:
        self._archive.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()


def _make_companyfacts_zip_fetcher(path: str | Path) -> _CompanyFactsZipFetcher:
    """Return an OFFLINE fetcher backed by the SEC bulk ``companyfacts.zip``.

    SEC publishes the entire XBRL company-facts universe as a single nightly archive at
    ``https://www.sec.gov/Archives/edgar/daily-index/xbrl/companyfacts.zip`` containing one
    ``CIK##########.json`` per filer — byte-identical JSON to the per-CIK
    ``data.sec.gov/.../companyfacts/CIK*.json`` endpoint. Lookups are lazy (one member read
    per CIK) so the ~1.4 GB archive never loads fully into memory. Download is a one-time
    operator step; this fetcher and all tests run purely against a local file. Returns the
    parsed companyfacts payload (``{"cik", "entityName", "facts": {...}}``) for
    ``normalize_companyfacts`` to consume, an explicit marker for an exact two-byte
    ``{}`` archive placeholder, or ``None`` if the CIK is absent / non-numeric.
    """
    return _CompanyFactsZipFetcher(path)


def _companyfacts_zip_member_url(cik: str | int) -> str:
    return f"{SEC_COMPANY_FACTS_ZIP_URL}#CIK{int(str(cik).strip()):010d}.json"


def _companyfacts_archive_identity(path: Path) -> tuple[int, int, int, int]:
    stat = path.stat()
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


def _companyfacts_cik_spellings(store: DuckDBStore) -> dict[int, tuple[str, ...]]:
    """Inventory exact stored spellings once, using the replacement's SQL rules.

    Only distinct CIK strings enter Python, never the warehouse's fact history.
    Memory scales with spelling cardinality; keep this cache local to one load.
    """
    rows = store.con.execute(
        """
        SELECT cik, try_cast(cik AS BIGINT) AS cik_number
        FROM (SELECT DISTINCT cik FROM sec_company_facts) stored
        WHERE regexp_full_match(trim(cik), '[0-9]+')
          AND try_cast(cik AS BIGINT) IS NOT NULL
        """
    ).fetchall()
    spellings: dict[int, list[str]] = {}
    for raw_cik, cik_number in rows:
        spellings.setdefault(cik_number, []).append(raw_cik)
    return {cik_number: tuple(sorted(values)) for cik_number, values in spellings.items()}


class SecCompanyFactsDataset(Dataset):
    dataset_id = "sec_company_facts"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: SecCompanyFactsOptions) -> DatasetLoadResult:
        archive_mode = options.symbol_source.lower() == "archive_members"
        if options.resume_from_run_id is not None and (
            not archive_mode or options.companyfacts_zip is None or options.symbol_limit is not None
            or options.symbol_offset != 0 or options.skip_loaded_targets
        ):
            raise ValueError("companyfacts resume requires the full archive with replacement enabled")
        # Resolve first: an archive selector without a ZIP must fail before a session opens.
        targets = resolve_companyfacts_targets(store, options)
        all_options = replace(options, skip_loaded_targets=False, symbol_offset=0, symbol_limit=None)
        all_targets = resolve_companyfacts_targets(store, all_options)
        eligible_targets = (
            resolve_companyfacts_targets(store, replace(all_options, skip_loaded_targets=True))
            if options.skip_loaded_targets else all_targets
        )
        if not all_targets and options.symbol_source.lower() == "symbols":
            missing = sorted({symbol_key(symbol) for symbol in options.symbols})
            sec_map = security_ids_for_symbols(store, missing)
            raise RuntimeError(f"No SEC CIK mapping for symbols {missing}; run security master first. Known: {sec_map}")
        target_ciks = [cik for _symbol, cik, _security_id in targets]
        assert len(target_ciks) == len(set(target_ciks)), "resolve_companyfacts_targets returned duplicate CIKs"
        source_mode = "bulk_zip" if options.companyfacts_zip is not None else "api"
        source_name = "SEC companyfacts bulk zip" if options.companyfacts_zip is not None else "SEC companyfacts API"
        fingerprint = hashlib.sha256(json_dumps({
            "concepts": sorted(set(options.concepts)), "taxonomies": SUPPORTED_FACT_TAXONOMIES,
        }).encode()).hexdigest()
        details: dict[str, Any] = {
            "symbols": options.symbols,
            "symbol_source": options.symbol_source,
            "symbol_limit": options.symbol_limit,
            "symbol_offset": options.symbol_offset,
            "skip_loaded_targets": options.skip_loaded_targets,
            "load_policy": "append_missing" if options.skip_loaded_targets else "replace",
            "completion_evidence": "no_persisted_archive_allowlist_receipts",
            "previously_completed_targets": None,
            "allowlist_sha256": fingerprint,
            "archive_sha256": None,
            "archive_member_count": None,
            "archive_cik_member_count": None,
            "archive_duplicate_cik_members": None,
            "archive_ignored_member_count": None,
            "valid_target_count": len(all_targets),
            "skipped_existing_targets": len(all_targets) - len(eligible_targets),
            "window_excluded_targets": len(eligible_targets) - len(targets),
            "target_count": len(targets),
            "refresh_derived_surfaces": options.refresh_derived_surfaces,
            "universe_id": options.universe_id,
            "as_of_date": options.as_of_date.isoformat() if options.as_of_date else None,
            "source_mode": source_mode,
            "companyfacts_zip": str(options.companyfacts_zip) if options.companyfacts_zip else None,
        }
        rows_loaded = point_rows = loaded_targets = completed_targets = 0
        unresolved_fact_rows = unresolved_entity_rows = unresolved_targets = unresolved_entity_targets = 0
        failed_targets: list[dict[str, str]] = []
        failure_types: dict[str, int] = {}
        empty_targets: list[dict[str, str]] = []
        empty_reasons: dict[str, int] = {}
        unavailable_targets: list[dict[str, str]] = []
        unavailable_reasons: dict[str, int] = {}
        unresolved_candidate_rows = recovered_candidate_rows = 0
        connection_reopens = committed_since_reopen = 0
        verified_members = {}
        cik_spellings_by_number: dict[int, tuple[str, ...]] | None = None
        with contextlib.ExitStack() as stack:
            zip_fetcher = None
            session = None
            if options.companyfacts_zip is not None:
                archive_stat = _companyfacts_archive_identity(options.companyfacts_zip)
                zip_fetcher = _make_companyfacts_zip_fetcher(options.companyfacts_zip)
                stack.callback(zip_fetcher.close)
                with options.companyfacts_zip.open("rb") as handle:
                    details["archive_sha256"] = hashlib.file_digest(handle, "sha256").hexdigest()
                if _companyfacts_archive_identity(options.companyfacts_zip) != archive_stat:
                    raise ValueError("companyfacts archive changed while computing its digest")
                names = zip_fetcher._archive.namelist()
                exact_names = [name for name in names if COMPANYFACTS_MEMBER_PATTERN.fullmatch(name)]
                details.update({
                    "archive_member_count": len(names),
                    "archive_cik_member_count": len(set(exact_names)),
                    "archive_duplicate_cik_members": len(exact_names) - len(set(exact_names)),
                    "archive_ignored_member_count": len(names) - len(exact_names),
                })
                if options.resume_from_run_id is not None:
                    LOGGER.info("companyfacts resume verifying receipts and retained fact/point rows")
                    verified_members, resume_details = verify_companyfacts_resume(
                        store, options, archive_sha256=details["archive_sha256"],
                        allowlist_sha256=fingerprint, target_ciks=set(target_ciks),
                        source_url_prefix=SEC_COMPANY_FACTS_ZIP_URL,
                        unresolved_prefix=UNRESOLVED_COMPANYFACTS_CIK_PREFIX,
                        duplicate_members=details["archive_duplicate_cik_members"],
                    )
                    details.update(resume_details)
                    if _companyfacts_archive_identity(options.companyfacts_zip) != archive_stat:
                        raise ValueError("companyfacts archive changed during resume verification")
                    # The proof queries have been fully consumed. Begin mutations
                    # with fresh index state under the same analytical caps.
                    connection_reopens += int(reopen_companyfacts_store(store))
                    LOGGER.info("companyfacts resume verified targets=%d rows=%d",
                                len(verified_members), details["resume_verified_rows"])
            elif targets:
                if options.resume_from_run_id is not None:
                    raise ValueError("companyfacts resume requires a local full archive")
                session = sec_session(options.user_agent)
            effective_skip_failed = options.skip_failed_targets or zip_fetcher is not None
            for index, (symbol, cik, security_id) in enumerate(targets):
                if committed_since_reopen >= _COMPANYFACTS_REOPEN_TARGETS:
                    connection_reopens += int(reopen_companyfacts_store(store))
                    committed_since_reopen = 0
                if options.progress_every_targets > 0 and index % options.progress_every_targets == 0:
                    LOGGER.info("companyfacts processed=%d total=%d loaded=%d empty=%d unavailable=%d failed=%d rows=%d",
                                index, len(targets), loaded_targets, len(empty_targets),
                                len(unavailable_targets), len(failed_targets), rows_loaded)
                if cik in verified_members:
                    member = verified_members[cik]
                    if member.unresolved is not None:
                        recovered = pd.DataFrame([member.unresolved])
                        candidates = _companyfacts_candidates(recovered, options)
                        with store.transaction():
                            recovered_candidate_rows += _replace_companyfacts_candidates(store, cik, candidates)
                        unresolved_candidate_rows += len(candidates)
                        unresolved_fact_rows += member.unresolved["security_unresolved_count"]
                        unresolved_targets += int(member.unresolved["security_unresolved_count"] > 0)
                        unresolved_entity_rows += member.unresolved["entity_unresolved_count"]
                        unresolved_entity_targets += int(member.unresolved["entity_unresolved_count"] > 0)
                        del recovered, candidates
                    else:
                        with store.transaction():
                            _replace_companyfacts_candidates(store, cik, pd.DataFrame())
                    committed_since_reopen += 1
                    loaded_targets += 1
                    completed_targets += 1
                    # Keep the original fact/point owner and source receipt. A
                    # future failed resume can verify the explicitly linked ancestor.
                    continue
                if zip_fetcher is None and options.request_delay_seconds > 0 and index > 0:
                    time.sleep(options.request_delay_seconds)
                source_url = (
                    _companyfacts_zip_member_url(cik) if zip_fetcher is not None
                    else SEC_COMPANY_FACTS_URL.format(cik=cik)
                )
                # Only source/member/normalization errors are skippable. Database errors remain fatal.
                archive_placeholder = None
                payload_identity = None
                try:
                    if zip_fetcher is not None:
                        payload = zip_fetcher(cik)
                        if payload is None:
                            raise FileNotFoundError(f"CIK {cik} absent from companyfacts.zip")
                        if isinstance(payload, _CompanyFactsArchivePlaceholder):
                            archive_placeholder = payload
                    else:
                        assert session is not None
                        for attempt in range(max(1, options.max_attempts)):
                            try:
                                response = session.get(source_url, timeout=options.request_timeout)
                                response.raise_for_status()
                                payload = response.json()
                                break
                            except Exception:
                                if attempt + 1 == max(1, options.max_attempts):
                                    raise
                                time.sleep(min(2.0 * (attempt + 1), 10.0))
                    if archive_placeholder is None:
                        if not isinstance(payload, dict) or not isinstance(payload.get("facts"), dict):
                            raise ValueError("companyfacts payload requires a facts object")
                        if archive_mode:
                            payload_identity = _validate_archive_payload_cik(payload, cik)
                        facts, points = normalize_companyfacts(
                            payload, symbol=symbol, security_id=security_id, cik=cik,
                            source_url=source_url, concepts=set(options.concepts), run_id=options.run_id,
                        )
                except Exception as exc:
                    error_type = type(exc).__name__
                    failure_types[error_type] = failure_types.get(error_type, 0) + 1
                    failure = {"symbol": symbol, "cik": cik, "error_type": error_type, "error": str(exc)[:200]}
                    record_source_file(store, dataset_id=self.dataset_id, source_url=source_url,
                                       cache_path=options.companyfacts_zip, status="error",
                                       sha256=details["archive_sha256"],
                                       metadata={**failure, "source_mode": source_mode, "run_id": options.run_id,
                                                 "allowlist_sha256": fingerprint})
                    if not effective_skip_failed:
                        raise RuntimeError(f"SEC companyfacts fetch failed for {symbol} (CIK {cik}): {exc}") from exc
                    failed_targets.append(failure)
                    continue
                if archive_placeholder is not None:
                    reason = "empty_archive_placeholder"
                    unavailable_reasons[reason] = unavailable_reasons.get(reason, 0) + 1
                    unavailable_targets.append({"cik": cik, "reason": reason})
                    record_source_file(
                        store, dataset_id=self.dataset_id, source_url=source_url,
                        cache_path=options.companyfacts_zip, status="unavailable",
                        sha256=details["archive_sha256"],
                        metadata={"symbol": symbol, "cik": cik, "source_mode": source_mode,
                                  "run_id": options.run_id, "rows": 0,
                                  "unavailable_reason": reason,
                                  "archive_member": archive_placeholder.member,
                                  "archive_member_bytes": archive_placeholder.byte_count,
                                  "archive_sha256": details["archive_sha256"],
                                  "allowlist_sha256": fingerprint},
                    )
                    # No facts, points, candidate mutations, or completion/loaded counts.
                    continue
                facts, unresolved = resolve_company_facts_identifiers(
                    store, facts, allow_current_fallback=not archive_mode,
                )
                if not unresolved.empty:
                    security_unresolved = int(unresolved["security_unresolved_count"].sum())
                    unresolved_fact_rows += security_unresolved
                    unresolved_targets += int(security_unresolved > 0)
                if not facts.empty:
                    entity_unresolved = int(facts["entity_id"].isna().sum())
                    unresolved_entity_rows += entity_unresolved
                    unresolved_entity_targets += int(entity_unresolved > 0)
                    # Archive points use exactly the same per-fact PIT IDs as raw facts.
                    if archive_mode:
                        points["security_id"] = facts["security_id"]
                        points["symbol"] = None  # CIK source labels are not trading symbols.
                if cik_spellings_by_number is None:
                    inventory_started = time.perf_counter()
                    cik_spellings_by_number = _companyfacts_cik_spellings(store)
                    if options.progress_every_targets > 0:
                        LOGGER.info("companyfacts CIK inventory issuers=%d spellings=%d elapsed_seconds=%.3f",
                                    len(cik_spellings_by_number),
                                    sum(len(values) for values in cik_spellings_by_number.values()),
                                    time.perf_counter() - inventory_started)
                cik_number = int(cik)
                stored_cik_spellings = cik_spellings_by_number.get(cik_number, ())
                reason = None
                if facts.empty:
                    supported = {key: value for key, value in payload["facts"].items()
                                 if key in SUPPORTED_FACT_TAXONOMIES}
                    if not any(supported.values()):
                        reason = "unsupported_or_empty_taxonomy"
                    elif options.concepts and not any(set(group) & set(options.concepts) for group in supported.values()):
                        reason = "allowlist_empty"
                    else:
                        reason = "no_valid_fact_rows"
                    empty_reasons[reason] = empty_reasons.get(reason, 0) + 1
                    empty_targets.append({"cik": cik, "reason": reason})
                else:
                    loaded_targets += 1
                candidates = _companyfacts_candidates(unresolved, options)
                receipt = dict(
                    dataset_id=self.dataset_id, source_url=source_url,
                    cache_path=options.companyfacts_zip, status="empty" if facts.empty else "loaded",
                    sha256=details["archive_sha256"],
                    metadata={"symbol": symbol, "cik": cik, "source_mode": source_mode,
                              "run_id": options.run_id,
                              "rows": len(facts), "empty_reason": reason,
                              "payload_cik_identity": payload_identity,
                              "archive_member": f"CIK{cik}.json" if zip_fetcher is not None else None,
                              "unresolved_security": bool(
                                  not unresolved.empty and unresolved["security_unresolved_count"].sum() > 0),
                              "unresolved_entity": bool(not facts.empty and facts["entity_id"].isna().any()),
                              "archive_sha256": details["archive_sha256"], "allowlist_sha256": fingerprint},
                )
                rows_loaded += self._replace_facts(
                    store, facts, points, security_id, cik=cik,
                    stored_cik_spellings=stored_cik_spellings, candidates=candidates, receipt=receipt,
                )
                committed_since_reopen += 1
                # Advance only after facts, points, candidates and receipt commit.
                if not facts.empty and cik not in stored_cik_spellings:
                    cik_spellings_by_number[cik_number] = (*stored_cik_spellings, cik)
                point_rows += len(points)
                completed_targets += 1
                unresolved_candidate_rows += len(candidates)
                del facts, points, payload, unresolved, candidates, receipt
            if zip_fetcher is not None and _companyfacts_archive_identity(options.companyfacts_zip) != archive_stat:
                raise ValueError("companyfacts archive changed during ingestion")
        if committed_since_reopen:
            connection_reopens += int(reopen_companyfacts_store(store))
        if options.refresh_derived_surfaces:
            concept_rows = refresh_xbrl_concept_catalog(store)
            revision_rows = refresh_fundamental_fact_revisions(store)
            statement_rows = refresh_fundamental_statement_points(store)
            period_rows = refresh_fundamental_periods(store)
            ttm_rows = refresh_fundamental_ttm_points(store)
        else:
            concept_rows = revision_rows = statement_rows = period_rows = ttm_rows = 0
        details.update({
            "completed_targets": completed_targets,
            "replayed_targets": completed_targets - len(verified_members),
            "resumed_loaded_targets": len(verified_members),
            "recovered_unresolved_candidate_rows": recovered_candidate_rows,
            "connection_reopens": connection_reopens,
            "connection_reopen_interval": _COMPANYFACTS_REOPEN_TARGETS,
            "loaded_targets": loaded_targets,
            "empty_target_count": len(empty_targets), "empty_target_reasons": empty_reasons,
            "empty_targets": empty_targets[:50],
            "unavailable_target_count": len(unavailable_targets),
            "unavailable_target_reasons": unavailable_reasons,
            "unavailable_targets": unavailable_targets[:50],
            "coverage_warnings": (["archive_empty_placeholders_preserved_prior_data"]
                                  if unavailable_targets else []),
            "failed_target_count": len(failed_targets), "failed_targets": failed_targets[:50],
            "failure_types": failure_types,
            "unresolved_security_targets": unresolved_targets,
            "unresolved_security_fact_rows": unresolved_fact_rows,
            "unresolved_entity_fact_rows": unresolved_entity_rows,
            "unresolved_entity_targets": unresolved_entity_targets,
            "facts": rows_loaded, "fundamental_points": point_rows,
            "xbrl_concept_catalog": concept_rows, "fundamental_fact_revisions": revision_rows,
            "fundamental_statement_points": statement_rows, "fundamental_periods": period_rows,
            "fundamental_ttm_points": ttm_rows, "unresolved_cik_candidate_rows": unresolved_candidate_rows,
            "outcome": ("failed_targets" if failed_targets else "no_valid_targets" if not all_targets
                        else "all_existing_skipped" if not eligible_targets else "empty_window" if not targets
                        else "loaded_with_unavailable" if unavailable_targets and loaded_targets
                        else "source_unavailable" if unavailable_targets
                        else "supported_facts_empty" if not loaded_targets else "loaded"),
            "listed_security_coverage_verified": False,
            "identity_policy": "dated_history_or_isolated_cik_source" if archive_mode else "legacy_current_fallback",
        })
        if options.progress_every_targets > 0:
            LOGGER.info("companyfacts processed=%d total=%d loaded=%d empty=%d unavailable=%d failed=%d unresolved=%d rows=%d finished=true",
                        len(targets), len(targets), loaded_targets, len(empty_targets),
                        len(unavailable_targets), len(failed_targets),
                        unresolved_targets, rows_loaded)
        if unavailable_targets:
            quality_check(store, dataset_id=self.dataset_id, table_name="sec_company_facts",
                          check_name="source_availability", status="warning",
                          observed_value=float(len(unavailable_targets)), threshold_value=0.0,
                          details=details)
        if failed_targets:
            # Dataset.run records a normal return as succeeded even when the
            # activation stage reports source incompleteness. Persist this
            # run-bound reason before returning so its completed tail can be
            # verified on the next resume, even after error receipts are retried.
            quality_check(
                store, dataset_id=self.dataset_id, table_name="sec_company_facts",
                check_name="source_completeness", status="failed",
                observed_value=float(len(failed_targets)), threshold_value=0.0,
                details={"run_id": options.run_id, "failed_target_count": len(failed_targets),
                         "source_mode": source_mode, "archive_sha256": details["archive_sha256"],
                         "allowlist_sha256": fingerprint},
            )
        quality_check(store, dataset_id=self.dataset_id, table_name="sec_company_facts", check_name="rows_loaded",
                      status="failed" if failed_targets else "passed" if rows_loaded > 0 else "warning",
                      observed_value=float(rows_loaded), threshold_value=1.0, details=details)
        return DatasetLoadResult(dataset_id=self.dataset_id, rows_loaded=rows_loaded, source=source_name, details=details)

    def _replace_facts(
        self,
        store: DuckDBStore,
        facts: pd.DataFrame,
        points: pd.DataFrame,
        security_id: str,
        *,
        cik: str,
        stored_cik_spellings: tuple[str, ...] | None = None,
        candidates: pd.DataFrame | None = None,
        receipt: dict[str, Any] | None = None,
    ) -> int:
        # Independent callers retain fresh numeric matching. The serial load
        # supplies a complete spelling inventory and maintains its own inserts.
        if stored_cik_spellings is None:
            cik_predicate = "regexp_full_match(trim(cik), '[0-9]+') AND try_cast(cik AS BIGINT) = cast(? AS BIGINT)"
            cik_params = [cik]
        elif stored_cik_spellings:
            cik_predicate = f"cik IN ({', '.join('?' for _ in stored_cik_spellings)})"
            cik_params = list(stored_cik_spellings)
        else:
            cik_predicate = "FALSE"
            cik_params = []
        with store.transaction():
            # Points have no CIK. Match both an old issuer identity and its fact
            # keys; filing keys alone may be shared by different issuers. Include
            # legacy loader fallback IDs as well as per-fact PIT-resolved IDs.
            # Materialize issuer-sized relations before joining the full points
            # history: the correlated EXISTS/identity OR exhausts query memory
            # on production history. These temp tables are created and dropped
            # in this transaction, so a failed replacement rolls them back too.
            store.con.execute(
                f"""
                CREATE TEMP TABLE companyfacts_old_fact_keys AS
                SELECT DISTINCT security_id, accession_number, taxonomy, concept, unit, period_end, period_start
                FROM sec_company_facts
                WHERE {cik_predicate}
                """,
                cik_params,
            )
            store.con.execute(
                """
                CREATE TEMP TABLE companyfacts_legacy_ids AS
                SELECT security_id FROM (VALUES (cast(? AS VARCHAR)), (cast(? AS VARCHAR))) AS ids(security_id)
                WHERE security_id IS NOT NULL
                UNION
                SELECT security_id FROM sec_company_tickers
                WHERE regexp_full_match(trim(cik), '[0-9]+')
                  AND try_cast(cik AS BIGINT) = cast(? AS BIGINT)
                  AND security_id IS NOT NULL
                """,
                [security_id, cik_security_id(cik), cik],
            )
            store.con.execute(
                """
                CREATE TEMP TABLE companyfacts_point_delete_keys AS
                SELECT * FROM companyfacts_old_fact_keys WHERE security_id IS NOT NULL
                UNION
                SELECT ids.security_id, f.accession_number, f.taxonomy, f.concept, f.unit, f.period_end, f.period_start
                FROM companyfacts_legacy_ids ids
                CROSS JOIN (
                    SELECT DISTINCT accession_number, taxonomy, concept, unit, period_end, period_start
                    FROM companyfacts_old_fact_keys
                ) f
                """
            )
            store.con.execute(
                """
                DELETE FROM fundamental_points p USING companyfacts_point_delete_keys k
                WHERE p.source = ? AND p.security_id = k.security_id
                  AND p.accession_number IS NOT DISTINCT FROM k.accession_number
                  AND p.taxonomy IS NOT DISTINCT FROM k.taxonomy
                  AND p.metric IS NOT DISTINCT FROM k.concept
                  AND p.unit IS NOT DISTINCT FROM k.unit
                  AND p.period_end IS NOT DISTINCT FROM k.period_end
                  AND p.period_start IS NOT DISTINCT FROM k.period_start
                """,
                [SOURCE_NAME],
            )
            store.con.execute(
                f"DELETE FROM sec_company_facts WHERE {cik_predicate}", cik_params,
            )
            _replace_companyfacts_candidates(store, cik, candidates if candidates is not None else pd.DataFrame())
            if not facts.empty:
                insert_frame(store, facts, "sec_company_facts", "sec_company_facts_insert")
                insert_frame(store, points, "fundamental_points", "fundamental_points_insert")
            store.con.execute("DROP TABLE companyfacts_point_delete_keys")
            store.con.execute("DROP TABLE companyfacts_legacy_ids")
            store.con.execute("DROP TABLE companyfacts_old_fact_keys")
            if receipt is not None:
                record_source_file(store, **receipt)
        return len(facts)
