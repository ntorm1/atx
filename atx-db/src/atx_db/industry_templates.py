"""PF2-S5: industry-specialized fundamental templates.

This module promotes the old inline SIC-to-template CASE into governed data:
template dimensions, routed entity assignments, required-item sets, and a small
coverage report. It is deliberately deterministic and offline-only.
"""
from __future__ import annotations

import datetime as dt
from dataclasses import astuple, dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .warehouse import quality_check

SOURCE_NAME = "derived_industry_templates_v1"


@dataclass(frozen=True)
class IndustryTemplate:
    template_code: str
    label: str
    vendor_profile: str
    accounting_class: str
    required_item_set: str
    description: str
    is_active: bool = True
    valid_from: dt.date = dt.date(1900, 1, 1)
    valid_to: dt.date | None = None


@dataclass(frozen=True)
class IndustryTemplateItem:
    template_code: str
    item_id: int
    canonical_code: str
    requirement_level: str
    not_available: bool
    notes: str
    valid_from: dt.date = dt.date(1900, 1, 1)
    valid_to: dt.date | None = None


@dataclass(frozen=True)
class IndustryTemplateOptions:
    source: str = SOURCE_NAME
    run_id: str | None = None


INDUSTRY_TEMPLATES: tuple[IndustryTemplate, ...] = (
    IndustryTemplate("ALL", "Commercial / industrial", "FactSet Commercial", "INDL", "all_core_v1",
                     "Industrial fallback template for companies without a specialized financial profile."),
    IndustryTemplate("BK", "Bank", "FactSet Bank", "FS", "bank_v1",
                     "Bank template for depositories and credit institutions."),
    IndustryTemplate("IS", "Insurance", "FactSet Insurance", "FS", "insurance_v1",
                     "Insurance template for carriers and reinsurers."),
    IndustryTemplate("UT", "Utility", "Compustat Utility", "INDL", "utility_v1",
                     "Regulated utility template with rate-base and utility operating metrics."),
    IndustryTemplate("BD", "Broker-dealer", "FactSet Other Financial", "FS", "broker_dealer_v1",
                     "Broker-dealer and securities intermediary template."),
    IndustryTemplate("RT", "REIT", "Compustat REIT / Nareit", "FS", "reit_v1",
                     "REIT template with FFO/AFFO and real-estate operating metrics."),
)


TEMPLATE_ITEMS: tuple[IndustryTemplateItem, ...] = (
    IndustryTemplateItem("ALL", 1001, "revenue", "required", False, "Core industrial revenue."),
    IndustryTemplateItem("ALL", 1031, "net_income", "required", False, "Core industrial net income."),
    IndustryTemplateItem("ALL", 1101, "total_assets", "required", False, "Core industrial total assets."),
    IndustryTemplateItem("ALL", 1201, "total_liabilities", "required", False, "Core industrial liabilities."),
    IndustryTemplateItem("ALL", 1221, "stockholders_equity", "required", False, "Core industrial equity."),
    IndustryTemplateItem("BK", 1501, "net_interest_income", "required", False, "Bank net interest income."),
    IndustryTemplateItem("BK", 1503, "interest_income_bank", "required", False, "Bank interest income."),
    IndustryTemplateItem("BK", 1504, "interest_expense_bank", "required", False, "Bank interest expense."),
    IndustryTemplateItem("BK", 1509, "total_loans", "required", False, "Bank total loans."),
    IndustryTemplateItem("BK", 1507, "nonperforming_loans", "required", True, "Vendor/regulatory only."),
    IndustryTemplateItem("BK", 1511, "tier1_capital", "required", True, "Regulatory capital only."),
    IndustryTemplateItem("IS", 1601, "premiums_earned", "required", False, "Insurance premiums earned."),
    IndustryTemplateItem("IS", 1605, "unpaid_claim_liability", "required", False, "Insurance unpaid claims."),
    IndustryTemplateItem("IS", 1606, "loss_ratio", "required", True, "Vendor/derived until claims components load."),
    IndustryTemplateItem("IS", 1607, "expense_ratio", "required", True, "Vendor/derived until expense components load."),
    IndustryTemplateItem("UT", 1801, "utility_operating_revenue", "required", False, "Utility operating revenue."),
    IndustryTemplateItem("UT", 1802, "utility_rate_base", "required", True, "Regulatory rate base is usually vendor/regulatory."),
    IndustryTemplateItem("UT", 1803, "utility_ppe_rate_base", "required", False, "Utility PP&E rate-base proxy."),
    IndustryTemplateItem("UT", 1804, "utility_operating_income", "required", False, "Utility operating income."),
    IndustryTemplateItem("UT", 1805, "utility_depreciation_amortization", "required", False, "Utility depreciation/amortization."),
    IndustryTemplateItem("BD", 1901, "broker_dealer_revenue", "required", False, "Broker-dealer revenue."),
    IndustryTemplateItem("BD", 1902, "segregated_cash_securities", "required", False, "Customer segregated cash/securities."),
    IndustryTemplateItem("BD", 1903, "payables_broker_dealers", "required", False, "Payables to brokers/clearing orgs."),
    IndustryTemplateItem("BD", 1904, "receivables_broker_dealers", "required", False, "Receivables from brokers/clearing orgs."),
    IndustryTemplateItem("BD", 1905, "net_capital", "required", True, "Regulatory net capital is vendor/regulatory."),
    IndustryTemplateItem("RT", 1701, "ffo", "required", False, "REIT funds from operations."),
    IndustryTemplateItem("RT", 1702, "ffo_per_share", "required", False, "REIT FFO per share."),
    IndustryTemplateItem("RT", 1703, "affo", "required", False, "REIT adjusted FFO."),
    IndustryTemplateItem("RT", 1704, "affo_per_share", "required", False, "REIT AFFO per share."),
    IndustryTemplateItem("RT", 1710, "nav_per_share", "required", True, "Vendor/appraisal only."),
)


