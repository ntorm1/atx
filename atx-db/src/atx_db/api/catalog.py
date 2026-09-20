"""Stable customer-facing dataset and record-schema contract.

The warehouse contains many internal and intermediate tables.  The API exposes only
the curated schemas declared here; callers can never provide a table name or SQL
expression.  This is both the public contract and the query allow-list.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from typing import Final


@dataclass(frozen=True)
class FieldSpec:
    name: str
    source_column: str
    data_type: str
    description: str
    unit: str | None = None
    nullable: bool = True
    filterable: bool = False


@dataclass(frozen=True)
class RecordSchema:
    dataset: str
    code: str
    version: str
    title: str
    description: str
    source_table: str
    time_column: str
    natural_key: tuple[str, ...]
    fields: tuple[FieldSpec, ...]
    item_column: str | None = None
    basis_column: str | None = None
    supports_vintages: bool = True
    max_sync_rows: int = 50_000
    coverage_item_columns: tuple[str, ...] = ()

    @property
    def field_names(self) -> tuple[str, ...]:
        return tuple(field.name for field in self.fields)

    def field(self, name: str) -> FieldSpec:
        for field in self.fields:
            if field.name == name:
                return field
        raise KeyError(name)


@dataclass(frozen=True)
class DatasetSpec:
    code: str
    version: str
    title: str
    description: str
    asset_class: str
    region: str
    entitlement: str
    default_schema: str
    schemas: tuple[RecordSchema, ...]

    def schema(self, code: str) -> RecordSchema:
        for schema in self.schemas:
            if schema.code == code:
                return schema
        raise KeyError(code)


_PIT_FIELDS: Final[tuple[FieldSpec, ...]] = (
    FieldSpec("as_of_date", "as_of_date", "date", "Economic observation date."),
    FieldSpec(
        "available_at",
        "available_at",
        "timestamp",
        "Earliest timestamp at which ATX could have delivered this revision.",
    ),
    FieldSpec(
        "source_loaded_at",
        "source_loaded_at",
        "timestamp",
        "Timestamp at which the source observation entered the warehouse.",
    ),
    FieldSpec("run_id", "run_id", "string", "Lineage identifier for the producing run."),
)


REPORTED_FUNDAMENTALS_SCHEMA = RecordSchema(
    dataset="ATX.US.FUNDAMENTALS",
    code="reported",
    version="1.1.0",
    title="Canonical reported US equity fundamentals",
    description=(
        "As-filed SEC facts mapped to comparable ATX items while retaining taxonomy, "
        "concept, accession, revision, and value-change evidence."
    ),
    source_table="fundamental_statement_points",
    time_column="period_end",
    natural_key=("revision_group_id",),
    item_column="canonical_metric",
    basis_column="fiscal_period",
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("symbol", "symbol", "string", "Ticker reported for the observation."),
        FieldSpec("cik", "cik", "string", "SEC Central Index Key.", nullable=False),
        FieldSpec(
            "item", "canonical_metric", "string", "Canonical ATX statement item.", nullable=False, filterable=True
        ),
        FieldSpec("item_id", "item_id", "int32", "Versioned canonical item identifier."),
        FieldSpec("statement", "statement_type", "string", "Canonical financial statement.", nullable=False),
        FieldSpec("section", "statement_section", "string", "Canonical statement section.", nullable=False),
        FieldSpec("period_type", "period_type", "string", "Instant or duration fact.", nullable=False),
        FieldSpec("period_start", "period_start", "date", "Fiscal period start; null for instant facts."),
        FieldSpec("period_end", "period_end", "date", "Fiscal period end.", nullable=False),
        FieldSpec("fiscal_year", "fiscal_year", "int32", "Issuer fiscal year."),
        FieldSpec("fiscal_period", "fiscal_period", "string", "Issuer fiscal period; this is the basis filter."),
        FieldSpec("form", "form", "string", "SEC form type."),
        FieldSpec("value", "value", "float64", "Sign-normalized canonical value."),
        FieldSpec("raw_value", "raw_value", "float64", "As-filed numeric value."),
        FieldSpec("unit", "unit", "string", "As-filed unit.", nullable=False),
        FieldSpec("unit_type", "unit_type", "string", "Canonical unit family.", nullable=False),
        FieldSpec(
            "normal_balance", "normal_balance", "string", "Debit, credit, or neutral sign convention.", nullable=False
        ),
        FieldSpec("taxonomy", "taxonomy", "string", "Source XBRL taxonomy.", nullable=False),
        FieldSpec("concept", "concept", "string", "Source XBRL concept.", nullable=False),
        FieldSpec("accession_number", "accession_number", "string", "SEC accession number.", nullable=False),
        FieldSpec("filed_date", "filed_date", "date", "EDGAR filing date."),
        FieldSpec("revision_sequence", "revision_sequence", "int32", "One-based revision sequence.", nullable=False),
        FieldSpec(
            "revision_count", "revision_count", "int32", "Visible revisions in the source chain.", nullable=False
        ),
        FieldSpec(
            "is_value_changed",
            "is_value_changed",
            "boolean",
            "Whether this revision changed the prior value.",
            nullable=False,
        ),
        FieldSpec("previous_value", "previous_value", "float64", "Prior sign-normalized value."),
        FieldSpec("value_delta", "value_delta", "float64", "Current less prior normalized value."),
        FieldSpec(
            "value_delta_percent", "value_delta_percent", "float64", "Revision delta divided by absolute prior value."
        ),
        FieldSpec("source", "source", "string", "ATX source adapter.", nullable=False),
        FieldSpec("source_url", "source_url", "string", "Authoritative filing URL.", nullable=False),
        FieldSpec(
            "revision_group_id", "revision_group_id", "string", "Stable revision-chain identifier.", nullable=False
        ),
        *_PIT_FIELDS,
    ),
)


FUNDAMENTALS_SCHEMA = RecordSchema(
    dataset="ATX.US.FUNDAMENTALS",
    code="standardized",
    version="2.0.0",
    title="Standardized US equity fundamentals",
    description=(
        "Comparable annual, quarterly, and TTM statement items with filing lineage and bitemporal revision visibility."
    ),
    source_table="fundamental_standardized",
    time_column="period_end",
    natural_key=("security_id", "item_id", "basis", "period_end"),
    item_column="canonical_code",
    basis_column="basis",
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("symbol", "symbol", "string", "Ticker reported for the observation."),
        FieldSpec("cik", "cik", "string", "SEC Central Index Key."),
        FieldSpec(
            "item", "canonical_code", "string", "Canonical ATX fundamental item code.", nullable=False, filterable=True
        ),
        FieldSpec("item_id", "item_id", "int32", "Versioned canonical item identifier.", nullable=False),
        FieldSpec(
            "basis",
            "basis",
            "string",
            "Fiscal basis such as annual, quarterly, or TTM.",
            nullable=False,
            filterable=True,
        ),
        FieldSpec("period_start", "period_start", "date", "Fiscal period start; null for instant items."),
        FieldSpec("period_end", "period_end", "date", "Fiscal period end.", nullable=False),
        FieldSpec("fiscal_year", "fiscal_year", "int32", "Issuer fiscal year."),
        FieldSpec("fiscal_period", "fiscal_period", "string", "Issuer fiscal-period label."),
        FieldSpec("value", "value", "float64", "Standardized value.", nullable=False),
        FieldSpec("unit", "unit", "string", "As-reported measurement unit or currency unit."),
        FieldSpec("unit_type", "unit_type", "string", "Canonical unit family."),
        FieldSpec("accession_number", "source_accession", "string", "Source SEC accession number."),
        FieldSpec("filed_date", "filed_date", "date", "EDGAR filing date."),
        FieldSpec("source", "source", "string", "ATX source adapter."),
        FieldSpec("upstream_source", "upstream_source", "string", "Raw upstream provider or filing source."),
        FieldSpec("standardization_rule", "rule_id", "string", "Rule that mapped source facts to the canonical item."),
        FieldSpec("revision_group_id", "revision_group_id", "string", "Stable standardized revision chain identifier."),
        FieldSpec("revision_sequence", "revision_sequence", "int32", "One-based revision sequence."),
        FieldSpec("revision_count", "revision_count", "int32", "Total revisions currently present in the chain."),
        FieldSpec("is_value_changed", "is_value_changed", "boolean", "Whether this revision changed the preceding value."),
        FieldSpec("previous_value", "previous_value", "float64", "Value in the preceding standardized revision."),
        FieldSpec("value_delta", "value_delta", "float64", "Current value less the preceding value."),
        FieldSpec("value_delta_percent", "value_delta_percent", "float64", "Revision delta divided by the absolute preceding value."),
        FieldSpec("update_type", "update_type", "string", "Original or restated standardized observation."),
        FieldSpec("valid_to", "valid_to", "timestamp", "Exclusive timestamp at which a successor revision became available."),
        FieldSpec(
            "input_codes_json",
            "input_codes_json",
            "json",
            "Source concept codes used to produce the value.",
            nullable=False,
        ),
        FieldSpec(
            "input_item_ids_json", "input_item_ids_json", "json", "Canonical input item identifiers.", nullable=False
        ),
        *_PIT_FIELDS,
    ),
)


INDUSTRY_FUNDAMENTALS_SCHEMA = RecordSchema(
    dataset="ATX.US.FUNDAMENTALS",
    code="industry-standardized",
    version="1.0.0",
    title="Industry-routed standardized US equity fundamentals",
    description=(
        "Revision-complete standardized observations enriched with PIT bank, insurance, "
        "REIT, utility, broker-dealer, or commercial statement-template routing."
    ),
    source_table="v_fundamental_industry_standardized",
    time_column="period_end",
    natural_key=("security_id", "item_id", "basis", "period_end"),
    item_column="canonical_code",
    basis_column="basis",
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("symbol", "symbol", "string", "Ticker reported for the observation."),
        FieldSpec("cik", "cik", "string", "SEC Central Index Key."),
        FieldSpec(
            "item", "canonical_code", "string", "Canonical ATX fundamental item code.",
            nullable=False, filterable=True,
        ),
        FieldSpec("item_id", "item_id", "int32", "Versioned canonical item identifier.", nullable=False),
        FieldSpec("basis", "basis", "string", "Annual, quarterly, instant, or TTM basis.",
                  nullable=False, filterable=True),
        FieldSpec("period_start", "period_start", "date", "Fiscal period start."),
        FieldSpec("period_end", "period_end", "date", "Fiscal period end.", nullable=False),
        FieldSpec("fiscal_year", "fiscal_year", "int32", "Issuer fiscal year."),
        FieldSpec("fiscal_period", "fiscal_period", "string", "Issuer fiscal-period label."),
        FieldSpec("value", "value", "float64", "Standardized value.", nullable=False),
        FieldSpec("unit", "unit", "string", "As-reported measurement unit or currency unit."),
        FieldSpec("unit_type", "unit_type", "string", "Canonical unit family."),
        FieldSpec(
            "industry_template", "industry_template", "string",
            "PIT statement template: ALL, BK, IS, RT, UT, or BD.", nullable=False,
        ),
        FieldSpec("template_label", "template_label", "string", "Human-readable statement-template label."),
        FieldSpec("vendor_profile", "vendor_profile", "string", "Comparable vendor industry profile."),
        FieldSpec("accounting_class", "accounting_class", "string", "Industrial or financial accounting class."),
        FieldSpec(
            "requirement_level", "requirement_level", "string",
            "Required, optional, or supplemental role within the selected template.", nullable=False,
        ),
        FieldSpec(
            "template_not_available", "template_not_available", "boolean",
            "Whether the template normally requires a non-SEC/vendor or regulatory source.", nullable=False,
        ),
        FieldSpec("template_match_reason", "template_match_reason", "string", "Auditable routing reason."),
        FieldSpec("matched_taxonomy", "matched_taxonomy", "string", "Classification taxonomy used for routing."),
        FieldSpec("matched_node_code", "matched_node_code", "string", "Classification code used for routing."),
        FieldSpec("route_available_at", "route_available_at", "timestamp", "When the selected route became known."),
        FieldSpec(
            "standardized_available_at", "standardized_available_at", "timestamp",
            "When the selected value revision became known.", nullable=False,
        ),
        FieldSpec("accession_number", "source_accession", "string", "Source SEC accession number."),
        FieldSpec("filed_date", "filed_date", "date", "EDGAR filing date."),
        FieldSpec("source", "source", "string", "ATX standardization adapter.", nullable=False),
        FieldSpec("upstream_source", "upstream_source", "string", "Raw upstream source or derivation."),
        FieldSpec("standardization_rule", "rule_id", "string", "Rule producing the canonical value."),
        FieldSpec("revision_group_id", "industry_revision_group_id", "string", "Industry-product revision chain."),
        FieldSpec("revision_sequence", "revision_sequence", "int32", "One-based product revision sequence."),
        FieldSpec("revision_count", "revision_count", "int32", "Product revisions currently present."),
        FieldSpec("is_value_changed", "is_value_changed", "boolean", "Whether this event changed the value."),
        FieldSpec("previous_value", "previous_value", "float64", "Value at the preceding product event."),
        FieldSpec("value_delta", "value_delta", "float64", "Value less its preceding product-event value."),
        FieldSpec("value_delta_percent", "value_delta_percent", "float64", "Delta divided by absolute prior value."),
        FieldSpec(
            "update_type", "update_type", "string",
            "Original, restated, classification_update, or metadata_update event.", nullable=False,
        ),
        FieldSpec("valid_to", "valid_to", "timestamp", "Exclusive successor product-event timestamp."),
        FieldSpec("input_codes_json", "input_codes_json", "json", "Source concept lineage.", nullable=False),
        FieldSpec("input_item_ids_json", "input_item_ids_json", "json", "Canonical input item lineage.", nullable=False),
        *_PIT_FIELDS,
    ),
)


FUNDAMENTAL_RECONCILIATION_SCHEMA = RecordSchema(
    dataset="ATX.US.FUNDAMENTALS",
    code="reconciliation",
    version="1.2.0",
    title="Point-in-time fundamental accounting reconciliation",
    description=(
        "Revision-complete accounting-identity results with tolerances, statuses, "
        "industry applicability, and exact standardized-input lineage."
    ),
    source_table="fundamental_reconciliation_serving",
    time_column="period_end",
    natural_key=("security_id", "rule_id", "basis", "period_end"),
    item_column="rule_id",
    basis_column="basis",
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("symbol", "symbol", "string", "Ticker reported for the evaluated inputs."),
        FieldSpec("cik", "cik", "string", "SEC Central Index Key."),
        FieldSpec("item", "rule_id", "string", "Governed reconciliation rule identifier.",
                  nullable=False, filterable=True),
        FieldSpec("rule_version", "rule_version", "string", "Semantic version of the rule.", nullable=False),
        FieldSpec("label", "label", "string", "Human-readable reconciliation label.", nullable=False),
        FieldSpec("statement", "statement_type", "string", "Statement family evaluated.", nullable=False),
        FieldSpec("industry_template", "industry_template", "string", "PIT applicable industry template."),
        FieldSpec("basis", "basis", "string", "Annual, quarterly, instant, or TTM basis.",
                  nullable=False, filterable=True),
        FieldSpec("period_start", "period_start", "date", "Fiscal period start."),
        FieldSpec("period_end", "period_end", "date", "Fiscal period end.", nullable=False),
        FieldSpec("fiscal_year", "fiscal_year", "int32", "Issuer fiscal year."),
        FieldSpec("fiscal_period", "fiscal_period", "string", "Issuer fiscal-period label."),
        FieldSpec("lhs_value", "lhs_value", "float64", "Weighted left-hand side of the identity."),
        FieldSpec("rhs_value", "rhs_value", "float64", "Weighted right-hand side of the identity."),
        FieldSpec("residual", "residual", "float64", "Left-hand side less right-hand side."),
        FieldSpec("residual_percent", "residual_percent", "float64", "Residual over the larger absolute side."),
        FieldSpec("tolerance", "tolerance", "float64", "Effective absolute-or-relative tolerance."),
        FieldSpec(
            "status", "status", "string",
            "Reconciled, mismatch, diagnostic_difference, or not_applicable.", nullable=False,
        ),
        FieldSpec(
            "is_applicable", "is_applicable", "bool",
            "Whether the rule applies to the PIT industry template.", nullable=False,
        ),
        FieldSpec(
            "mismatch_severity", "mismatch_severity", "string",
            "Error or diagnostic interpretation for an out-of-tolerance result.", nullable=False,
        ),
        FieldSpec("unit_type", "unit_type", "string", "Canonical measurement family.", nullable=False),
        FieldSpec("citation", "citation", "string", "Authoritative rule or XBRL guidance source."),
        FieldSpec("description", "description", "string", "Governed rule definition."),
        FieldSpec(
            "input_filing_status", "input_filing_status", "string",
            "Single-filing, mixed-vintage, or unknown accession scope for the selected inputs.",
            nullable=False,
        ),
        FieldSpec(
            "input_accession_count", "input_accession_count", "int32",
            "Distinct non-null filing accessions represented by the selected inputs.", nullable=False,
        ),
        FieldSpec(
            "input_accessions_json", "input_accessions_json", "json",
            "Sorted distinct filing accessions represented by the selected inputs.", nullable=False,
        ),
        FieldSpec(
            "context_verification_status", "context_verification_status", "string",
            "Whether filing-instance facts verify that all inputs share one XBRL context and unit.",
            nullable=False,
        ),
        FieldSpec(
            "verified_filing_context_id", "verified_filing_context_id", "string",
            "Matched filing-instance context identifier when same-context verification succeeds.",
        ),
        FieldSpec(
            "context_evidence_json", "context_evidence_json", "json",
            "Filing, context, unit, and dimensional evidence supporting verification status.",
            nullable=False,
        ),
        FieldSpec(
            "mismatch_reason", "mismatch_reason", "string",
            "Reasoned interpretation of the numerical status and input-context evidence.",
            nullable=False,
        ),
        FieldSpec(
            "is_hard_failure", "is_hard_failure", "boolean",
            "True only for an error-severity mismatch verified in one filing context and unit.",
            nullable=False,
        ),
        FieldSpec(
            "extension_mapping_applied", "extension_mapping_applied", "boolean",
            "Whether a governed issuer-extension mapping completed the filing-context equation.",
            nullable=False,
        ),
        FieldSpec(
            "extension_inputs_json", "extension_inputs_json", "json",
            "Approved issuer-extension facts applied after standardized-input reconciliation.",
            nullable=False,
        ),
        FieldSpec(
            "input_standardized_ids_json", "input_standardized_ids_json", "json",
            "Ordered standardized observation identifiers used by the check.", nullable=False,
        ),
        FieldSpec("input_item_ids_json", "input_item_ids_json", "json", "Ordered input item identifiers.",
                  nullable=False),
        FieldSpec("input_values_json", "input_values_json", "json", "Ordered weighted input evidence.",
                  nullable=False),
        FieldSpec("revision_group_id", "reconciliation_group_id", "string", "Stable result revision chain."),
        FieldSpec("revision_sequence", "revision_sequence", "int32", "One-based result revision sequence."),
        FieldSpec("revision_count", "revision_count", "int32", "Result revisions currently present."),
        FieldSpec("previous_status", "previous_status", "string", "Status at the preceding result event."),
        FieldSpec(
            "update_type", "update_type", "string",
            "Original, restated, classification_update, or metadata_update event.",
            nullable=False,
        ),
        FieldSpec("valid_to", "valid_to", "timestamp", "Exclusive successor event timestamp."),
        *_PIT_FIELDS,
    ),
)


RATIOS_SCHEMA = RecordSchema(
    dataset="ATX.US.FUNDAMENTALS",
    code="ratios",
    version="1.0.0",
    title="Point-in-time financial ratios",
    description="Formula-governed financial ratios with numerator, denominator, and revision lineage.",
    source_table="fundamental_ratios",
    time_column="period_end",
    natural_key=("security_id", "ratio_code", "basis", "period_end"),
    item_column="ratio_code",
    basis_column="basis",
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("symbol", "symbol", "string", "Ticker reported for the observation."),
        FieldSpec("cik", "cik", "string", "SEC Central Index Key."),
        FieldSpec("item", "ratio_code", "string", "Governed ratio or score code.", nullable=False, filterable=True),
        FieldSpec("category", "ratio_category", "string", "Ratio family.", nullable=False),
        FieldSpec("kind", "ratio_kind", "string", "Ratio, growth metric, or composite-score kind.", nullable=False),
        FieldSpec(
            "basis",
            "basis",
            "string",
            "Fiscal basis such as annual, quarterly, or TTM.",
            nullable=False,
            filterable=True,
        ),
        FieldSpec("period_start", "period_start", "date", "Fiscal period start."),
        FieldSpec("period_end", "period_end", "date", "Fiscal period end.", nullable=False),
        FieldSpec("fiscal_year", "fiscal_year", "int32", "Issuer fiscal year."),
        FieldSpec("fiscal_period", "fiscal_period", "string", "Issuer fiscal-period label."),
        FieldSpec("value", "value", "float64", "Computed ratio value."),
        FieldSpec("unit", "unit", "string", "Output unit.", nullable=False),
        FieldSpec("numerator_item", "numerator_code", "string", "Canonical numerator code."),
        FieldSpec("numerator_value", "numerator_value", "float64", "Point-in-time numerator value."),
        FieldSpec("denominator_item", "denominator_code", "string", "Canonical denominator code."),
        FieldSpec("denominator_value", "denominator_value", "float64", "Point-in-time denominator value."),
        FieldSpec(
            "is_meaningful",
            "is_meaningful",
            "boolean",
            "Whether formula guards consider the value economically meaningful.",
            nullable=False,
        ),
        FieldSpec("accession_number", "source_accession", "string", "Source SEC accession number."),
        FieldSpec("filed_date", "filed_date", "date", "EDGAR filing date."),
        FieldSpec("source", "source", "string", "ATX source adapter.", nullable=False),
        FieldSpec("input_codes_json", "input_codes_json", "json", "Formula input codes."),
        FieldSpec("input_item_ids_json", "input_item_ids_json", "json", "Formula input item identifiers."),
        *_PIT_FIELDS,
    ),
)


RESTATEMENTS_SCHEMA = RecordSchema(
    dataset="ATX.US.FUNDAMENTALS",
    code="restatements",
    version="1.0.0",
    title="US equity fundamental restatement events",
    description=(
        "One immutable event per standardized revision that changed a previously "
        "published value, with the restating and superseded filings, first-reported "
        "baseline, and point-in-time availability of both vintages."
    ),
    source_table="v_fundamental_restatement_events",
    time_column="period_end",
    natural_key=("revision_group_id", "revision_sequence"),
    item_column="canonical_code",
    basis_column="basis",
    fields=(
        FieldSpec(
            "event_id",
            "restatement_event_id",
            "string",
            "Stable identifier of the restating standardized revision.",
            nullable=False,
        ),
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("symbol", "symbol", "string", "Ticker reported for the observation."),
        FieldSpec("cik", "cik", "string", "SEC Central Index Key."),
        FieldSpec(
            "item", "canonical_code", "string", "Canonical ATX fundamental item code.", nullable=False, filterable=True
        ),
        FieldSpec("item_id", "item_id", "int32", "Versioned canonical item identifier.", nullable=False),
        FieldSpec(
            "basis",
            "basis",
            "string",
            "Fiscal basis such as annual, quarterly, or TTM.",
            nullable=False,
            filterable=True,
        ),
        FieldSpec("period_start", "period_start", "date", "Fiscal period start; null for instant items."),
        FieldSpec("period_end", "period_end", "date", "Fiscal period end.", nullable=False),
        FieldSpec("fiscal_year", "fiscal_year", "int32", "Issuer fiscal year."),
        FieldSpec("fiscal_period", "fiscal_period", "string", "Issuer fiscal-period label."),
        FieldSpec("unit", "unit", "string", "As-reported measurement unit or currency unit."),
        FieldSpec("unit_type", "unit_type", "string", "Canonical unit family."),
        FieldSpec("restated_value", "restated_value", "float64", "Value published by the restating revision.", nullable=False),
        FieldSpec("previous_value", "previous_value", "float64", "Value in the immediately superseded revision."),
        FieldSpec(
            "first_reported_value",
            "first_reported_value",
            "float64",
            "Value of the first standardized revision in the chain.",
        ),
        FieldSpec("value_delta", "value_delta", "float64", "Restated value less the superseded value."),
        FieldSpec(
            "value_delta_percent",
            "value_delta_percent",
            "float64",
            "Revision delta divided by the absolute superseded value.",
        ),
        FieldSpec(
            "cumulative_delta",
            "cumulative_delta",
            "float64",
            "Restated value less the first-reported value.",
        ),
        FieldSpec(
            "restating_accession",
            "restating_accession",
            "string",
            "SEC accession that published the restated value.",
        ),
        FieldSpec("restating_filed_date", "restating_filed_date", "date", "Filing date of the restating accession."),
        FieldSpec(
            "previous_accession",
            "previous_accession",
            "string",
            "SEC accession behind the superseded value.",
        ),
        FieldSpec(
            "previous_available_at",
            "previous_available_at",
            "timestamp",
            "Point-in-time availability of the superseded revision.",
        ),
        FieldSpec("revision_group_id", "revision_group_id", "string", "Stable standardized revision chain identifier.", nullable=False),
        FieldSpec("revision_sequence", "revision_sequence", "int32", "One-based revision sequence of the restating revision.", nullable=False),
        FieldSpec("revision_count", "revision_count", "int32", "Total revisions currently present in the chain."),
        FieldSpec("update_type", "update_type", "string", "Original or restated standardized observation."),
        FieldSpec(
            "is_latest_revision",
            "is_latest_revision",
            "boolean",
            "Whether the restating revision is currently the chain head.",
        ),
        *_PIT_FIELDS,
    ),
)


DAILY_BARS_SCHEMA = RecordSchema(
    dataset="ATX.US.EQUITIES",
    code="ohlcv-1d",
    version="1.0.0",
    title="US equity daily bars",
    description="Daily OHLCV, adjustment, share-count, and market-cap observations.",
    source_table="equity_daily_bars",
    time_column="trade_date",
    natural_key=("source", "security_id", "trade_date"),
    item_column=None,
    basis_column=None,
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("vendor_security_id", "vendor_security_id", "string", "Upstream vendor instrument identifier."),
        FieldSpec("symbol", "symbol", "string", "Ticker reported for the bar.", nullable=False),
        FieldSpec("trade_date", "trade_date", "date", "Trading session date.", nullable=False),
        FieldSpec("open", "open", "float64", "Session open price.", "USD"),
        FieldSpec("high", "high", "float64", "Session high price.", "USD"),
        FieldSpec("low", "low", "float64", "Session low price.", "USD"),
        FieldSpec("close", "close", "float64", "Session close price.", "USD"),
        FieldSpec("adjusted_close", "adjusted_close", "float64", "Split/dividend-adjusted close.", "USD"),
        FieldSpec("volume", "volume", "int64", "Reported share volume.", "shares"),
        FieldSpec("vwap", "vwap", "float64", "Volume-weighted average price.", "USD"),
        FieldSpec("dividend_amount", "dividend_amount", "float64", "Cash dividend amount.", "USD"),
        FieldSpec("split_factor", "split_factor", "float64", "Split adjustment factor.", "ratio"),
        FieldSpec("is_adjusted", "is_adjusted", "boolean", "Whether price fields are adjusted.", nullable=False),
        FieldSpec("shares_outstanding", "shares_outstanding", "int64", "Point-in-time shares outstanding.", "shares"),
        FieldSpec("market_cap_usd", "market_cap_usd", "float64", "Point-in-time market capitalization.", "USD"),
        FieldSpec("source", "source", "string", "ATX source adapter.", nullable=False),
        FieldSpec("as_of_date", "as_of_date", "date", "Economic observation date."),
        FieldSpec("available_at", "available_at", "timestamp", "Earliest deliverable timestamp."),
        FieldSpec("source_loaded_at", "source_loaded_at", "timestamp", "Warehouse load timestamp.", nullable=False),
        FieldSpec("run_id", "run_id", "string", "Lineage identifier for the producing run."),
    ),
)


DERIVED_METRICS_SCHEMA = RecordSchema(
    dataset="ATX.US.FUNDAMENTALS",
    code="derived-metrics",
    version="2.1.0",
    title="Point-in-time derived metrics",
    description=(
        "Ratios, per-share, growth, leverage, quality, investment and payout metrics computed "
        "by one declarative engine from standardized history. Each row is a filing-event state, "
        "including NULL invalidations. Rank states before filtering valid values. Legacy scopes "
        "are marked incomplete; modeled filing availability does not certify observation-time vintages."
    ),
    source_table="derived_metric_values",
    time_column="period_end",
    natural_key=("revision_group_id",),
    item_column="metric_code",
    basis_column="metric_window",
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("metric", "metric_code", "string", "Derived metric code.", nullable=False, filterable=True),
        FieldSpec(
            "window",
            "metric_window",
            "string",
            "Metric window: q, ttm, annual, instant, avg2 or daily.",
            nullable=False,
            filterable=True,
        ),
        FieldSpec("period_end", "period_end", "date", "Fiscal period end.", nullable=False),
        FieldSpec("value", "value", "float64", "Derived value; NULL for an invalid event state."),
        FieldSpec("derived_value_id", "derived_value_id", "string", "Stable typed event-state identity.", nullable=False),
        FieldSpec("value_status", "value_status", "string", "valid, zero_denominator, missing_input_or_domain, or nonfinite."),
        FieldSpec("history_status", "history_status", "string", "event_reconstructed or incomplete legacy_latest_only."),
        FieldSpec("value_origin", "value_origin", "string", "annual_fallback, annual_dependency, quarterly, instant, scalar, incomparable, unavailable, or legacy_unspecified."),
        FieldSpec("fiscal_period_start", "fiscal_period_start", "date", "Selected current duration start; NULL for balance/scalar inputs or missing span evidence."),
        FieldSpec("fiscal_period_end", "fiscal_period_end", "date", "Selected current operand endpoint, including an exact annual fallback endpoint."),
        FieldSpec("revision_group_id", "revision_group_id", "string", "Source/security/metric/window/bucket/definition identity."),
        FieldSpec("revision_sequence", "revision_sequence", "int64", "Event order within the revision group."),
        FieldSpec("revision_count", "revision_count", "int64", "Number of retained group states."),
        FieldSpec("valid_to", "valid_to", "timestamp", "Exclusive next state event; NULL for the last state."),
        FieldSpec("target_bucket", "target_bucket", "int64", "Stable calendar quarter bucket identity."),
        FieldSpec("definition_hash", "definition_hash", "string", "Fingerprint of formula and input contract."),
        FieldSpec("arithmetic_available_at", "arithmetic_available_at", "timestamp", "Arithmetic input clock; transition available_at may be later."),
        FieldSpec(
            "inputs_hash",
            "inputs_hash",
            "string",
            "SHA-256 over ordered typed frame lineage with actual selected input states, values and clocks.",
            nullable=False,
        ),
        FieldSpec("source", "source", "string", "ATX engine identifier.", nullable=False),
        FieldSpec("as_of_date", "as_of_date", "date", "Date of the modeled state transition."),
        FieldSpec("available_at", "available_at", "timestamp", "Modeled filing-event transition time, including invalidation and control flow."),
        FieldSpec("source_loaded_at", "source_loaded_at", "timestamp", "Warehouse reconstruction time; not historical delivery evidence."),
        FieldSpec("run_id", "run_id", "string", "Lineage identifier for the producing run."),
    ),
)


_MARKET_DAILY_METRICS: Final[tuple[tuple[str, str, str | None], ...]] = (
    ("market_cap", "Price times point-in-time shares outstanding.", "USD"),
    ("enterprise_value", "Market cap plus debt, preferred and minority less cash.", "USD"),
    ("pe_ttm", "Price to trailing earnings available to common.", None),
    ("pb", "Price to common book value.", None),
    ("ps_ttm", "Price to trailing revenue.", None),
    ("pcf_ttm", "Price to trailing operating cash flow.", None),
    ("ev_ebitda", "Enterprise value to trailing EBITDA.", None),
    ("ev_sales", "Enterprise value to trailing revenue.", None),
    ("fcf_yield", "Trailing free cash flow over market cap.", None),
    ("dividend_yield", "Trailing common dividends over market cap.", None),
    ("earnings_yield", "Trailing earnings available to common over market cap.", None),
    ("shareholder_yield", "Net payout plus net debt paydown over market cap.", None),
    ("net_payout_yield", "Dividends plus net buybacks over market cap.", None),
    ("total_payout_yield", "Gross dividends plus gross buybacks over market cap.", None),
    ("buyback_yield", "Net buybacks over market cap.", None),
    ("book_to_market", "Common book value over market cap.", None),
    ("rd_to_market_equity", "Trailing R&D expense over market cap.", None),
    ("gross_profit_to_ev", "Trailing gross profit over enterprise value.", None),
    ("cfo_to_ev", "Trailing operating cash flow over enterprise value.", None),
    ("ebit_to_ev", "Trailing operating income over enterprise value.", None),
    ("sales_to_ev", "Trailing revenue over enterprise value.", None),
    ("altman_z", "Altman Z-score with market equity.", None),
    ("total_return_1m", "Twenty-one-trading-day total return.", None),
    ("total_return_3m", "Sixty-three-trading-day total return.", None),
    ("total_return_6m", "One-hundred-twenty-six-trading-day total return.", None),
    ("total_return_12m", "Two-hundred-fifty-two-trading-day total return.", None),
    ("momentum_12_1", "Twelve-month total return skipping the last month.", None),
    ("realized_vol_60d", "Annualized sixty-day realized volatility.", None),
    ("realized_vol_252d", "Annualized two-hundred-fifty-two-day realized volatility.", None),
    ("dollar_volume_20d", "Twenty-day average daily dollar volume.", "USD"),
)


MARKET_DAILY_SCHEMA = RecordSchema(
    dataset="ATX.US.EQUITIES",
    code="market-daily-1d",
    version="1.1.0",
    title="Daily market and valuation panel",
    description=(
        "Daily prices, point-in-time shares, market cap, enterprise value, valuation multiples, "
        "total returns, momentum and realized volatility. Rebuilds select historical raw, DEI and "
        "derived states, retaining invalidations and newest-visible-period precedence; the 22:00 "
        "bar cutoff is modeled availability and does not certify historical vendor vintages."
    ),
    coverage_item_columns=tuple(name for name, _, _ in _MARKET_DAILY_METRICS),
    source_table="market_daily_metrics",
    time_column="trade_date",
    natural_key=("security_id", "trade_date"),
    supports_vintages=False,
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("symbol", "symbol", "string", "Ticker for the observation."),
        FieldSpec("trade_date", "trade_date", "date", "Exchange trading date.", nullable=False),
        FieldSpec("close", "close", "float64", "Unadjusted closing price.", "USD"),
        FieldSpec("adj_close", "adj_close", "float64", "Corporate-action adjusted close.", "USD"),
        FieldSpec("volume", "volume", "int64", "Share volume.", "shares"),
        FieldSpec("shares_outstanding", "shares_outstanding", "float64", "Point-in-time shares.", "shares"),
        FieldSpec("shares_source", "shares_source", "string", "dei or archive.", filterable=True),
        FieldSpec(
            "shares_reconciliation_ratio",
            "shares_reconciliation_ratio",
            "float64",
            "dei shares divided by archive shares on the same date.",
        ),
        *(
            FieldSpec(name, name, "float64", description, unit)
            for name, description, unit in _MARKET_DAILY_METRICS
        ),
        FieldSpec(
            "fundamental_available_at",
            "fundamental_available_at",
            "timestamp",
            "Max availability of the fundamentals joined into this row.",
        ),
        FieldSpec(
            "inputs_hash",
            "inputs_hash",
            "string",
            "SHA-256 over the price, share and fundamental-availability inputs.",
            nullable=False,
        ),
        FieldSpec("source", "source", "string", "ATX engine identifier.", nullable=False),
        *_PIT_FIELDS,
    ),
)


SECURITY_MASTER_SCHEMA = RecordSchema(
    dataset="ATX.US.FUNDAMENTALS",
    code="security-master",
    version="1.0.0",
    title="US equity security master",
    description=(
        "Current-state security spine with its open CIK, LEI and FIGI identifiers. "
        "CUSIP is internal-only by policy and is never part of this contract."
    ),
    source_table="v_security_master_public",
    time_column="as_of_date",
    natural_key=("security_id",),
    supports_vintages=False,
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("entity_id", "entity_id", "string", "Issuing entity key; 'CIK-<cik>' for SEC filers."),
        FieldSpec("issuer_id", "issuer_id", "string", "Issuer grouping key."),
        FieldSpec("primary_symbol", "primary_symbol", "string", "Current primary ticker."),
        FieldSpec("name", "name", "string", "Security name."),
        FieldSpec("asset_class", "asset_class", "string", "ATX asset class.", nullable=False),
        FieldSpec("country", "country", "string", "Country of listing.", nullable=False),
        FieldSpec("currency", "currency", "string", "Trading currency.", nullable=False),
        FieldSpec("active", "active", "boolean", "Whether the listing is currently active.", nullable=False),
        FieldSpec("cik", "cik", "string", "SEC Central Index Key.", filterable=True),
        FieldSpec("lei", "lei", "string", "Legal Entity Identifier (GLEIF).", filterable=True),
        FieldSpec("figi", "figi", "string", "Financial Instrument Global Identifier (OpenFIGI).", filterable=True),
        *_PIT_FIELDS,
    ),
)


UNIVERSE_SCHEMA = RecordSchema(
    dataset="ATX.US.EQUITIES",
    code="universe",
    version="1.0.0",
    title="US-listed equity universe membership",
    description=(
        "Interval-keyed point-in-time universe of US-listed common stock, ADRs, REITs and "
        "LPs. Members with no resolved CIK are present with has_cik=false."
    ),
    source_table="universe_us_listed_membership",
    time_column="valid_from",
    natural_key=("universe_id", "security_id", "valid_from"),
    fields=(
        FieldSpec("universe_id", "universe_id", "string", "Universe identifier.", nullable=False, filterable=True),
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("symbol", "symbol", "string", "Ticker at the start of the interval."),
        FieldSpec("valid_from", "valid_from", "date", "First session of the membership interval.", nullable=False),
        FieldSpec("valid_to", "valid_to", "date", "Last session of the interval; null while open."),
        FieldSpec("security_type", "security_type", "string", "common, ADR, REIT or LP.", nullable=False, filterable=True),
        FieldSpec("exchange_code", "exchange_code", "string", "XNYS, XNAS, XASE, ARCX or BATS.", nullable=False, filterable=True),
        FieldSpec("has_cik", "has_cik", "boolean", "Whether the security resolves to an SEC filer.", nullable=False, filterable=True),
        FieldSpec("cik", "cik", "string", "SEC Central Index Key when resolved."),
        FieldSpec("market_cap_decile", "market_cap_decile", "int32", "Market-cap decile AT valid_from only."),
        FieldSpec("reason", "reason", "string", "member or member_no_cik.", nullable=False),
        FieldSpec("decision_count", "decision_count", "int32", "Sessions backing the interval.", nullable=False),
        *_PIT_FIELDS,
    ),
)


DELISTINGS_SCHEMA = RecordSchema(
    dataset="ATX.US.EQUITIES",
    code="delistings",
    version="1.0.0",
    title="US equity delisting events",
    description=(
        "One delisting event per security and delist date, attributed to the "
        "highest-precedence public evidence and carrying its terminal-return state."
    ),
    source_table="delisting_events",
    time_column="delist_date",
    # Unresolved securities retain their symbol as event identity.
    natural_key=("source", "security_id", "symbol", "delist_date"),
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier."),
        FieldSpec("symbol", "symbol", "string", "Ticker at delisting.", nullable=False),
        FieldSpec("delist_date", "delist_date", "date", "Date trading ceased.", nullable=False),
        FieldSpec("delist_code", "delist_code", "string", "Warehouse delist code.", nullable=False, filterable=True),
        FieldSpec("delist_reason", "delist_reason", "string", "Attributed reason category.", nullable=False, filterable=True),
        FieldSpec("delisting_return", "delisting_return", "float64", "Terminal return when known.", "ratio"),
        FieldSpec("delisting_return_type", "delisting_return_type", "string", "OBSERVED, POLICY or UNOBSERVED.", nullable=False),
        FieldSpec("is_return_imputed", "is_return_imputed", "boolean", "Whether the terminal return is a policy convention.", nullable=False),
        FieldSpec("return_policy", "return_policy", "string", "Policy code that produced the return.", nullable=False),
        FieldSpec("evidence_source", "evidence_source", "string", "Evidence family.", nullable=False),
        FieldSpec("evidence_confidence", "evidence_confidence", "string", "high, medium or low.", nullable=False),
        FieldSpec("inferred_from_absence", "inferred_from_absence", "boolean", "Whether the event was inferred rather than filed.", nullable=False),
        FieldSpec("source", "source", "string", "ATX source adapter.", nullable=False, filterable=True),
        *_PIT_FIELDS,
    ),
)


DATASETS: Final[tuple[DatasetSpec, ...]] = (
    DatasetSpec(
        code="ATX.US.FUNDAMENTALS",
        version="1.0.0",
        title="ATX US Fundamentals",
        description="Normalized US equity financial statements, ratios, vintages, and lineage.",
        asset_class="equity",
        region="US",
        entitlement="us_fundamentals",
        default_schema="reported",
        schemas=(
            REPORTED_FUNDAMENTALS_SCHEMA,
            FUNDAMENTALS_SCHEMA,
            INDUSTRY_FUNDAMENTALS_SCHEMA,
            FUNDAMENTAL_RECONCILIATION_SCHEMA,
            RATIOS_SCHEMA,
            RESTATEMENTS_SCHEMA,
            DERIVED_METRICS_SCHEMA,
            SECURITY_MASTER_SCHEMA,
        ),
    ),
    DatasetSpec(
        code="ATX.US.EQUITIES",
        version="1.0.0",
        title="ATX US Equities",
        description="US equity reference and end-of-day market observations.",
        asset_class="equity",
        region="US",
        entitlement="us_equities_eod",
        default_schema="ohlcv-1d",
        schemas=(DAILY_BARS_SCHEMA, MARKET_DAILY_SCHEMA, UNIVERSE_SCHEMA, DELISTINGS_SCHEMA),
    ),
)


def get_dataset(code: str) -> DatasetSpec:
    for dataset in DATASETS:
        if dataset.code == code:
            return dataset
    raise KeyError(code)


def get_schema(dataset_code: str, schema_code: str) -> RecordSchema:
    return get_dataset(dataset_code).schema(schema_code)


def public_catalog() -> list[dict[str, object]]:
    return [
        {
            "dataset": dataset.code,
            "version": dataset.version,
            "title": dataset.title,
            "description": dataset.description,
            "asset_class": dataset.asset_class,
            "region": dataset.region,
            "default_schema": dataset.default_schema,
            "schemas": [schema.code for schema in dataset.schemas],
        }
        for dataset in DATASETS
    ]


def _public_schema_contract(schema: RecordSchema) -> dict[str, object]:
    return {
        "dataset": schema.dataset,
        "schema": schema.code,
        "version": schema.version,
        "title": schema.title,
        "description": schema.description,
        "time_field": schema.time_column,
        "natural_key": list(schema.natural_key),
        "supports_vintages": schema.supports_vintages,
        "max_sync_rows": schema.max_sync_rows,
        "fields": [
            {
                "name": field.name,
                "data_type": field.data_type,
                "description": field.description,
                "unit": field.unit,
                "nullable": field.nullable,
                "filterable": field.filterable,
            }
            for field in schema.fields
        ],
    }


def _record_schema_sha256(schema: RecordSchema) -> str:
    """Hash the complete customer-visible record contract deterministically."""

    payload = json.dumps(
        _public_schema_contract(schema),
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def public_schema(schema: RecordSchema) -> dict[str, object]:
    payload = _public_schema_contract(schema)
    payload["schema_sha256"] = _record_schema_sha256(schema)
    return payload
