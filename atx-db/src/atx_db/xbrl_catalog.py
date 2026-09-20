"""Concept-sized materialization for the SEC companyfacts catalog."""

from __future__ import annotations

from .connection import DuckDBStore
from .warehouse import json_dumps

_COLUMNS = (
    "source", "taxonomy", "concept", "label", "description", "statement_category",
    "units_json", "forms_json", "fiscal_periods_json", "first_period_end",
    "last_period_end", "first_filed_date", "last_filed_date", "first_available_at",
    "last_available_at", "fact_count", "security_count", "accession_count",
    "latest_source_loaded_at",
)


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


def refresh_concept_catalog(store: DuckDBStore) -> int:
    """Aggregate facts in DuckDB and publish one row per source/taxonomy/concept.

    Only concept-level results cross into Python. DuckDB controls the large scan
    and distinct-count aggregation under the caller's memory/spill settings.
    ``rowid`` pins the original first-non-NULL label/description choice to physical
    insertion order, including when insertion-order preservation is disabled.
    """
    aggregates = store.con.execute(
        """
        SELECT
            source,
            taxonomy,
            concept,
            arg_min(label, rowid) AS label,
            arg_min(description, rowid) AS description,
            list_sort(list(DISTINCT unit) FILTER (WHERE unit IS NOT NULL AND unit <> '')),
            list_sort(list(DISTINCT form) FILTER (WHERE form IS NOT NULL AND form <> '')),
            list_sort(list(DISTINCT fiscal_period)
                FILTER (WHERE fiscal_period IS NOT NULL AND fiscal_period <> '')),
            min(period_end),
            max(period_end),
            min(filed_date),
            max(filed_date),
            min(available_at),
            max(available_at),
            count(*),
            count(DISTINCT security_id),
            count(DISTINCT accession_number),
            max(source_loaded_at)
        FROM sec_company_facts
        WHERE taxonomy IS NOT NULL AND taxonomy <> ''
          AND concept IS NOT NULL AND concept <> ''
        GROUP BY source, taxonomy, concept
        ORDER BY source, taxonomy, concept
        """
    ).fetchall()
    if not aggregates:
        return 0

    # Keep the existing Python JSON serialization, including whitespace and
    # Unicode escaping. Dates/timestamps remain native DuckDB/Python values;
    # no full-fact DataFrame or pandas timestamp conversion is needed.
    rows = [
        (
            *row[:5],
            _statement_category(row[1], row[2]),
            *(json_dumps(values or []) for values in row[5:8]),
            *row[8:],
        )
        for row in aggregates
    ]
    columns = ", ".join(_COLUMNS)
    placeholders = ", ".join("?" for _ in _COLUMNS)
    try:
        with store.transaction():
            store.con.execute(
                f"""
                CREATE TEMP TABLE xbrl_concept_catalog_load AS
                SELECT {columns} FROM xbrl_concept_catalog WHERE false
                """
            )
            store.con.executemany(
                f"INSERT INTO xbrl_concept_catalog_load ({columns}) VALUES ({placeholders})",
                rows,
            )
            # Preserve the existing upsert scope: absent concepts, including an
            # entirely empty source table, do not erase historical catalog rows.
            store.con.execute(
                """
                DELETE FROM xbrl_concept_catalog AS dst
                USING xbrl_concept_catalog_load AS src
                WHERE dst.source = src.source
                  AND dst.taxonomy = src.taxonomy
                  AND dst.concept = src.concept
                """
            )
            store.con.execute(
                f"""
                INSERT INTO xbrl_concept_catalog ({columns})
                SELECT {columns} FROM xbrl_concept_catalog_load
                ORDER BY source, taxonomy, concept
                """
            )
    finally:
        store.con.execute("DROP TABLE IF EXISTS xbrl_concept_catalog_load")
    return len(rows)