def template_for_sic(sic_code: str | int | None) -> tuple[str, str]:
    """Return (template_code, match_reason) for a SIC code."""

    try:
        sic = int(str(sic_code))
    except (TypeError, ValueError):
        return "ALL", "default_all"
    if 6000 <= sic <= 6199:
        return "BK", "sic_6000_6199_bank"
    if 6200 <= sic <= 6299:
        return "BD", "sic_6200_6299_broker_dealer"
    if 6300 <= sic <= 6411:
        return "IS", "sic_6300_6411_insurance"
    if sic == 6798:
        return "RT", "sic_6798_reit"
    if 4900 <= sic <= 4999:
        return "UT", "sic_4900_4999_utility"
    return "ALL", "default_all"


def seed_industry_templates(store: DuckDBStore) -> int:
    """Seed template and required-item constants into governed tables."""

    templates = pd.DataFrame.from_records(
        [astuple(row) for row in INDUSTRY_TEMPLATES],
        columns=[
            "template_code",
            "label",
            "vendor_profile",
            "accounting_class",
            "required_item_set",
            "description",
            "is_active",
            "valid_from",
            "valid_to",
        ],
    )
    items = pd.DataFrame.from_records(
        [astuple(row) for row in TEMPLATE_ITEMS],
        columns=[
            "template_code",
            "item_id",
            "canonical_code",
            "requirement_level",
            "not_available",
            "notes",
            "valid_from",
            "valid_to",
        ],
    )
    store.con.register("_industry_template_seed", templates)
    store.con.register("_industry_template_item_seed", items)
    try:
        store.con.execute(
            """
            INSERT OR REPLACE INTO industry_template (
                template_code, label, vendor_profile, accounting_class,
                required_item_set, description, is_active, valid_from, valid_to, updated_at
            )
            SELECT
                template_code, label, vendor_profile, accounting_class,
                required_item_set, description, is_active,
                CAST(valid_from AS DATE), CAST(valid_to AS DATE), now()
            FROM _industry_template_seed
            """
        )
        store.con.execute(
            """
            INSERT OR REPLACE INTO industry_template_item (
                template_code, item_id, canonical_code, requirement_level,
                not_available, notes, valid_from, valid_to, updated_at
            )
            SELECT
                template_code, item_id, canonical_code, requirement_level,
                not_available, notes, CAST(valid_from AS DATE), CAST(valid_to AS DATE), now()
            FROM _industry_template_item_seed
            """
        )
    finally:
        store.con.unregister("_industry_template_seed")
        store.con.unregister("_industry_template_item_seed")
    return len(INDUSTRY_TEMPLATES)


