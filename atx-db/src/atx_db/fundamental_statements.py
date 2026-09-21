from __future__ import annotations

from contextlib import suppress
from dataclasses import astuple, dataclass

import pandas as pd

from ._bulk_publication import _identifier
from ._fundamental_publication import check_publication_session, fundamental_publication
from .connection import DuckDBStore
from .industry_templates import refresh_entity_industry_templates
from .statement_map_seed import (
    FundamentalStatementMapRow,
    default_statement_map_rows,
)

SOURCE_NAME = "SEC companyfacts"


def __getattr__(name: str) -> object:
    """PEP 562 shim: keep the historical module-level tuple name working."""

    if name == "FUNDAMENTAL_STATEMENT_MAP_ROWS":
        return default_statement_map_rows()
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


@dataclass(frozen=True)
class StatementMapOverlayException:
    taxonomy: str
    concept: str
    industry_template: str
    item_id: int
    canonical_metric: str
    reason: str


FUNDAMENTAL_STATEMENT_MAP_KEY = ("source", "taxonomy", "concept", "industry_template")
CONCEPT_MAP_SEED_COLUMNS = (
    "taxonomy",
    "concept",
    "canonical_metric",
    "item_id",
    "statement_type",
    "industry_template",
)
CONCEPT_MAP_SUPPORTED_TAXONOMIES = ("us-gaap", "dei")

# dei cover-page concepts the shares/float pipeline needs. They are not statement
# lines, so they are not in the statement-map projection, but shares_outstanding.py
# and the market-cap chain cannot run without them.
DEI_COVER_PAGE_CONCEPTS: tuple[str, ...] = (
    "EntityCommonStockSharesOutstanding",
    "EntityPublicFloat",
)


def _fundamental_statement_map_pk_columns(store: DuckDBStore) -> tuple[str, ...]:
    try:
        row = store.con.execute(
            """
            SELECT constraint_column_names
            FROM duckdb_constraints()
            WHERE table_name = 'fundamental_statement_map'
              AND constraint_type = 'PRIMARY KEY'
            """
        ).fetchone()
    except Exception:
        return ()
    if row is None or row[0] is None:
        return ()
    return tuple(str(col) for col in row[0])


def _create_fundamental_statement_map_table(store: DuckDBStore, table_name: str) -> None:
    store.con.execute(
        f"""
        CREATE TABLE IF NOT EXISTS {table_name} (
            source VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            statement_type VARCHAR NOT NULL,
            statement_section VARCHAR NOT NULL,
            canonical_metric VARCHAR NOT NULL,
            canonical_label VARCHAR NOT NULL,
            period_type VARCHAR NOT NULL,
            normal_balance VARCHAR NOT NULL,
            unit_type VARCHAR NOT NULL,
            value_multiplier DOUBLE NOT NULL DEFAULT 1.0,
            concept_priority INTEGER NOT NULL DEFAULT 100,
            is_core_metric BOOLEAN NOT NULL DEFAULT true,
            is_active BOOLEAN NOT NULL DEFAULT true,
            notes VARCHAR,
            item_id INTEGER,
            industry_template VARCHAR DEFAULT 'ALL',
            is_derived BOOLEAN DEFAULT FALSE,
            derivation_expr VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (source, taxonomy, concept, industry_template)
        )
        """
    )


def _ensure_fundamental_statement_map_storage(store: DuckDBStore) -> None:
    """Keep the map table keyed by source/taxonomy/concept/template on old DBs."""

    _create_fundamental_statement_map_table(store, "fundamental_statement_map")
    store.con.execute("DROP INDEX IF EXISTS idx_fundamental_statement_map_lookup")
    for statement in (
        "ALTER TABLE fundamental_statement_map ADD COLUMN IF NOT EXISTS item_id INTEGER",
        "ALTER TABLE fundamental_statement_map ADD COLUMN IF NOT EXISTS industry_template VARCHAR DEFAULT 'ALL'",
        "ALTER TABLE fundamental_statement_map ADD COLUMN IF NOT EXISTS is_derived BOOLEAN DEFAULT FALSE",
        "ALTER TABLE fundamental_statement_map ADD COLUMN IF NOT EXISTS derivation_expr VARCHAR",
    ):
        store.con.execute(statement)

    if _fundamental_statement_map_pk_columns(store) == FUNDAMENTAL_STATEMENT_MAP_KEY:
        return

    scratch = "fundamental_statement_map_rekey"
    store.con.execute(f"DROP TABLE IF EXISTS {scratch}")
    _create_fundamental_statement_map_table(store, scratch)
    store.con.execute(
        f"""
        INSERT OR REPLACE INTO {scratch} (
            source,
            taxonomy,
            concept,
            statement_type,
            statement_section,
            canonical_metric,
            canonical_label,
            period_type,
            normal_balance,
            unit_type,
            value_multiplier,
            concept_priority,
            is_core_metric,
            is_active,
            notes,
            item_id,
            industry_template,
            is_derived,
            derivation_expr,
            updated_at
        )
        SELECT
            source,
            taxonomy,
            concept,
            statement_type,
            statement_section,
            canonical_metric,
            canonical_label,
            period_type,
            normal_balance,
            unit_type,
            value_multiplier,
            concept_priority,
            is_core_metric,
            is_active,
            notes,
            item_id,
            coalesce(nullif(industry_template, ''), 'ALL') AS industry_template,
            coalesce(is_derived, FALSE) AS is_derived,
            derivation_expr,
            coalesce(updated_at, now()) AS updated_at
        FROM fundamental_statement_map
        """
    )
    store.con.execute("DROP TABLE fundamental_statement_map")
    store.con.execute(f"ALTER TABLE {scratch} RENAME TO fundamental_statement_map")


