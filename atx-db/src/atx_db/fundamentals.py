from __future__ import annotations

import datetime as dt
import json
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pandas as pd
import requests

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
    security_and_entity_ids_for_ciks_asof,
    security_ids_for_symbols,
)
from .warehouse import insert_frame, json_dumps, now_utc_naive, quality_check, record_source_file, symbol_key


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
COMPANY_FACT_SYMBOL_SOURCES = ("symbols", "universe", "sec_company_tickers", "loaded_facts")


@dataclass(frozen=True)
class SecCompanyFactsOptions:
    symbols: tuple[str, ...] = ("AAPL",)
    concepts: tuple[str, ...] = DEFAULT_CONCEPTS
    symbol_source: str = "symbols"
    symbol_limit: int | None = None
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
    source = options.symbol_source.lower()
    if source == "symbols":
        return _apply_limit(ciks_for_symbols(store, options.symbols), options.symbol_limit)
    if source == "universe":
        return ciks_for_universe(
            store,
            universe_id=options.universe_id,
            as_of_date=options.as_of_date,
            limit=options.symbol_limit,
        )
    if source == "sec_company_tickers":
        return ciks_from_sec_company_tickers(store, limit=options.symbol_limit)
    if source == "loaded_facts":
        return ciks_from_loaded_facts(store, limit=options.symbol_limit)
    raise ValueError(
        f"Unsupported SEC companyfacts symbol_source {options.symbol_source!r}; "
        f"expected one of {', '.join(COMPANY_FACT_SYMBOL_SOURCES)}"
    )


def _date(value: Any) -> dt.date | None:
    if not value:
        return None
    return dt.date.fromisoformat(str(value))