def refresh_entity_industry_templates(
    store: DuckDBStore,
    options: IndustryTemplateOptions | None = None,
) -> int:
    """Materialize revision-complete security -> template routing from SIC history."""

    options = options or IndustryTemplateOptions()
    seed_industry_templates(store)
    with store.transaction():
        store.con.execute("DELETE FROM entity_industry_template WHERE source = ?", [options.source])
        store.con.execute(
            """
            INSERT INTO entity_industry_template (
                route_id, source, security_id, symbol, industry_template,
                matched_taxonomy, matched_node_code, match_reason,
                valid_from, valid_to, as_of_date, available_at,
                is_latest_revision, run_id, source_loaded_at,
                route_revision_group_id, revision_sequence, revision_count,
                previous_industry_template, update_type, knowledge_valid_to
            )
            WITH sic_raw AS (
                SELECT
                    ec.classification_id,
                    ec.security_id,
                    ec.node_code,
                    ec.valid_from,
                    ec.valid_to,
                    ec.as_of_date,
                    coalesce(ec.available_at, ec.source_loaded_at) AS available_at,
                    ec.source_loaded_at
                FROM entity_classification ec
                JOIN taxonomy tx
                  ON tx.taxonomy_id = ec.taxonomy_id
                 AND tx.code = 'SIC'
                WHERE ec.is_primary
            ),
            sic_boundaries AS (
                SELECT
                    security_id,
                    valid_from,
                    lead(valid_from) OVER (
                        PARTITION BY security_id ORDER BY valid_from
                    ) AS next_valid_from
                FROM (
                    SELECT DISTINCT security_id, valid_from
                    FROM sic_raw
                )
            ),
            bounded_sic AS (
                SELECT
                    raw.*,
                    CASE
                        WHEN raw.valid_to IS NULL THEN boundary.next_valid_from
                        WHEN boundary.next_valid_from IS NULL THEN raw.valid_to
                        ELSE least(raw.valid_to, boundary.next_valid_from)
                    END AS effective_valid_to
                FROM sic_raw raw
                JOIN sic_boundaries boundary
                  ON boundary.security_id = raw.security_id
                 AND boundary.valid_from = raw.valid_from
            ),
            first_sic AS (
                SELECT security_id,min(valid_from) AS first_valid_from
                FROM bounded_sic
                GROUP BY security_id
            ),
            fundamental_history AS (
                SELECT security_id,min(period_end) AS first_period_end
                FROM sec_company_facts
                WHERE period_end IS NOT NULL
                GROUP BY security_id
            ),
            routed_sic AS (
                SELECT
                    sic.classification_id,
                    sic.security_id,
                    s.primary_symbol AS symbol,
                    sic.node_code,
                    sic.valid_from,
                    sic.effective_valid_to AS valid_to,
                    sic.as_of_date,
                    sic.available_at,
                    sic.source_loaded_at,
                    CASE
                        WHEN try_cast(sic.node_code AS INTEGER) BETWEEN 6000 AND 6199 THEN 'BK'
                        WHEN try_cast(sic.node_code AS INTEGER) BETWEEN 6200 AND 6299 THEN 'BD'
                        WHEN try_cast(sic.node_code AS INTEGER) BETWEEN 6300 AND 6411 THEN 'IS'
                        WHEN try_cast(sic.node_code AS INTEGER) = 6798 THEN 'RT'
                        WHEN try_cast(sic.node_code AS INTEGER) BETWEEN 4900 AND 4999 THEN 'UT'
                        ELSE 'ALL'
                    END AS industry_template,
                    CASE
                        WHEN try_cast(sic.node_code AS INTEGER) BETWEEN 6000 AND 6199 THEN 'sic_6000_6199_bank'
                        WHEN try_cast(sic.node_code AS INTEGER) BETWEEN 6200 AND 6299 THEN 'sic_6200_6299_broker_dealer'
                        WHEN try_cast(sic.node_code AS INTEGER) BETWEEN 6300 AND 6411 THEN 'sic_6300_6411_insurance'
                        WHEN try_cast(sic.node_code AS INTEGER) = 6798 THEN 'sic_6798_reit'
                        WHEN try_cast(sic.node_code AS INTEGER) BETWEEN 4900 AND 4999 THEN 'sic_4900_4999_utility'
                        ELSE 'default_all'
                    END AS match_reason
                FROM bounded_sic sic
                LEFT JOIN securities s ON s.security_id = sic.security_id
            ),
            inferred_backcast_routes AS (
                SELECT
                    routed.* REPLACE (
                        history.first_period_end AS valid_from,
                        routed.valid_from AS valid_to,
                        concat('inferred_backcast_',routed.match_reason) AS match_reason
                    )
                FROM routed_sic routed
                JOIN first_sic first
                  ON first.security_id=routed.security_id
                 AND first.first_valid_from=routed.valid_from
                JOIN fundamental_history history
                  ON history.security_id=routed.security_id
                 AND history.first_period_end<routed.valid_from
            ),
            fallback_routes AS (
                SELECT
                    CAST(NULL AS VARCHAR) AS classification_id,
                    s.security_id,
                    s.primary_symbol AS symbol,
                    CAST(NULL AS VARCHAR) AS node_code,
                    coalesce(
                        max(sic.effective_valid_to) FILTER (
                            WHERE sic.effective_valid_to <= current_date
                        ),
                        s.first_seen_date,
                        DATE '1900-01-01'
                    ) AS valid_from,
                    min(sic.valid_from) FILTER (
                        WHERE sic.valid_from > current_date
                    ) AS valid_to,
                    coalesce(s.first_seen_date, DATE '1900-01-01') AS as_of_date,
                    s.source_loaded_at AS available_at,
                    s.source_loaded_at,
                    'ALL' AS industry_template,
                    'default_all_no_current_sic' AS match_reason
                FROM securities s
                LEFT JOIN bounded_sic sic ON sic.security_id = s.security_id
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM bounded_sic current_sic
                    WHERE current_sic.security_id = s.security_id
                      AND current_sic.valid_from <= current_date
                      AND coalesce(current_sic.effective_valid_to, DATE '9999-12-31') > current_date
                )
                GROUP BY
                    s.security_id,s.primary_symbol,s.first_seen_date,s.source_loaded_at
            ),
            base_routes AS (
                SELECT * FROM routed_sic
                UNION ALL BY NAME
                SELECT * FROM inferred_backcast_routes
                UNION ALL BY NAME
                SELECT * FROM fallback_routes
            ),
            sequenced AS (
                SELECT
                    base_routes.*,
                    sha256(concat_ws('|', ?, security_id, CAST(valid_from AS VARCHAR)))
                        AS route_revision_group_id,
                    row_number() OVER route_window AS revision_sequence,
                    count(*) OVER route_window AS revision_count,
                    lag(industry_template) OVER route_window AS previous_industry_template,
                    lead(available_at) OVER route_window AS knowledge_valid_to
                FROM base_routes
                WINDOW route_window AS (
                    PARTITION BY security_id,valid_from
                    ORDER BY available_at,source_loaded_at,classification_id NULLS FIRST
                    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
                )
            )
            SELECT
                sha256(concat_ws('|', ?, security_id, industry_template,
                                 CAST(valid_from AS VARCHAR),CAST(available_at AS VARCHAR),
                                 coalesce(classification_id,''))) AS route_id,
                ? AS source,
                security_id,
                symbol,
                industry_template,
                CASE WHEN node_code IS NULL THEN NULL ELSE 'SIC' END AS matched_taxonomy,
                node_code AS matched_node_code,
                match_reason,
                valid_from,
                valid_to,
                as_of_date,
                available_at,
                revision_sequence = revision_count AS is_latest_revision,
                ? AS run_id,
                source_loaded_at,
                route_revision_group_id,
                revision_sequence,
                revision_count,
                previous_industry_template,
                CASE WHEN revision_sequence = 1 THEN 'original' ELSE 'restated' END AS update_type,
                knowledge_valid_to
            FROM sequenced
            """,
            [options.source, options.source, options.source, options.run_id],
        )
    row = store.con.execute(
        "SELECT count(*) FROM entity_industry_template WHERE source = ?",
        [options.source],
    ).fetchone()
    if row is None:
        raise RuntimeError("industry-template route count query returned no row")
    return int(row[0])