STATEMENT_MAP_OVERLAY_EXCEPTION_REASONS: dict[tuple[str, str, str, int], str] = {
    (
        "vendor-only",
        "__VENDOR_ONLY__nonperforming_loans",
        "BK",
        1507,
    ): "No standard us-gaap total non-performing loans concept is verified in the PF-S3 source map.",
    (
        "vendor-only",
        "__VENDOR_ONLY__net_charge_offs",
        "BK",
        1508,
    ): "Net charge-offs remain a vendor/regulatory credit metric pending a standard us-gaap mapping.",
    (
        "vendor-only",
        "__VENDOR_ONLY__total_deposits",
        "BK",
        1510,
    ): "Total deposits stay vendor-only until a bank-template total-deposit tag is source-verified.",
    (
        "vendor-only",
        "__VENDOR_ONLY__tier1_capital",
        "BK",
        1511,
    ): "Regulatory capital line; not a SEC companyfacts us-gaap statement concept.",
    (
        "vendor-only",
        "__VENDOR_ONLY__tier1_capital_ratio",
        "BK",
        1512,
    ): "Regulatory capital ratio; not a SEC companyfacts us-gaap statement concept.",
    (
        "vendor-only",
        "__VENDOR_ONLY__cet1",
        "BK",
        1513,
    ): "Regulatory CET1 capital line; not a SEC companyfacts us-gaap statement concept.",
    (
        "vendor-only",
        "__VENDOR_ONLY__risk_weighted_assets",
        "BK",
        1514,
    ): "Regulatory risk-weighted-assets denominator; not a SEC companyfacts us-gaap statement concept.",
    (
        "vendor-only",
        "__VENDOR_ONLY__efficiency_ratio",
        "BK",
        1515,
    ): "Bank efficiency ratio remains derived/vendor-only until formula-library support.",
    (
        "vendor-only",
        "__VENDOR_ONLY__insurance_benefits_paid",
        "IS",
        1604,
    ): "Benefits-paid mapping is not source-verified as a single comparable us-gaap statement concept.",
    (
        "vendor-only",
        "__VENDOR_ONLY__loss_ratio",
        "IS",
        1606,
    ): "Loss ratio is derived/vendor-only pending mapped claims and premium components.",
    (
        "vendor-only",
        "__VENDOR_ONLY__expense_ratio",
        "IS",
        1607,
    ): "Expense ratio is derived/vendor-only pending mapped expense and premium components.",
    (
        "vendor-only",
        "__VENDOR_ONLY__investment_portfolio",
        "IS",
        1609,
    ): "Investment portfolio remains vendor-only; no single source-verified us-gaap total is mapped.",
    (
        "vendor-only",
        "__VENDOR_ONLY__insurance_float",
        "IS",
        1610,
    ): "Insurance float is a derived/vendor construct, not a SEC companyfacts us-gaap statement concept.",
    (
        "nareit",
        "FundsFromOperations",
        "RT",
        1701,
    ): "Nareit-defined FFO is intentionally non-us-gaap and excluded from companyfacts defaults.",
    (
        "extension",
        "__EXTENSION__affo",
        "RT",
        1703,
    ): "AFFO is company-specific extension/vendor data; no common us-gaap concept is verified.",
    (
        "extension",
        "__EXTENSION__noi",
        "RT",
        1705,
    ): "NOI is a Nareit/property operating definition kept as extension/derived input.",
    (
        "extension",
        "__EXTENSION__same_store_noi",
        "RT",
        1706,
    ): "Same-store NOI is property-level KPI/extension data, not a standard us-gaap concept.",
    (
        "extension",
        "__EXTENSION__occupancy_rate",
        "RT",
        1707,
    ): "Occupancy rate is a REIT operating KPI/extension, not a standard us-gaap concept.",
    (
        "extension",
        "__EXTENSION__rent_per_square_foot",
        "RT",
        1708,
    ): "Rent per square foot is a REIT operating KPI/extension, not a standard us-gaap concept.",
    (
        "extension",
        "__EXTENSION__gross_leasable_area",
        "RT",
        1709,
    ): "Gross leasable area is a REIT operating KPI/extension, not a standard us-gaap concept.",
    (
        "vendor-only",
        "__VENDOR_ONLY__nav_per_share",
        "RT",
        1710,
    ): "NAV per share is vendor/appraisal-derived, not a standard us-gaap concept.",
    (
        "vendor-only",
        "__VENDOR_ONLY__capitalization_rate",
        "RT",
        1711,
    ): "Capitalization rate is market/appraisal-derived, not a standard us-gaap concept.",
    (
        "vendor-only",
        "__VENDOR_ONLY__ffo_payout_ratio",
        "RT",
        1712,
    ): "FFO payout ratio is derived from FFO and dividends; no direct us-gaap concept is mapped.",
    (
        "vendor-only",
        "__VENDOR_ONLY__utility_rate_base",
        "UT",
        1802,
    ): "Utility rate base is generally a regulatory/vendor field, not a standard us-gaap fact.",
    (
        "vendor-only",
        "__VENDOR_ONLY__net_capital",
        "BD",
        1905,
    ): "Broker-dealer net capital is a regulatory field, not a standard us-gaap companyfacts concept.",
}


def _statement_map_overlay_exception_key(
    row: FundamentalStatementMapRow,
) -> tuple[str, str, str, int]:
    return (
        row.taxonomy,
        row.concept,
        row.industry_template,
        int(row.item_id) if row.item_id is not None else -1,
    )


def statement_map_unloadable_overlay_rows(
    rows: tuple[FundamentalStatementMapRow, ...] | None = None,
) -> tuple[FundamentalStatementMapRow, ...]:
    """Return bank/insurance/REIT rows intentionally outside companyfacts loading."""

    rows = default_statement_map_rows() if rows is None else rows
    supported = set(CONCEPT_MAP_SUPPORTED_TAXONOMIES)
    return tuple(
        row
        for row in rows
        if row.industry_template != "ALL"
        and not row.is_derived
        and (
            row.taxonomy not in supported
            or not row.is_active
            or row.concept.startswith(("__VENDOR_ONLY__", "__EXTENSION__"))
        )
    )


def statement_map_overlay_exception_rows(
    rows: tuple[FundamentalStatementMapRow, ...] | None = None,
) -> tuple[StatementMapOverlayException, ...]:
    """Return the explicit report of non-loadable overlay rows and their reasons."""

    rows = default_statement_map_rows() if rows is None else rows
    rows_by_key = {_statement_map_overlay_exception_key(row): row for row in rows}
    missing = sorted(set(STATEMENT_MAP_OVERLAY_EXCEPTION_REASONS) - set(rows_by_key))
    if missing:
        raise ValueError(f"statement-map overlay allowlist keys missing from map: {missing}")
    return tuple(
        StatementMapOverlayException(
            taxonomy=key[0],
            concept=key[1],
            industry_template=key[2],
            item_id=key[3],
            canonical_metric=rows_by_key[key].canonical_metric,
            reason=reason,
        )
        for key, reason in sorted(
            STATEMENT_MAP_OVERLAY_EXCEPTION_REASONS.items(),
            key=lambda item: (item[0][2], item[0][3], item[0][0], item[0][1]),
        )
    )


def unexplained_statement_map_overlay_rows(
    rows: tuple[FundamentalStatementMapRow, ...] | None = None,
) -> tuple[FundamentalStatementMapRow, ...]:
    """Return non-loadable overlay rows not covered by the explicit exception report."""

    rows = default_statement_map_rows() if rows is None else rows
    allowed = set(STATEMENT_MAP_OVERLAY_EXCEPTION_REASONS)
    return tuple(
        row
        for row in statement_map_unloadable_overlay_rows(rows)
        if _statement_map_overlay_exception_key(row) not in allowed
    )


def concept_map_projection_rows(
    rows: tuple[FundamentalStatementMapRow, ...] | None = None,
    *,
    taxonomies: tuple[str, ...] = CONCEPT_MAP_SUPPORTED_TAXONOMIES,
) -> tuple[tuple[str, str, str, int, str, str], ...]:
    """Return active, loadable us-gaap/dei concept-map seed rows.

    Derived sentinels and vendor/extension-only placeholders are map metadata, not
    companyfacts fetch concepts. Non-loadable overlay rows stay in
    ``FUNDAMENTAL_STATEMENT_MAP_ROWS`` and are enumerated by
    ``statement_map_overlay_exception_rows``.
    """

    rows = default_statement_map_rows() if rows is None else rows
    taxonomy_set = set(taxonomies)
    projected: list[tuple[str, str, str, int, str, str]] = []
    for row in rows:
        if row.taxonomy not in taxonomy_set or not row.is_active or row.is_derived or row.concept.startswith("__"):
            continue
        if row.item_id is None:
            raise ValueError(
                f"active concept map row has no item_id: {row.taxonomy}:{row.concept} ({row.industry_template})"
            )
        projected.append(
            (
                row.taxonomy,
                row.concept,
                row.canonical_metric,
                int(row.item_id),
                row.statement_type,
                row.industry_template,
            )
        )
    return tuple(
        sorted(
            projected,
            key=lambda values: (values[5], values[4], values[0], values[1], values[2], values[3]),
        )
    )


def rule_alias_concepts() -> tuple[tuple[str, str], ...]:
    """Return sorted distinct (alias_scheme, alias_code) over active rules.

    Imported lazily: standardization imports connection/dataset/warehouse and we
    do not want fundamental_statements to pull that chain in at module import.
    """

    from .standardization import default_standardization_rules

    return tuple(
        sorted(
            {
                (alias.alias_scheme, alias.alias_code)
                for rule in default_standardization_rules()
                if rule.is_active
                for alias in rule.source_aliases
            }
        )
    )