def resolve_company_facts_identifiers(
    store: DuckDBStore, facts: pd.DataFrame
) -> tuple[pd.DataFrame, pd.DataFrame]:
    """PF-S5 S5-3: resolve security_id/entity_id per fact through the S5-0 spine.

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

    unresolved_rows: list[dict[str, Any]] = []
    # Facts sharing the exact same available_at resolve identically, so batch
    # the spine lookup per distinct available_at instead of per fact row.
    for available_at, group in out.groupby("available_at", sort=False, dropna=False):
        if pd.isna(available_at):
            # PF-S5 S5-3 fix: a fact with a null/NaT available_at has no known
            # filing-availability time. Resolving it "as of now" would leak
            # today's full identifier state into a fact whose true availability
            # is unknown -- a lookahead violation of the warehouse's
            # non-negotiable no-lookahead PIT invariant. Route it into the same
            # unresolved-ledger path as an unresolvable CIK instead: the fact
            # keeps flowing with its passthrough security_id, entity_id stays
            # null, and the caller records it in identifier_resolution_candidates.
            # (normalize_companyfacts always derives available_at from
            # filed_date, so this branch should not be reached in practice --
            # but it must be fail-safe, not fail-lookahead, if that ever changes.)
            for idx in group.index:
                cik = str(out.at[idx, "cik"]).strip()
                unresolved_rows.append(
                    {
                        "cik": cik,
                        "security_id": out.at[idx, "security_id"],
                        "available_at": available_at,
                    }
                )
            continue
        as_of_ts = pd.Timestamp(available_at).to_pydatetime()
        ciks = sorted({str(c).strip() for c in group["cik"] if str(c).strip()})
        resolved = security_and_entity_ids_for_ciks_asof(store, ciks, as_of_ts=as_of_ts)
        for idx in group.index:
            cik = str(out.at[idx, "cik"]).strip()
            match = resolved.get(cik)
            if match is None:
                unresolved_rows.append(
                    {
                        "cik": cik,
                        "security_id": out.at[idx, "security_id"],
                        "available_at": available_at,
                    }
                )
                continue
            resolved_security_id, resolved_entity_id = match
            out.at[idx, "security_id"] = resolved_security_id
            out.at[idx, "entity_id"] = resolved_entity_id

    if unresolved_rows:
        unresolved = pd.DataFrame(unresolved_rows).drop_duplicates(subset=["cik"]).reset_index(drop=True)
    else:
        unresolved = pd.DataFrame(columns=["cik", "security_id", "available_at"])
    return out, unresolved


def _unresolved_cik_candidates(unresolved: pd.DataFrame, *, run_id: str | None) -> pd.DataFrame:
    """Build identifier_resolution_candidates rows for unresolved companyfacts CIKs.

    Mirrors identifiers_figi.py's unmatched-cusip routing: status ``proposed``
    (a no-match problem, not a conflict among known entities), with a real
    ``source_security_id``/``target_security_id`` -- the fact's own best-effort
    passthrough security_id -- so the candidate is still auditable even though
    no spine entity was found for it.
    """
    if unresolved is None or unresolved.empty:
        return pd.DataFrame()
    as_of_date = dt.date.today()
    available_at = now_utc_naive()
    rows: list[dict[str, Any]] = []
    for row in unresolved.itertuples(index=False):
        security_id = str(row.security_id)
        rows.append(
            {
                "candidate_id": candidate_id_for(
                    source_dataset_id="sec_company_facts",
                    source_period=None,
                    source_key_type="CIK",
                    source_key_value=row.cik,
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
                "details_json": json_dumps({"status_reason": "unresolved_cik_at_fact_available_at"}),
                "run_id": run_id,
            }
        )
    return pd.DataFrame(rows)


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
        for concept, concept_payload in taxonomy_facts.items():
            if concepts and concept not in concepts:
                continue
            label = concept_payload.get("label")
            description = concept_payload.get("description")
            units = concept_payload.get("units", {})
            for unit, unit_rows in units.items():
                for item in unit_rows:
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


def _statement_category(taxonomy: str, concept: str) -> str:
    name = concept.lower()
    if taxonomy.lower() == "dei" or "sharesoutstanding" in name:
        return "share_count"
    if "earningspershare" in name:
        return "per_share"
    if any(
        token in name
        for token in (
            "netcashprovidedbyusedin",
            "paymentstoacquirepropertyplantandequipment",
            "paymentsforrepurchase",
            "paymentsofdividends",
        )
    ):
        return "cash_flow"
    if any(token in name for token in ("assets", "liabilities", "equity", "stocksincludingadditionalpaidincapital")):
        return "balance_sheet"
    if any(token in name for token in ("revenue", "income", "loss")):
        return "income_statement"
    return "other"


def _json_values(values: pd.Series) -> str:
    cleaned = sorted({str(value) for value in values.dropna() if str(value) != ""})
    return json_dumps(cleaned)


def refresh_xbrl_concept_catalog(store: DuckDBStore) -> int:
    """Refresh concept-level metadata from loaded SEC companyfacts."""

    facts = store.con.execute(
        """
        SELECT
            source,
            taxonomy,
            concept,
            label,
            description,
            unit,
            form,
            fiscal_period,
            period_end,
            filed_date,
            available_at,
            security_id,
            accession_number,
            source_loaded_at
        FROM sec_company_facts
        WHERE taxonomy IS NOT NULL
          AND taxonomy <> ''
          AND concept IS NOT NULL
          AND concept <> ''
        """
    ).df()
    if facts.empty:
        return 0

    rows: list[dict[str, Any]] = []
    grouped = facts.groupby(["source", "taxonomy", "concept"], dropna=False, sort=True)
    for (source, taxonomy, concept), group in grouped:
        labels = group["label"].dropna()
        descriptions = group["description"].dropna()
        rows.append(
            {
                "source": str(source),
                "taxonomy": str(taxonomy),
                "concept": str(concept),
                "label": str(labels.iloc[0]) if not labels.empty else None,
                "description": str(descriptions.iloc[0]) if not descriptions.empty else None,
                "statement_category": _statement_category(str(taxonomy), str(concept)),
                "units_json": _json_values(group["unit"]),
                "forms_json": _json_values(group["form"]),
                "fiscal_periods_json": _json_values(group["fiscal_period"]),
                "first_period_end": group["period_end"].min(),
                "last_period_end": group["period_end"].max(),
                "first_filed_date": group["filed_date"].min(),
                "last_filed_date": group["filed_date"].max(),
                "first_available_at": group["available_at"].min(),
                "last_available_at": group["available_at"].max(),
                "fact_count": int(len(group)),
                "security_count": int(group["security_id"].nunique()),
                "accession_count": int(group["accession_number"].nunique()),
                "latest_source_loaded_at": group["source_loaded_at"].max(),
            }
        )

    frame = pd.DataFrame(rows)
    store.con.register("xbrl_concept_catalog_load", frame)
    try:
        with store.transaction():
            store.con.execute(
                """
                DELETE FROM xbrl_concept_catalog AS dst
                USING xbrl_concept_catalog_load AS src
                WHERE dst.source = src.source
                  AND dst.taxonomy = src.taxonomy
                  AND dst.concept = src.concept
                """
            )
            columns = ", ".join(frame.columns)
            store.con.execute(
                f"""
                INSERT INTO xbrl_concept_catalog ({columns})
                SELECT {columns}
                FROM xbrl_concept_catalog_load
                """
            )
    finally:
        store.con.unregister("xbrl_concept_catalog_load")
    return int(len(frame))


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
    return int(store.con.execute("SELECT count(*) FROM fundamental_fact_revisions").fetchone()[0])


class _CompanyFactsZipFetcher:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self._archive = zipfile.ZipFile(self.path)
        self._names = set(self._archive.namelist())

    def __call__(self, cik: str | int) -> dict | None:
        try:
            member = f"CIK{int(str(cik).strip()):010d}.json"
        except (TypeError, ValueError):
            return None
        if member not in self._names:
            return None
        with self._archive.open(member) as handle:
            return json.load(handle)

    def close(self) -> None:
        self._archive.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


def _make_companyfacts_zip_fetcher(path: str | Path):
    """Return an OFFLINE fetcher backed by the SEC bulk ``companyfacts.zip``.

    SEC publishes the entire XBRL company-facts universe as a single nightly archive at
    ``https://www.sec.gov/Archives/edgar/daily-index/xbrl/companyfacts.zip`` containing one
    ``CIK##########.json`` per filer — byte-identical JSON to the per-CIK
    ``data.sec.gov/.../companyfacts/CIK*.json`` endpoint. Lookups are lazy (one member read
    per CIK) so the ~1.4 GB archive never loads fully into memory. Download is a one-time
    operator step; this fetcher and all tests run purely against a local file. Returns the
    parsed companyfacts payload (``{"cik", "entityName", "facts": {...}}``) for
    ``normalize_companyfacts`` to consume, or ``None`` if the CIK is absent / non-numeric.
    """
    return _CompanyFactsZipFetcher(path)


def _companyfacts_zip_member_url(cik: str | int) -> str:
    return f"{SEC_COMPANY_FACTS_ZIP_URL}#CIK{int(str(cik).strip()):010d}.json"


class SecCompanyFactsDataset(Dataset):
    dataset_id = "sec_company_facts"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: SecCompanyFactsOptions) -> DatasetLoadResult:
        # S45: an offline companyfacts.zip source replaces the per-CIK network endpoint.
        # When it is set we never open a network session — the load is fully local.
        zip_fetcher = (
            _make_companyfacts_zip_fetcher(options.companyfacts_zip)
            if options.companyfacts_zip is not None
            else None
        )
        session = sec_session(options.user_agent) if zip_fetcher is None else None
        source_mode = "bulk_zip" if zip_fetcher is not None else "api"
        source_name = "SEC companyfacts bulk zip" if zip_fetcher is not None else "SEC companyfacts API"
        source_cache_path = options.companyfacts_zip if zip_fetcher is not None else None
        effective_skip_failed = options.skip_failed_targets or zip_fetcher is not None
        concept_filter = set(options.concepts)
        targets = resolve_companyfacts_targets(store, options)
        # PF-S5 S5-3 fix (Minor): _replace_facts deletes sec_company_facts rows
        # keyed on cik alone (see its docstring/comment below). That is safe
        # ONLY because every resolve_companyfacts_targets source
        # (ciks_for_symbols / ciks_for_universe / ciks_from_sec_company_tickers /
        # ciks_from_loaded_facts) dedupes to at most one row per CIK per load.
        # Guard that invariant here so a future dual-class/aliasing path that
        # returns two targets for the same CIK fails loudly instead of silently
        # deleting the first target's just-inserted facts on the second delete.
        target_ciks = [cik for _symbol, cik, _security_id in targets]
        assert len(target_ciks) == len(set(target_ciks)), (
            "resolve_companyfacts_targets returned duplicate CIKs within one load "
            f"({len(target_ciks)} targets, {len(set(target_ciks))} distinct CIKs); "
            "_replace_facts deletes sec_company_facts by cik alone and would drop "
            "the earlier target's facts -- fix the target resolver to dedupe by CIK."
        )
        if not targets:
            if options.symbol_source == "symbols":
                missing = sorted({symbol_key(symbol) for symbol in options.symbols})
                sec_map = security_ids_for_symbols(store, missing)
                raise RuntimeError(f"No SEC CIK mapping for symbols {missing}; run security master first. Known: {sec_map}")
            raise RuntimeError(
                "No SEC companyfacts targets resolved for "
                f"symbol_source={options.symbol_source!r}; run security master/universe jobs first."
            )

        rows_loaded = 0
        point_rows = 0
        loaded_targets = 0
        failed_targets: list[dict[str, str]] = []
        unresolved_ciks: list[pd.DataFrame] = []
        attempts = max(1, options.max_attempts)
        for index, (symbol, cik, security_id) in enumerate(targets):
            # Local zip reads need no throttle; only the network path is rate-limited.
            if zip_fetcher is None and options.request_delay_seconds > 0 and index > 0:
                time.sleep(options.request_delay_seconds)
            source_url = (
                _companyfacts_zip_member_url(cik)
                if zip_fetcher is not None
                else SEC_COMPANY_FACTS_URL.format(cik=cik)
            )
            payload = None
            last_error: Exception | None = None
            if zip_fetcher is not None:
                try:
                    payload = zip_fetcher(cik)
                    if payload is None:
                        last_error = FileNotFoundError(f"CIK {cik} absent from companyfacts.zip")
                except Exception as exc:  # corrupt member / decode errors
                    last_error = exc
            else:
                for attempt in range(attempts):
                    try:
                        response = session.get(source_url, timeout=options.request_timeout)
                        response.raise_for_status()
                        payload = response.json()
                        break
                    except Exception as exc:  # network / HTTP / decode errors
                        last_error = exc
                        if attempt + 1 < attempts:
                            time.sleep(min(2.0 * (attempt + 1), 10.0))
            if payload is None:
                if not effective_skip_failed:
                    raise RuntimeError(f"SEC companyfacts fetch failed for {symbol} (CIK {cik}): {last_error}")
                failed_targets.append({"symbol": symbol, "cik": cik, "error": str(last_error)[:200]})
                record_source_file(
                    store,
                    dataset_id=self.dataset_id,
                    source_url=source_url,
                    cache_path=source_cache_path,
                    status="error",
                    metadata={
                        "symbol": symbol,
                        "cik": cik,
                        "source_mode": source_mode,
                        "error": str(last_error)[:200],
                    },
                )
                continue
            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=source_url,
                cache_path=source_cache_path,
                status="fetched",
                metadata={"symbol": symbol, "cik": cik, "source_mode": source_mode},
            )
            facts, points = normalize_companyfacts(
                payload,
                symbol=symbol,
                security_id=security_id,
                cik=cik,
                source_url=source_url,
                concepts=concept_filter,
                run_id=options.run_id,
            )
            # PF-S5 S5-3: resolve security_id/entity_id through the S5-0 spine,
            # per fact, as of that fact's own available_at (no lookahead).
            # Unresolved CIKs are never dropped -- they keep flowing with their
            # passthrough security_id and are collected for ledger routing below.
            facts, unresolved = resolve_company_facts_identifiers(store, facts)
            if not unresolved.empty:
                unresolved_ciks.append(unresolved)
            rows_loaded += self._replace_facts(store, facts, points, security_id, cik=cik)
            point_rows += len(points)
            loaded_targets += 1
        unresolved_candidate_rows = 0
        if unresolved_ciks:
            unresolved_frame = pd.concat(unresolved_ciks, ignore_index=True).drop_duplicates(subset=["cik"])
            candidates = _unresolved_cik_candidates(unresolved_frame, run_id=options.run_id)
            if not candidates.empty:
                store.con.register("sec_company_facts_unresolved_candidates", candidates)
                try:
                    with store.transaction():
                        store.con.execute(
                            """
                            DELETE FROM identifier_resolution_candidates
                            WHERE source_dataset_id = 'sec_company_facts'
                              AND match_method = ?
                              AND source_key_value IN (
                                  SELECT source_key_value FROM sec_company_facts_unresolved_candidates
                              )
                            """,
                            [FACT_IDENTIFIER_MATCH_METHOD],
                        )
                        insert_frame(
                            store,
                            candidates,
                            "identifier_resolution_candidates",
                            "sec_company_facts_unresolved_candidates_insert",
                        )
                finally:
                    store.con.unregister("sec_company_facts_unresolved_candidates")
                unresolved_candidate_rows = int(len(candidates))
        concept_rows = refresh_xbrl_concept_catalog(store)
        revision_rows = refresh_fundamental_fact_revisions(store)
        statement_rows = refresh_fundamental_statement_points(store)
        period_rows = refresh_fundamental_periods(store)
        ttm_rows = refresh_fundamental_ttm_points(store)
        if zip_fetcher is not None:
            zip_fetcher.close()

        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="sec_company_facts",
            check_name="rows_loaded",
            status="passed" if rows_loaded > 0 else "warning",
            observed_value=float(rows_loaded),
            threshold_value=1.0,
            details={
                "symbols": options.symbols,
                "symbol_source": options.symbol_source,
                "symbol_limit": options.symbol_limit,
                "universe_id": options.universe_id,
                "as_of_date": options.as_of_date.isoformat() if options.as_of_date else None,
                "target_count": len(targets),
                "source_mode": source_mode,
                "companyfacts_zip": str(options.companyfacts_zip) if options.companyfacts_zip else None,
                "point_rows": point_rows,
                "concept_catalog_rows": concept_rows,
                "revision_rows": revision_rows,
                "statement_rows": statement_rows,
                "period_rows": period_rows,
                "ttm_rows": ttm_rows,
                "unresolved_cik_candidate_rows": unresolved_candidate_rows,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=source_name,
            details={
                "symbols": options.symbols,
                "symbol_source": options.symbol_source,
                "symbol_limit": options.symbol_limit,
                "universe_id": options.universe_id,
                "as_of_date": options.as_of_date.isoformat() if options.as_of_date else None,
                "target_count": len(targets),
                "loaded_targets": loaded_targets,
                "failed_target_count": len(failed_targets),
                "failed_targets": failed_targets[:50],
                "source_mode": source_mode,
                "companyfacts_zip": str(options.companyfacts_zip) if options.companyfacts_zip else None,
                "facts": rows_loaded,
                "fundamental_points": point_rows,
                "xbrl_concept_catalog": concept_rows,
                "fundamental_fact_revisions": revision_rows,
                "fundamental_statement_points": statement_rows,
                "fundamental_periods": period_rows,
                "fundamental_ttm_points": ttm_rows,
                "unresolved_cik_candidate_rows": unresolved_candidate_rows,
            },
        )

    def _replace_facts(
        self,
        store: DuckDBStore,
        facts: pd.DataFrame,
        points: pd.DataFrame,
        security_id: str,
        *,
        cik: str,
    ) -> int:
        if facts.empty:
            return 0
        store.con.register("sec_company_facts_load", facts)
        store.con.register("fundamental_points_load", points)
        try:
            with store.transaction():
                # PF-S5 S5-3: sec_company_facts.security_id is now resolved per
                # fact row through the spine (resolve_company_facts_identifiers)
                # and is no longer guaranteed identical across every row for one
                # target -- so the replace-key for THIS table is cik (stable for
                # the whole target loop iteration), not security_id.
                # Deleting by cik alone is safe ONLY because every
                # resolve_companyfacts_targets source dedupes to one row per CIK
                # per load (asserted in load() before this loop runs) -- if a
                # future target resolver ever returns two rows for the same CIK,
                # the second target's delete here would silently wipe out the
                # first target's facts inserted moments earlier in this same load.
                # fundamental_points is untouched by S5-3 and still keys on the
                # loader's original passthrough security_id.
                store.con.execute(
                    """
                    DELETE FROM sec_company_facts
                    WHERE cik = ?
                    """
                    ,
                    [cik],
                )
                store.con.execute(
                    """
                    DELETE FROM fundamental_points
                    WHERE security_id = ?
                      AND source = ?
                    """
                    ,
                    [security_id, SOURCE_NAME],
                )
                insert_frame(store, facts, "sec_company_facts", "sec_company_facts_insert")
                insert_frame(store, points, "fundamental_points", "fundamental_points_insert")
        finally:
            for relation in ("sec_company_facts_load", "fundamental_points_load"):
                try:
                    store.con.unregister(relation)
                except Exception:
                    pass
        return int(len(facts))