def refresh_industry_template_coverage(
    store: DuckDBStore,
    options: IndustryTemplateOptions | None = None,
) -> int:
    """Refresh per-template item coverage and missing/not-available counts."""

    options = options or IndustryTemplateOptions()
    seed_industry_templates(store)
    with store.transaction():
        store.con.execute("DELETE FROM industry_template_coverage WHERE source = ?", [options.source])
        store.con.execute(
            """
            INSERT INTO industry_template_coverage (
                coverage_id, source, industry_template, routed_entity_count,
                required_item_count, present_item_count, not_available_item_count,
                missing_item_count, missing_item_ids_json, as_of_date, available_at,
                is_latest_revision, run_id, source_loaded_at
            )
            WITH routed AS (
                SELECT industry_template, count(DISTINCT security_id) AS routed_entity_count
                FROM entity_industry_template
                WHERE source = ? AND is_latest_revision
                GROUP BY 1
            ),
            required AS (
                SELECT template_code, item_id, not_available
                FROM industry_template_item
                WHERE requirement_level = 'required'
                  AND (valid_to IS NULL OR valid_to > current_date)
            ),
            present AS (
                SELECT industry_template AS template_code, item_id
                FROM fundamental_statement_map
                WHERE is_active
                  AND item_id IS NOT NULL
                GROUP BY 1, 2
            ),
            scored AS (
                SELECT
                    r.template_code,
                    r.item_id,
                    r.not_available,
                    p.item_id IS NOT NULL AS is_present
                FROM required r
                LEFT JOIN present p
                  ON p.template_code = r.template_code
                 AND p.item_id = r.item_id
            ),
            grouped AS (
                SELECT
                    template_code,
                    count(*) AS required_item_count,
                    sum(CASE WHEN is_present THEN 1 ELSE 0 END) AS present_item_count,
                    sum(CASE WHEN not_available THEN 1 ELSE 0 END) AS not_available_item_count,
                    sum(CASE WHEN NOT is_present AND NOT not_available THEN 1 ELSE 0 END) AS missing_item_count,
                    CAST(to_json(list(item_id ORDER BY item_id) FILTER (
                        WHERE NOT is_present AND NOT not_available
                    )) AS VARCHAR) AS missing_item_ids_json
                FROM scored
                GROUP BY 1
            )
            SELECT
                sha256(concat_ws('|', ?, g.template_code, CAST(current_date AS VARCHAR))) AS coverage_id,
                ? AS source,
                g.template_code AS industry_template,
                coalesce(r.routed_entity_count, 0) AS routed_entity_count,
                g.required_item_count,
                g.present_item_count,
                g.not_available_item_count,
                g.missing_item_count,
                coalesce(g.missing_item_ids_json, '[]') AS missing_item_ids_json,
                current_date AS as_of_date,
                now() AS available_at,
                true AS is_latest_revision,
                ? AS run_id,
                now() AS source_loaded_at
            FROM grouped g
            LEFT JOIN routed r ON r.industry_template = g.template_code
            """,
            [options.source, options.source, options.source, options.run_id],
        )
    row = store.con.execute(
        "SELECT count(*) FROM industry_template_coverage WHERE source = ?",
        [options.source],
    ).fetchone()
    if row is None:
        raise RuntimeError("industry-template coverage count query returned no row")
    return int(row[0])


def run_industry_template_refresh(
    store: DuckDBStore,
    options: IndustryTemplateOptions | None = None,
) -> dict[str, Any]:
    """Seed templates, route entities, refresh coverage, and record a small check row."""

    options = options or IndustryTemplateOptions()
    store.initialize()
    routed = refresh_entity_industry_templates(store, options)
    coverage = refresh_industry_template_coverage(store, options)
    quality_check(
        store,
        dataset_id="industry_template",
        table_name="industry_template_coverage",
        check_name="industry_template_rows_materialized",
        status="passed" if coverage >= len(INDUSTRY_TEMPLATES) else "failed",
        observed_value=float(coverage),
        threshold_value=float(len(INDUSTRY_TEMPLATES)),
        details={"routed_entities": routed},
    )
    return {"routed_entities": routed, "coverage_rows": coverage}