def default_companyfacts_concepts() -> tuple[str, ...]:
    """Concept names admitted by the companyfacts loader by default.

    Tier1-S2 T2: this used to be only the statement-map projection (137
    concepts), which throttled the publication funnel - a curated alias could
    never emit because its concept was never ingested. It is now the union of

      1. the active, loadable statement-map projection,
      2. every alias referenced by an active standardization rule whose scheme
         is a supported taxonomy, and
      3. the dei cover-page concepts.

    All three inputs are committed seeds, so the result is deterministic.
    """

    concepts = {row[1] for row in concept_map_projection_rows()}
    supported = set(CONCEPT_MAP_SUPPORTED_TAXONOMIES)
    concepts.update(code for scheme, code in rule_alias_concepts() if scheme in supported)
    concepts.update(DEI_COVER_PAGE_CONCEPTS)
    return tuple(sorted(concepts))


def seed_fundamental_statement_map(store: DuckDBStore) -> int:
    """Seed canonical statement mappings for public SEC companyfacts concepts."""

    _ensure_fundamental_statement_map_storage(store)
    import pandas as pd

    seed = pd.DataFrame.from_records(
        [astuple(row) for row in default_statement_map_rows()],
        columns=(
            "source",
            "taxonomy",
            "concept",
            "statement_type",
            "statement_section",
            "canonical_metric",
            "canonical_label",
            "period_type",
            "normal_balance",
            "unit_type",
            "value_multiplier",
            "concept_priority",
            "is_core_metric",
            "is_active",
            "notes",
            "item_id",
            "industry_template",
            "is_derived",
            "derivation_expr",
        ),
    )
    store.con.execute(
        """
        DELETE FROM fundamental_statement_map
        WHERE source = ?
          AND industry_template = 'ALL'
          AND item_id BETWEEN 1428 AND 1440
        """,
        [SOURCE_NAME],
    )
    store.con.register("_fundamental_statement_map_seed", seed)
    try:
        store.con.execute(
            """
            INSERT OR REPLACE INTO fundamental_statement_map (
                source,
                taxonomy,
                concept,
                statement_type,
                statement_section,
                canonical_metric,
                canonical_label,
                period_type,
                normal_balance,
                unit_type,
                value_multiplier,
                concept_priority,
                is_core_metric,
                is_active,
                notes,
                item_id,
                industry_template,
                is_derived,
                derivation_expr,
                updated_at
            )
            SELECT
                source,
                taxonomy,
                concept,
                statement_type,
                statement_section,
                canonical_metric,
                canonical_label,
                period_type,
                normal_balance,
                unit_type,
                value_multiplier,
                concept_priority,
                is_core_metric,
                is_active,
                notes,
                item_id,
                industry_template,
                is_derived,
                derivation_expr,
                now()
            FROM _fundamental_statement_map_seed
            """
        )
    finally:
        store.con.unregister("_fundamental_statement_map_seed")
    store.con.execute(
        "CREATE INDEX IF NOT EXISTS idx_fundamental_statement_map_lookup "
        "ON fundamental_statement_map(taxonomy, concept, is_active)"
    )
    return len(default_statement_map_rows())


