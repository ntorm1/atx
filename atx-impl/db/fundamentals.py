from __future__ import annotations

import datetime as dt
import time
from dataclasses import dataclass
from typing import Any

import pandas as pd
import requests

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .fundamental_statements import (
    refresh_fundamental_periods,
    refresh_fundamental_statement_points,
    refresh_fundamental_ttm_points,
)
from .security_master import SEC_USER_AGENT, sec_session, security_ids_for_symbols
from .warehouse import insert_frame, json_dumps, quality_check, record_source_file, symbol_key


SEC_COMPANY_FACTS_URL = "https://data.sec.gov/api/xbrl/companyfacts/CIK{cik}.json"
SOURCE_NAME = "SEC companyfacts"
DEFAULT_CONCEPTS = (
    "Assets",
    "Liabilities",
    "StockholdersEquity",
    "Revenues",
    "RevenueFromContractWithCustomerExcludingAssessedTax",
    "NetIncomeLoss",
    "OperatingIncomeLoss",
    "NetCashProvidedByUsedInOperatingActivities",
    "NetCashProvidedByUsedInInvestingActivities",
    "NetCashProvidedByUsedInFinancingActivities",
    "PaymentsToAcquirePropertyPlantAndEquipment",
    "PaymentsForRepurchaseOfCommonStock",
    "PaymentsOfDividends",
    "EarningsPerShareDiluted",
    "CommonStocksIncludingAdditionalPaidInCapital",
    "EntityCommonStockSharesOutstanding",
)
# Only taxonomies the statement map understands are loaded. The fundamentals pipeline
# is us-gaap (+ dei cover-page) based; IFRS foreign private issuers report under
# ``ifrs-full`` with no us-gaap map, which both leaves catalog concepts unmapped and
# collides on canonical metric names (e.g. ifrs-full Assets vs us-gaap Assets) — so
# non-supported taxonomies are dropped at load.
SUPPORTED_FACT_TAXONOMIES = ("us-gaap", "dei")
COMPANY_FACT_SYMBOL_SOURCES = ("symbols", "universe", "sec_company_tickers")


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
    raise ValueError(
        f"Unsupported SEC companyfacts symbol_source {options.symbol_source!r}; "
        f"expected one of {', '.join(COMPANY_FACT_SYMBOL_SOURCES)}"
    )


def _date(value: Any) -> dt.date | None:
    if not value:
        return None
    return dt.date.fromisoformat(str(value))


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


def refresh_fundamental_fact_revisions(store: DuckDBStore) -> int:
    """Refresh accession-level revision chains from SEC companyfacts."""

    with store.transaction():
        store.con.execute("DELETE FROM fundamental_fact_revisions")
        store.con.execute(
            """
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
                FROM sec_company_facts
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
    return int(store.con.execute("SELECT count(*) FROM fundamental_fact_revisions").fetchone()[0])


class SecCompanyFactsDataset(Dataset):
    dataset_id = "sec_company_facts"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: SecCompanyFactsOptions) -> DatasetLoadResult:
        session = sec_session(options.user_agent)
        concept_filter = set(options.concepts)
        targets = resolve_companyfacts_targets(store, options)
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
        attempts = max(1, options.max_attempts)
        for index, (symbol, cik, security_id) in enumerate(targets):
            if options.request_delay_seconds > 0 and index > 0:
                time.sleep(options.request_delay_seconds)
            source_url = SEC_COMPANY_FACTS_URL.format(cik=cik)
            payload = None
            last_error: Exception | None = None
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
                if not options.skip_failed_targets:
                    raise RuntimeError(f"SEC companyfacts fetch failed for {symbol} (CIK {cik}): {last_error}")
                failed_targets.append({"symbol": symbol, "cik": cik, "error": str(last_error)[:200]})
                record_source_file(
                    store,
                    dataset_id=self.dataset_id,
                    source_url=source_url,
                    status="error",
                    metadata={"symbol": symbol, "cik": cik, "error": str(last_error)[:200]},
                )
                continue
            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=source_url,
                status="fetched",
                metadata={"symbol": symbol, "cik": cik},
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
            rows_loaded += self._replace_facts(store, facts, points, security_id)
            point_rows += len(points)
            loaded_targets += 1
        concept_rows = refresh_xbrl_concept_catalog(store)
        revision_rows = refresh_fundamental_fact_revisions(store)
        statement_rows = refresh_fundamental_statement_points(store)
        period_rows = refresh_fundamental_periods(store)
        ttm_rows = refresh_fundamental_ttm_points(store)

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
                "point_rows": point_rows,
                "concept_catalog_rows": concept_rows,
                "revision_rows": revision_rows,
                "statement_rows": statement_rows,
                "period_rows": period_rows,
                "ttm_rows": ttm_rows,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source="SEC companyfacts API",
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
                "facts": rows_loaded,
                "fundamental_points": point_rows,
                "xbrl_concept_catalog": concept_rows,
                "fundamental_fact_revisions": revision_rows,
                "fundamental_statement_points": statement_rows,
                "fundamental_periods": period_rows,
                "fundamental_ttm_points": ttm_rows,
            },
        )

    def _replace_facts(
        self,
        store: DuckDBStore,
        facts: pd.DataFrame,
        points: pd.DataFrame,
        security_id: str,
    ) -> int:
        if facts.empty:
            return 0
        store.con.register("sec_company_facts_load", facts)
        store.con.register("fundamental_points_load", points)
        try:
            with store.transaction():
                store.con.execute(
                    """
                    DELETE FROM sec_company_facts
                    WHERE security_id = ?
                    """
                    ,
                    [security_id],
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