def _insert_derived_reit_statement_points(
    store: DuckDBStore, *, table: str = "fundamental_statement_points"
) -> int:
    """Derive REIT FFO/AFFO rows from raw statement points when reported rows are absent."""

    table = _identifier(table)
    before = int(store.con.execute(f"SELECT count(*) FROM {table}").fetchone()[0])
    store.con.execute(
        f"""
        INSERT INTO {table} (
            statement_point_id,
            fact_revision_id,
            revision_group_id,
            source,
            security_id,
            symbol,
            cik,
            statement_type,
            statement_section,
            canonical_metric,
            item_id,
            canonical_label,
            taxonomy,
            concept,
            unit,
            unit_type,
            period_type,
            normal_balance,
            period_start,
            period_end,
            as_of_date,
            available_at,
            fiscal_year,
            fiscal_period,
            form,
            accession_number,
            source_accession,
            filed_date,
            revision_sequence,
            revision_count,
            is_latest_revision,
            is_value_changed,
            raw_value,
            value,
            previous_raw_value,
            previous_value,
            value_delta,
            value_delta_percent,
            run_id,
            source_url,
            source_loaded_at
        )
        WITH latest_routes AS (
            SELECT security_id, industry_template
            FROM (
                SELECT
                    eit.*,
                    row_number() OVER (
                        PARTITION BY security_id
                        ORDER BY available_at DESC, source_loaded_at DESC, route_id DESC
                    ) AS route_rank
                FROM entity_industry_template eit
                WHERE is_latest_revision
                  AND valid_from <= current_date
                  AND coalesce(valid_to, DATE '9999-12-31') > current_date
            )
            WHERE route_rank = 1
        ),
        source_inputs AS (
            SELECT
                p.source,
                p.security_id,
                any_value(p.symbol) AS symbol,
                any_value(p.cik) AS cik,
                p.period_start,
                p.period_end,
                p.accession_number,
                max(p.as_of_date) AS as_of_date,
                max(p.available_at) AS available_at,
                any_value(p.fiscal_year) AS fiscal_year,
                any_value(p.fiscal_period) AS fiscal_period,
                any_value(p.form) AS form,
                any_value(p.run_id) AS run_id,
                coalesce(max(p.source_url), 'derived:reit_ffo_affo') AS source_url,
                max(p.source_loaded_at) AS source_loaded_at,
                max(p.unit) FILTER (
                    WHERE p.canonical_metric IN ('net_income', 'da_cf', 'da_is', 'depreciation', 'ffo', 'affo')
                ) AS monetary_unit,
                max(p.value) FILTER (WHERE p.canonical_metric = 'net_income') AS net_income,
                coalesce(
                    max(p.value) FILTER (WHERE p.canonical_metric = 'da_cf'),
                    max(p.value) FILTER (WHERE p.canonical_metric = 'da_is'),
                    max(p.value) FILTER (WHERE p.canonical_metric = 'depreciation')
                ) AS depreciation_amortization,
                max(p.value) FILTER (WHERE p.canonical_metric = 'ffo') AS reported_ffo,
                max(p.value) FILTER (WHERE p.canonical_metric = 'affo') AS reported_affo,
                max(p.value) FILTER (WHERE p.canonical_metric = 'shares_diluted_avg') AS shares_diluted_avg
            FROM {table} p
            JOIN latest_routes r
              ON r.security_id = p.security_id
             AND r.industry_template = 'RT'
            WHERE p.period_type = 'duration'
              AND p.canonical_metric IN (
                    'net_income',
                    'da_cf',
                    'da_is',
                    'depreciation',
                    'ffo',
                    'affo',
                    'shares_diluted_avg'
              )
            GROUP BY
                p.source,
                p.security_id,
                p.period_start,
                p.period_end,
                p.accession_number
        ),
        derived_basis AS (
            SELECT
                *,
                net_income + coalesce(depreciation_amortization, 0.0) AS fallback_ffo,
                coalesce(reported_ffo, net_income + coalesce(depreciation_amortization, 0.0)) AS ffo_basis,
                coalesce(reported_affo, reported_ffo, net_income + coalesce(depreciation_amortization, 0.0)) AS affo_basis
            FROM source_inputs
            WHERE net_income IS NOT NULL
               OR reported_ffo IS NOT NULL
               OR reported_affo IS NOT NULL
        ),
        candidate_rows AS (
            SELECT
                source,
                security_id,
                symbol,
                cik,
                'reit_statement' AS statement_type,
                'cash_flow' AS statement_section,
                'ffo' AS canonical_metric,
                'Funds from operations (FFO)' AS canonical_label,
                'derived' AS taxonomy,
                '__DERIVED__ffo' AS concept,
                coalesce(monetary_unit, 'USD') AS unit,
                'monetary' AS unit_type,
                fallback_ffo AS value,
                period_start,
                period_end,
                as_of_date,
                available_at,
                fiscal_year,
                fiscal_period,
                form,
                accession_number,
                run_id,
                source_url,
                source_loaded_at
            FROM derived_basis b
            WHERE reported_ffo IS NULL
              AND fallback_ffo IS NOT NULL
              AND NOT EXISTS (
                  SELECT 1
                  FROM {table} existing
                  WHERE existing.source = b.source
                    AND existing.security_id = b.security_id
                    AND existing.canonical_metric = 'ffo'
                    AND existing.period_start IS NOT DISTINCT FROM b.period_start
                    AND existing.period_end = b.period_end
                    AND existing.accession_number = b.accession_number
              )
            UNION ALL
            SELECT
                source,
                security_id,
                symbol,
                cik,
                'reit_statement',
                'cash_flow',
                'affo',
                'Adjusted FFO (AFFO)',
                'derived',
                '__DERIVED__affo',
                coalesce(monetary_unit, 'USD'),
                'monetary',
                affo_basis,
                period_start,
                period_end,
                as_of_date,
                available_at,
                fiscal_year,
                fiscal_period,
                form,
                accession_number,
                run_id,
                source_url,
                source_loaded_at
            FROM derived_basis b
            WHERE affo_basis IS NOT NULL
              AND NOT EXISTS (
                  SELECT 1
                  FROM {table} existing
                  WHERE existing.source = b.source
                    AND existing.security_id = b.security_id
                    AND existing.canonical_metric = 'affo'
                    AND existing.period_start IS NOT DISTINCT FROM b.period_start
                    AND existing.period_end = b.period_end
                    AND existing.accession_number = b.accession_number
              )
            UNION ALL
            SELECT
                source,
                security_id,
                symbol,
                cik,
                'reit_statement',
                'per_share',
                'ffo_per_share',
                'FFO per share',
                'derived',
                '__DERIVED__ffo_per_share',
                'USD/shares',
                'per_share',
                ffo_basis / shares_diluted_avg,
                period_start,
                period_end,
                as_of_date,
                available_at,
                fiscal_year,
                fiscal_period,
                form,
                accession_number,
                run_id,
                source_url,
                source_loaded_at
            FROM derived_basis b
            WHERE ffo_basis IS NOT NULL
              AND shares_diluted_avg IS NOT NULL
              AND shares_diluted_avg <> 0
              AND NOT EXISTS (
                  SELECT 1
                  FROM {table} existing
                  WHERE existing.source = b.source
                    AND existing.security_id = b.security_id
                    AND existing.canonical_metric = 'ffo_per_share'
                    AND existing.period_start IS NOT DISTINCT FROM b.period_start
                    AND existing.period_end = b.period_end
                    AND existing.accession_number = b.accession_number
              )
            UNION ALL
            SELECT
                source,
                security_id,
                symbol,
                cik,
                'reit_statement',
                'per_share',
                'affo_per_share',
                'AFFO per share',
                'derived',
                '__DERIVED__affo_per_share',
                'USD/shares',
                'per_share',
                affo_basis / shares_diluted_avg,
                period_start,
                period_end,
                as_of_date,
                available_at,
                fiscal_year,
                fiscal_period,
                form,
                accession_number,
                run_id,
                source_url,
                source_loaded_at
            FROM derived_basis b
            WHERE affo_basis IS NOT NULL
              AND shares_diluted_avg IS NOT NULL
              AND shares_diluted_avg <> 0
              AND NOT EXISTS (
                  SELECT 1
                  FROM {table} existing
                  WHERE existing.source = b.source
                    AND existing.security_id = b.security_id
                    AND existing.canonical_metric = 'affo_per_share'
                    AND existing.period_start IS NOT DISTINCT FROM b.period_start
                    AND existing.period_end = b.period_end
                    AND existing.accession_number = b.accession_number
              )
        ),
        keyed AS (
            SELECT
                sha256(
                    concat_ws(
                        '|',
                        'derived_reit_ffo_affo',
                        source,
                        security_id,
                        canonical_metric,
                        unit,
                        coalesce(CAST(period_start AS VARCHAR), ''),
                        CAST(period_end AS VARCHAR),
                        accession_number
                    )
                ) AS statement_point_id,
                sha256(
                    concat_ws(
                        '|',
                        'derived_reit_ffo_affo',
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
                sha256(
                    concat_ws(
                        '|',
                        source,
                        security_id,
                        canonical_metric,
                        unit,
                        coalesce(CAST(period_start AS VARCHAR), ''),
                        CAST(period_end AS VARCHAR)
                    )
                ) AS revision_group_id,
                *,
                lag(value) OVER statement_window AS previous_value,
                row_number() OVER statement_window AS revision_sequence,
                count(*) OVER statement_window AS revision_count
            FROM candidate_rows
            WINDOW statement_window AS (
                PARTITION BY source, security_id, canonical_metric, unit, period_start, period_end
                ORDER BY
                    coalesce(available_at, CAST(as_of_date AS TIMESTAMP)),
                    as_of_date,
                    coalesce(source_loaded_at, TIMESTAMP '1970-01-01'),
                    accession_number
                ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
            )
        )
        SELECT
            statement_point_id,
            fact_revision_id,
            revision_group_id,
            source,
            security_id,
            symbol,
            cik,
            statement_type,
            statement_section,
            canonical_metric,
            CASE canonical_metric
                WHEN 'ffo' THEN 1701
                WHEN 'ffo_per_share' THEN 1702
                WHEN 'affo' THEN 1703
                WHEN 'affo_per_share' THEN 1704
            END AS item_id,
            canonical_label,
            taxonomy,
            concept,
            unit,
            unit_type,
            'duration' AS period_type,
            'credit' AS normal_balance,
            period_start,
            period_end,
            as_of_date,
            available_at,
            fiscal_year,
            fiscal_period,
            form,
            accession_number,
            accession_number AS source_accession,
            as_of_date AS filed_date,
            revision_sequence,
            revision_count,
            revision_sequence = revision_count AS is_latest_revision,
            CASE
                WHEN revision_sequence = 1 THEN false
                ELSE value IS DISTINCT FROM previous_value
            END AS is_value_changed,
            value AS raw_value,
            value,
            previous_value AS previous_raw_value,
            previous_value,
            CASE
                WHEN previous_value IS NULL OR value IS NULL THEN NULL
                ELSE value - previous_value
            END AS value_delta,
            CASE
                WHEN previous_value IS NULL OR previous_value = 0 OR value IS NULL THEN NULL
                ELSE (value - previous_value) / abs(previous_value)
            END AS value_delta_percent,
            run_id,
            source_url,
            source_loaded_at
        FROM keyed
        """
    )
    after = int(store.con.execute(f"SELECT count(*) FROM {table}").fetchone()[0])
    return after - before


def refresh_fundamental_statement_points(
    store: DuckDBStore,
    concepts: tuple[str, ...] | None = None,
) -> int:
    """Refresh mapped statement facts, optionally for selected raw concepts."""

    check_publication_session(store)
    seed_fundamental_statement_map(store)
    selected = tuple(sorted({str(concept) for concept in concepts or () if concept}))
    registered = False
    concept_join = ""
    if selected:
        store.con.register(
            "fundamental_statement_concept_filter",
            pd.DataFrame({"concept": selected}),
        )
        registered = True
        concept_join = "JOIN fundamental_statement_concept_filter cf ON cf.concept = r.concept"
    else:
        refresh_entity_industry_templates(store)
    try:
        with fundamental_publication(
            store,
            ("fundamental_statement_points",),
            replace_where=(
                "concept IN (SELECT concept FROM fundamental_statement_concept_filter)"
                if selected else "TRUE"
            ),
            owned_registrations=("fundamental_statement_concept_filter",) if registered else (),
        ):
            store.con.execute(
                f"""
            INSERT INTO fundamental_statement_points_bulk_stage (
                statement_point_id,
                fact_revision_id,
                revision_group_id,
                source,
                security_id,
                symbol,
                cik,
                statement_type,
                statement_section,
                canonical_metric,
                item_id,
                canonical_label,
                taxonomy,
                concept,
                unit,
                unit_type,
                period_type,
                normal_balance,
                period_start,
                period_end,
                as_of_date,
                available_at,
                fiscal_year,
                fiscal_period,
                form,
                accession_number,
                source_accession,
                filed_date,
                revision_sequence,
                revision_count,
                is_latest_revision,
                is_value_changed,
                raw_value,
                value,
                previous_raw_value,
                previous_value,
                value_delta,
                value_delta_percent,
                run_id,
                source_url,
                source_loaded_at
            )
            WITH security_industry_templates AS (
                SELECT
                    security_id,
                    industry_template
                FROM (
                    SELECT
                        eit.*,
                        row_number() OVER (
                            PARTITION BY security_id
                            ORDER BY available_at DESC, source_loaded_at DESC, route_id DESC
                        ) AS route_rank
                    FROM entity_industry_template eit
                    WHERE eit.is_latest_revision
                      AND eit.valid_from <= current_date
                      AND coalesce(eit.valid_to, DATE '9999-12-31') > current_date
                )
                WHERE route_rank = 1
            ),
            mapped AS (
                SELECT
                    sha256(
                        concat_ws(
                            '|',
                            r.source,
                            r.security_id,
                            m.canonical_metric,
                            r.unit,
                            coalesce(CAST(r.period_start AS VARCHAR), ''),
                            CAST(r.period_end AS VARCHAR),
                            r.accession_number
                        )
                    ) AS statement_point_id,
                    sha256(
                        concat_ws(
                            '|',
                            r.source,
                            r.security_id,
                            m.canonical_metric,
                            r.unit,
                            coalesce(CAST(r.period_start AS VARCHAR), ''),
                            CAST(r.period_end AS VARCHAR)
                        )
                    ) AS statement_revision_group_id,
                    r.fact_revision_id,
                    r.source,
                    r.security_id,
                    s.primary_symbol AS symbol,
                    r.cik,
                    m.statement_type,
                    m.statement_section,
                    m.canonical_metric,
                    m.item_id,
                    m.canonical_label,
                    r.taxonomy,
                    r.concept,
                    r.unit,
                    m.unit_type,
                    m.period_type,
                    m.normal_balance,
                    r.period_start,
                    r.period_end,
                    r.filed_date AS as_of_date,
                    r.available_at,
                    r.fiscal_year,
                    r.fiscal_period,
                    r.form,
                    r.accession_number,
                    r.value AS raw_value,
                    r.value * m.value_multiplier AS value,
                    r.run_id,
                    r.source_url,
                    r.source_loaded_at,
                    row_number() OVER (
                        PARTITION BY
                            r.source,
                            r.security_id,
                            m.canonical_metric,
                            r.unit,
                            r.period_start,
                            r.period_end,
                            r.accession_number
                        ORDER BY
                            m.concept_priority,
                            r.available_at DESC NULLS LAST,
                            r.source_loaded_at DESC NULLS LAST,
                            r.taxonomy,
                            r.concept,
                            r.fact_revision_id
                    ) AS canonical_rank
                FROM fundamental_fact_revisions r
                {concept_join}
                LEFT JOIN security_industry_templates it
                  ON it.security_id = r.security_id
                JOIN fundamental_statement_map m
                  ON m.source = r.source
                 AND m.taxonomy = r.taxonomy
                 AND m.concept = r.concept
                 AND (
                        m.industry_template = 'ALL'
                     OR m.industry_template = coalesce(it.industry_template, 'ALL')
                 )
                 AND m.is_active
                 AND m.is_derived = FALSE
                LEFT JOIN securities s
                  ON s.security_id = r.security_id
            ),
            chosen AS (
                SELECT *
                FROM mapped
                WHERE canonical_rank = 1
                  -- A duration (flow) fact with no period_start has no definable
                  -- window; some companyfacts vintages emit such malformed facts
                  -- (e.g. an early NetIncomeLoss with only an end date). Drop them
                  -- so every duration statement point has a valid period_start.
                  AND NOT (period_type = 'duration' AND period_start IS NULL)
            ),
            sequenced AS (
                SELECT
                    chosen.*,
                    row_number() OVER statement_window AS revision_sequence,
                    count(*) OVER statement_window AS revision_count,
                    lag(raw_value) OVER statement_window AS previous_raw_value,
                    lag(value) OVER statement_window AS previous_value
                FROM chosen
                WINDOW statement_window AS (
                    PARTITION BY statement_revision_group_id
                    ORDER BY
                        coalesce(available_at, CAST(as_of_date AS TIMESTAMP)),
                        as_of_date,
                        coalesce(source_loaded_at, TIMESTAMP '1970-01-01'),
                        accession_number,
                        statement_point_id
                    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
                )
            )
            SELECT
                statement_point_id,
                fact_revision_id,
                statement_revision_group_id AS revision_group_id,
                source,
                security_id,
                symbol,
                cik,
                statement_type,
                statement_section,
                canonical_metric,
                item_id,
                canonical_label,
                taxonomy,
                concept,
                unit,
                unit_type,
                period_type,
                normal_balance,
                period_start,
                period_end,
                as_of_date,
                available_at,
                fiscal_year,
                fiscal_period,
                form,
                accession_number,
                accession_number AS source_accession,
                as_of_date AS filed_date,
                revision_sequence,
                revision_count,
                revision_sequence = revision_count AS is_latest_revision,
                CASE
                    WHEN revision_sequence = 1 THEN false
                    ELSE value IS DISTINCT FROM previous_value
                END AS is_value_changed,
                raw_value,
                value,
                previous_raw_value,
                previous_value,
                CASE
                    WHEN previous_value IS NULL OR value IS NULL THEN NULL
                    ELSE value - previous_value
                END AS value_delta,
                CASE
                    WHEN previous_value IS NULL OR previous_value = 0 OR value IS NULL THEN NULL
                    ELSE (value - previous_value) / abs(previous_value)
                END AS value_delta_percent,
                run_id,
                source_url,
                source_loaded_at
            FROM sequenced
            """
            )
            if not selected:
                _insert_derived_reit_statement_points(store, table="fundamental_statement_points_bulk_stage")
    finally:
        if registered:
            with suppress(Exception):
                store.con.unregister("fundamental_statement_concept_filter")
    return int(store.con.execute("SELECT count(*) FROM fundamental_statement_points").fetchone()[0])


def refresh_fundamental_periods(store: DuckDBStore) -> int:
    """Refresh normalized reporting-period windows from SEC statement points."""

    with fundamental_publication(store, ("fundamental_periods",)):
        store.con.execute(
            """
            INSERT INTO fundamental_periods_bulk_stage (
                fundamental_period_id,
                period_group_id,
                source,
                security_id,
                symbol,
                cik,
                period_start,
                period_end,
                datadate,
                period_days,
                normalized_period_type,
                calendar_year,
                calendar_quarter,
                calendar_period,
                rdq,
                pdate,
                fdate,
                ldate,
                as_of_date,
                available_at,
                form,
                accession_number,
                reported_fiscal_years_json,
                reported_fiscal_periods_json,
                statement_types_json,
                canonical_metrics_json,
                input_statement_point_ids_json,
                statement_point_count,
                canonical_metric_count,
                concept_count,
                value_changed_statement_count,
                has_balance_sheet,
                has_income_statement,
                has_cash_flow,
                has_per_share,
                revision_sequence,
                revision_count,
                is_latest_revision,
                first_available_at,
                latest_available_at,
                source_loaded_at
            )
            WITH grouped_base AS (
                SELECT
                    sha256(
                        concat_ws(
                            '|',
                            source,
                            security_id,
                            coalesce(CAST(period_start AS VARCHAR), ''),
                            CAST(period_end AS VARCHAR)
                        )
                    ) AS period_group_id,
                    sha256(
                        concat_ws(
                            '|',
                            source,
                            security_id,
                            coalesce(CAST(period_start AS VARCHAR), ''),
                            CAST(period_end AS VARCHAR),
                            accession_number
                        )
                    ) AS fundamental_period_id,
                    source,
                    security_id,
                    any_value(symbol) AS symbol,
                    any_value(cik) AS cik,
                    period_start,
                    period_end,
                    period_end AS datadate,
                    CASE
                        WHEN period_start IS NULL THEN NULL
                        ELSE date_diff('day', period_start, period_end) + 1
                    END AS period_days,
                    CASE
                        WHEN period_start IS NULL THEN 'instant'
                        WHEN date_diff('day', period_start, period_end) + 1 BETWEEN 70 AND 120 THEN 'quarter'
                        WHEN date_diff('day', period_start, period_end) + 1 BETWEEN 121 AND 220 THEN 'semiannual_ytd'
                        WHEN date_diff('day', period_start, period_end) + 1 BETWEEN 221 AND 329 THEN 'multi_quarter_ytd'
                        WHEN date_diff('day', period_start, period_end) + 1 BETWEEN 330 AND 380 THEN 'annual'
                        WHEN date_diff('day', period_start, period_end) + 1 > 380 THEN 'multi_year_comparative'
                        ELSE 'other'
                    END AS normalized_period_type,
                    CAST(EXTRACT(YEAR FROM period_end) AS INTEGER) AS calendar_year,
                    CAST(EXTRACT(QUARTER FROM period_end) AS INTEGER) AS calendar_quarter,
                    CAST(EXTRACT(YEAR FROM period_end) AS VARCHAR)
                        || 'Q'
                        || CAST(EXTRACT(QUARTER FROM period_end) AS VARCHAR) AS calendar_period,
                    max(as_of_date) AS as_of_date,
                    max(as_of_date) AS fdate,
                    max(available_at) AS available_at,
                    any_value(form) AS form,
                    accession_number,
                    CAST(to_json(list(DISTINCT CAST(fiscal_year AS VARCHAR) ORDER BY CAST(fiscal_year AS VARCHAR))) AS VARCHAR) AS reported_fiscal_years_json,
                    CAST(to_json(list(DISTINCT fiscal_period ORDER BY fiscal_period)) AS VARCHAR) AS reported_fiscal_periods_json,
                    CAST(to_json(list(DISTINCT statement_type ORDER BY statement_type)) AS VARCHAR) AS statement_types_json,
                    CAST(to_json(list(DISTINCT canonical_metric ORDER BY canonical_metric)) AS VARCHAR) AS canonical_metrics_json,
                    CAST(to_json(list(statement_point_id ORDER BY statement_type, canonical_metric, statement_point_id)) AS VARCHAR) AS input_statement_point_ids_json,
                    count(*) AS statement_point_count,
                    count(DISTINCT canonical_metric) AS canonical_metric_count,
                    count(DISTINCT concat_ws('|', taxonomy, concept)) AS concept_count,
                    sum(CASE WHEN is_value_changed THEN 1 ELSE 0 END) AS value_changed_statement_count,
                    bool_or(statement_type = 'balance_sheet') AS has_balance_sheet,
                    bool_or(statement_type = 'income_statement') AS has_income_statement,
                    bool_or(statement_type = 'cash_flow') AS has_cash_flow,
                    bool_or(statement_type = 'per_share') AS has_per_share,
                    max(source_loaded_at) AS source_loaded_at
                FROM fundamental_statement_points
                WHERE source IS NOT NULL
                  AND source <> ''
                  AND security_id IS NOT NULL
                  AND security_id <> ''
                  AND cik IS NOT NULL
                  AND cik <> ''
                  AND period_end IS NOT NULL
                  AND as_of_date IS NOT NULL
                  AND accession_number IS NOT NULL
                  AND accession_number <> ''
                GROUP BY source, security_id, period_start, period_end, accession_number
            ),
            rdq_candidates AS (
                SELECT
                    security_id,
                    coalesce(report_date, filing_date, CAST(acceptance_datetime AS DATE)) AS rdq,
                    acceptance_datetime AS rdq_available_at,
                    accession_number AS rdq_accession_number,
                    source_url AS rdq_source_url
                FROM sec_submissions
                WHERE form = '8-K'
                  AND coalesce(items, '') LIKE '%2.02%'
                  AND coalesce(report_date, filing_date, CAST(acceptance_datetime AS DATE)) IS NOT NULL
            ),
            grouped AS (
                SELECT
                    grouped_base.*,
                    rdq.rdq,
                    rdq.rdq AS pdate
                FROM grouped_base
                LEFT JOIN LATERAL (
                    SELECT
                        r.rdq,
                        r.rdq_available_at,
                        r.rdq_accession_number
                    FROM rdq_candidates r
                    WHERE r.security_id = grouped_base.security_id
                      AND r.rdq >= grouped_base.period_end
                      AND r.rdq <= grouped_base.fdate
                    ORDER BY
                        r.rdq,
                        r.rdq_available_at NULLS LAST,
                        r.rdq_accession_number
                    LIMIT 1
                ) rdq ON true
            ),
            sequenced AS (
                SELECT
                    grouped.*,
                    row_number() OVER period_window AS revision_sequence,
                    count(*) OVER period_window AS revision_count,
                    min(available_at) OVER period_window AS first_available_at,
                    max(available_at) OVER period_window AS latest_available_at,
                    CAST(max(available_at) OVER period_window AS DATE) AS ldate
                FROM grouped
                WINDOW period_window AS (
                    PARTITION BY period_group_id
                    ORDER BY
                        available_at,
                        as_of_date,
                        coalesce(source_loaded_at, TIMESTAMP '1970-01-01'),
                        accession_number
                    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
                )
            )
            SELECT
                fundamental_period_id,
                period_group_id,
                source,
                security_id,
                symbol,
                cik,
                period_start,
                period_end,
                datadate,
                period_days,
                normalized_period_type,
                calendar_year,
                calendar_quarter,
                calendar_period,
                rdq,
                pdate,
                fdate,
                ldate,
                as_of_date,
                available_at,
                form,
                accession_number,
                coalesce(reported_fiscal_years_json, '[]') AS reported_fiscal_years_json,
                coalesce(reported_fiscal_periods_json, '[]') AS reported_fiscal_periods_json,
                statement_types_json,
                canonical_metrics_json,
                input_statement_point_ids_json,
                statement_point_count,
                canonical_metric_count,
                concept_count,
                value_changed_statement_count,
                has_balance_sheet,
                has_income_statement,
                has_cash_flow,
                has_per_share,
                revision_sequence,
                revision_count,
                revision_sequence = revision_count AS is_latest_revision,
                first_available_at,
                latest_available_at,
                source_loaded_at
            FROM sequenced
            """
        )
    return int(store.con.execute("SELECT count(*) FROM fundamental_periods").fetchone()[0])


def refresh_fundamental_ttm_points(
    store: DuckDBStore,
    canonical_metrics: tuple[str, ...] | None = None,
) -> int:
    """Refresh PIT-safe trailing-twelve-month statement values.

    ``canonical_metrics`` scopes both the delete and the source scan. This keeps a
    single-concept activation from rewriting the full multi-million-row TTM surface.
    Omitting it preserves the original full-refresh behavior.
    """

    selected = tuple(sorted({str(metric) for metric in canonical_metrics or () if metric}))
    placeholders = ", ".join("?" for _ in selected)
    metric_filter_sql = f"AND canonical_metric IN ({placeholders})" if selected else ""

    with fundamental_publication(
        store,
        ("fundamental_ttm_points",),
        replace_where=f"canonical_metric IN ({placeholders})" if selected else "TRUE",
        replace_params=selected,
    ):
        store.con.execute(
            f"""
            INSERT INTO fundamental_ttm_points_bulk_stage (
                ttm_point_id,
                ttm_revision_group_id,
                anchor_statement_point_id,
                source,
                security_id,
                symbol,
                cik,
                statement_type,
                statement_section,
                canonical_metric,
                canonical_label,
                unit,
                unit_type,
                ttm_start_date,
                ttm_end_date,
                as_of_date,
                available_at,
                fiscal_year,
                fiscal_period,
                form,
                accession_number,
                quarter_count,
                coverage_days,
                min_input_available_at,
                max_input_available_at,
                input_statement_point_ids_json,
                input_accessions_json,
                input_period_ends_json,
                ttm_value,
                previous_ttm_value,
                ttm_value_delta,
                ttm_value_delta_percent,
                revision_sequence,
                revision_count,
                is_latest_revision,
                is_value_changed,
                calculation_method,
                source_loaded_at
            )
            WITH statement_points AS (
                SELECT
                    statement_point_id,
                    revision_group_id,
                    source,
                    security_id,
                    symbol,
                    cik,
                    statement_type,
                    statement_section,
                    canonical_metric,
                    canonical_label,
                    unit,
                    unit_type,
                    period_start,
                    period_end,
                    as_of_date,
                    available_at,
                    fiscal_year,
                    fiscal_period,
                    form,
                    accession_number,
                    value,
                    revision_sequence,
                    source_loaded_at,
                    date_diff('day', period_start, period_end) + 1 AS period_days,
                    coalesce(available_at, CAST(as_of_date AS TIMESTAMP)) AS availability_ts
                FROM fundamental_statement_points
                WHERE period_type = 'duration'
                  AND period_start IS NOT NULL
                  AND period_end IS NOT NULL
                  AND value IS NOT NULL
                  {metric_filter_sql}
            ),
            reported_quarter_points AS (
                SELECT
                    statement_point_id,
                    statement_point_id AS anchor_statement_point_id,
                    revision_group_id,
                    source,
                    security_id,
                    symbol,
                    cik,
                    statement_type,
                    statement_section,
                    canonical_metric,
                    canonical_label,
                    unit,
                    unit_type,
                    period_start,
                    period_end,
                    as_of_date,
                    available_at,
                    fiscal_year,
                    fiscal_period,
                    form,
                    accession_number,
                    value,
                    revision_sequence,
                    source_loaded_at,
                    period_days,
                    availability_ts,
                    1 AS quarter_source_priority
                FROM statement_points
                WHERE period_days BETWEEN 70 AND 115
            ),
            ytd_points AS (
                SELECT *
                FROM statement_points
                WHERE unit_type = 'monetary'
                  AND period_days BETWEEN 160 AND 380
            ),
            prior_ytd_points AS (
                SELECT *
                FROM statement_points
                WHERE unit_type = 'monetary'
                  AND period_days BETWEEN 70 AND 290
            ),
            derived_ytd_quarter_points AS (
                SELECT
                    sha256(concat_ws('|', 'derived_ytd_quarter', current_ytd.statement_point_id, prior_ytd.statement_point_id)) AS statement_point_id,
                    current_ytd.statement_point_id AS anchor_statement_point_id,
                    sha256(concat_ws('|', 'derived_ytd_quarter', current_ytd.revision_group_id, prior_ytd.revision_group_id)) AS revision_group_id,
                    current_ytd.source,
                    current_ytd.security_id,
                    current_ytd.symbol,
                    current_ytd.cik,
                    current_ytd.statement_type,
                    current_ytd.statement_section,
                    current_ytd.canonical_metric,
                    current_ytd.canonical_label,
                    current_ytd.unit,
                    current_ytd.unit_type,
                    CAST(prior_ytd.period_end + INTERVAL 1 DAY AS DATE) AS period_start,
                    current_ytd.period_end,
                    greatest(current_ytd.as_of_date, prior_ytd.as_of_date) AS as_of_date,
                    greatest(current_ytd.availability_ts, prior_ytd.availability_ts) AS available_at,
                    current_ytd.fiscal_year,
                    CASE
                        WHEN current_ytd.period_days BETWEEN 160 AND 205 THEN 'Q2_DERIVED'
                        WHEN current_ytd.period_days BETWEEN 250 AND 290 THEN 'Q3_DERIVED'
                        WHEN current_ytd.period_days BETWEEN 330 AND 380 THEN 'Q4_DERIVED'
                        ELSE 'Q_DERIVED'
                    END AS fiscal_period,
                    current_ytd.form,
                    current_ytd.accession_number,
                    current_ytd.value - prior_ytd.value AS value,
                    current_ytd.revision_sequence,
                    greatest(
                        coalesce(current_ytd.source_loaded_at, TIMESTAMP '1970-01-01'),
                        coalesce(prior_ytd.source_loaded_at, TIMESTAMP '1970-01-01')
                    ) AS source_loaded_at,
                    date_diff('day', CAST(prior_ytd.period_end + INTERVAL 1 DAY AS DATE), current_ytd.period_end) + 1 AS period_days,
                    greatest(current_ytd.availability_ts, prior_ytd.availability_ts) AS availability_ts,
                    2 AS quarter_source_priority
                FROM ytd_points current_ytd
                JOIN prior_ytd_points prior_ytd
                  ON prior_ytd.source = current_ytd.source
                 AND prior_ytd.security_id = current_ytd.security_id
                 AND prior_ytd.canonical_metric = current_ytd.canonical_metric
                 AND prior_ytd.unit = current_ytd.unit
                 AND prior_ytd.period_start = current_ytd.period_start
                 AND prior_ytd.period_end < current_ytd.period_end
                 AND prior_ytd.as_of_date <= current_ytd.as_of_date
                 AND prior_ytd.availability_ts <= current_ytd.availability_ts
                QUALIFY row_number() OVER (
                    PARTITION BY current_ytd.statement_point_id
                    ORDER BY
                        prior_ytd.period_end DESC,
                        prior_ytd.availability_ts DESC,
                        prior_ytd.as_of_date DESC,
                        coalesce(prior_ytd.source_loaded_at, TIMESTAMP '1970-01-01') DESC,
                        prior_ytd.revision_sequence DESC,
                        prior_ytd.statement_point_id DESC
                ) = 1
            ),
            quarter_points AS (
                SELECT *
                FROM reported_quarter_points
                UNION ALL
                SELECT *
                FROM derived_ytd_quarter_points
                WHERE period_days BETWEEN 70 AND 115
            ),
            visible AS (
                SELECT
                    a.anchor_statement_point_id AS anchor_statement_point_id,
                    a.as_of_date AS anchor_as_of_date,
                    a.available_at AS anchor_available_at,
                    a.fiscal_year AS anchor_fiscal_year,
                    a.fiscal_period AS anchor_fiscal_period,
                    a.form AS anchor_form,
                    a.accession_number AS anchor_accession_number,
                    a.source_loaded_at AS anchor_source_loaded_at,
                    q.*,
                    row_number() OVER (
                        PARTITION BY a.anchor_statement_point_id, q.period_start, q.period_end
                        ORDER BY
                            q.availability_ts DESC,
                            q.quarter_source_priority,
                            q.as_of_date DESC,
                            coalesce(q.source_loaded_at, TIMESTAMP '1970-01-01') DESC,
                            q.revision_sequence DESC,
                            q.statement_point_id DESC
                    ) AS visible_rank
                FROM quarter_points a
                JOIN quarter_points q
                  ON q.source = a.source
                 AND q.security_id = a.security_id
                 AND q.canonical_metric = a.canonical_metric
                 AND q.unit = a.unit
                 AND q.period_end <= a.period_end
                 -- Only the trailing ~12 months can contribute to a TTM (the four
                 -- quarters ending at the anchor span <365 days). Without this lower
                 -- bound the self-join is triangular over a security's entire quarter
                 -- history (O(N^2)), which is fine for a handful of securities but
                 -- spilled tens of GiB once the universe widened to ~1,600 issuers.
                 AND q.period_end > a.period_end - INTERVAL 400 DAY
                 AND q.as_of_date <= a.as_of_date
                 AND q.availability_ts <= a.availability_ts
            ),
            latest_visible AS (
                SELECT *
                FROM visible
                WHERE visible_rank = 1
            ),
            trailing_windows AS (
                SELECT
                    *,
                    row_number() OVER (
                        PARTITION BY anchor_statement_point_id
                        ORDER BY period_end DESC, period_start DESC, statement_point_id DESC
                    ) AS trailing_rank
                FROM latest_visible
            ),
            aggregated AS (
                SELECT
                    sha256(
                        concat_ws(
                            '|',
                            any_value(source),
                            any_value(security_id),
                            any_value(canonical_metric),
                            any_value(unit),
                            CAST(max(period_end) AS VARCHAR)
                        )
                    ) AS ttm_revision_group_id,
                    any_value(anchor_statement_point_id) AS anchor_statement_point_id,
                    any_value(source) AS source,
                    any_value(security_id) AS security_id,
                    any_value(symbol) AS symbol,
                    any_value(cik) AS cik,
                    any_value(statement_type) AS statement_type,
                    any_value(statement_section) AS statement_section,
                    any_value(canonical_metric) AS canonical_metric,
                    any_value(canonical_label) AS canonical_label,
                    any_value(unit) AS unit,
                    any_value(unit_type) AS unit_type,
                    min(period_start) AS ttm_start_date,
                    max(period_end) AS ttm_end_date,
                    greatest(max(as_of_date), any_value(anchor_as_of_date)) AS as_of_date,
                    greatest(
                        max(availability_ts),
                        any_value(anchor_available_at)
                    ) AS available_at,
                    any_value(anchor_fiscal_year) AS fiscal_year,
                    any_value(anchor_fiscal_period) AS fiscal_period,
                    any_value(anchor_form) AS form,
                    any_value(anchor_accession_number) AS accession_number,
                    count(*) AS quarter_count,
                    date_diff('day', min(period_start), max(period_end)) + 1 AS coverage_days,
                    min(availability_ts) AS min_input_available_at,
                    max(availability_ts) AS max_input_available_at,
                    CAST(to_json(list(statement_point_id ORDER BY period_end, statement_point_id)) AS VARCHAR) AS input_statement_point_ids_json,
                    CAST(to_json(list(accession_number ORDER BY period_end, statement_point_id)) AS VARCHAR) AS input_accessions_json,
                    CAST(to_json(list(CAST(period_end AS VARCHAR) ORDER BY period_end, statement_point_id)) AS VARCHAR) AS input_period_ends_json,
                    sum(value) AS ttm_value,
                    bool_or(fiscal_period = 'Q4_DERIVED') AS has_stitched_q4,
                    max(coalesce(source_loaded_at, anchor_source_loaded_at)) AS source_loaded_at
                FROM trailing_windows
                WHERE trailing_rank <= 4
                GROUP BY anchor_statement_point_id
                HAVING count(*) = 4
                   AND date_diff('day', min(period_start), max(period_end)) + 1 BETWEEN 330 AND 380
            ),
            keyed AS (
                SELECT
                    sha256(concat_ws('|', ttm_revision_group_id, anchor_statement_point_id)) AS ttm_point_id,
                    *
                FROM aggregated
            ),
            sequenced AS (
                SELECT
                    keyed.*,
                    row_number() OVER ttm_window AS revision_sequence,
                    count(*) OVER ttm_window AS revision_count,
                    lag(ttm_value) OVER ttm_window AS previous_ttm_value
                FROM keyed
                WINDOW ttm_window AS (
                    PARTITION BY ttm_revision_group_id
                    ORDER BY
                        available_at,
                        as_of_date,
                        coalesce(source_loaded_at, TIMESTAMP '1970-01-01'),
                        anchor_statement_point_id
                    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
                )
            )
            SELECT
                ttm_point_id,
                ttm_revision_group_id,
                anchor_statement_point_id,
                source,
                security_id,
                symbol,
                cik,
                statement_type,
                statement_section,
                canonical_metric,
                canonical_label,
                unit,
                unit_type,
                ttm_start_date,
                ttm_end_date,
                as_of_date,
                available_at,
                fiscal_year,
                fiscal_period,
                form,
                accession_number,
                CAST(quarter_count AS INTEGER) AS quarter_count,
                CAST(coverage_days AS INTEGER) AS coverage_days,
                min_input_available_at,
                max_input_available_at,
                input_statement_point_ids_json,
                input_accessions_json,
                input_period_ends_json,
                ttm_value,
                previous_ttm_value,
                CASE
                    WHEN previous_ttm_value IS NULL OR ttm_value IS NULL THEN NULL
                    ELSE ttm_value - previous_ttm_value
                END AS ttm_value_delta,
                CASE
                    WHEN previous_ttm_value IS NULL OR previous_ttm_value = 0 OR ttm_value IS NULL THEN NULL
                    ELSE (ttm_value - previous_ttm_value) / abs(previous_ttm_value)
                END AS ttm_value_delta_percent,
                revision_sequence,
                revision_count,
                revision_sequence = revision_count AS is_latest_revision,
                CASE
                    WHEN revision_sequence = 1 THEN false
                    ELSE ttm_value IS DISTINCT FROM previous_ttm_value
                END AS is_value_changed,
                CASE
                    WHEN has_stitched_q4 THEN 'stitched_quarterly_ttm'
                    ELSE 'sum_four_visible_quarter_like_statement_points_with_ytd_quarter_derivations'
                END AS calculation_method,
                source_loaded_at
            FROM sequenced
            """,
            list(selected),
        )
    return int(store.con.execute("SELECT count(*) FROM fundamental_ttm_points").fetchone()[0])
