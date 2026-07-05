"""PF3-S6 migration bodies: ratio formula catalog expansion."""

from __future__ import annotations

import hashlib
import json

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import (
    _catalog_fields_for_tables,
    _formula_registry_catalog_view,
    _formula_registry_schema_catalog,
)
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _s6_1_growth_formula_rows() -> tuple[tuple[object, ...], ...]:
    metrics = (
        ("revenue", "revenue", "ttm", "[1001]", "trailing-twelve-month revenue"),
        ("net_income", "net_income", "ttm", "[1031]", "trailing-twelve-month net income"),
        ("operating_income", "operating_income", "ttm", "[1014]", "trailing-twelve-month operating income"),
        (
            "operating_cash_flow",
            "operating_cash_flow",
            "ttm",
            "[1301]",
            "trailing-twelve-month operating cash flow",
        ),
        ("free_cash_flow", "free_cash_flow", "ttm", "[1301, 1305]", "trailing-twelve-month free cash flow"),
        ("assets", "assets", "instant", "[1101]", "total assets"),
        ("equity", "stockholders_equity", "instant", "[1221]", "stockholders equity"),
    )
    rows: list[tuple[object, ...]] = []
    for prefix, metric_code, basis, item_ids, label in metrics:
        inputs_json = json.dumps([metric_code])
        rows.extend(
            [
                (
                    f"{prefix}_growth_qoq",
                    "growth_cagr",
                    "growth",
                    "ratio",
                    metric_code,
                    f"{metric_code}_prior_quarter",
                    item_ids,
                    item_ids,
                    inputs_json,
                    "pct_change",
                    f"metric:{metric_code}|mode:qoq|horizon_years:0.25|basis:{basis}",
                    "require_positive_denominator",
                    f"Quarter-over-quarter percentage change in {label}.",
                    None,
                    "1900-01-01",
                    None,
                ),
                (
                    f"{prefix}_cagr_3y",
                    "growth_cagr",
                    "growth",
                    "ratio",
                    metric_code,
                    f"{metric_code}_prior_3y",
                    item_ids,
                    item_ids,
                    inputs_json,
                    "cagr",
                    f"metric:{metric_code}|mode:cagr|horizon_years:3|basis:{basis}",
                    "require_positive_denominator",
                    f"Three-year compound annual growth rate in {label}.",
                    None,
                    "1900-01-01",
                    None,
                ),
                (
                    f"{prefix}_cagr_5y",
                    "growth_cagr",
                    "growth",
                    "ratio",
                    metric_code,
                    f"{metric_code}_prior_5y",
                    item_ids,
                    item_ids,
                    inputs_json,
                    "cagr",
                    f"metric:{metric_code}|mode:cagr|horizon_years:5|basis:{basis}",
                    "require_positive_denominator",
                    f"Five-year compound annual growth rate in {label}.",
                    None,
                    "1900-01-01",
                    None,
                ),
                (
                    f"{prefix}_growth_stability_3y",
                    "growth_cagr",
                    "growth",
                    "ratio",
                    f"{prefix}_yoy_growth",
                    "trailing_3y_window",
                    item_ids,
                    item_ids,
                    inputs_json,
                    "stability",
                    f"metric:{metric_code}|mode:stability|horizon_years:3|basis:{basis}|min_observations:2",
                    None,
                    f"Three-year stability score for {label} growth.",
                    None,
                    "1900-01-01",
                    None,
                ),
                (
                    f"{prefix}_growth_consistency_3y",
                    "growth_cagr",
                    "growth",
                    "ratio",
                    "positive_growth_observations",
                    "trailing_3y_observations",
                    item_ids,
                    item_ids,
                    inputs_json,
                    "consistency",
                    f"metric:{metric_code}|mode:consistency|horizon_years:3|basis:{basis}|min_observations:2",
                    None,
                    f"Three-year positive-growth consistency share for {label}.",
                    None,
                    "1900-01-01",
                    None,
                ),
            ]
        )
    return tuple(rows)


_S6_0_FORMULA_REGISTRY_ROWS = (
    (
        "pretax_margin",
        "profitability",
        "ratio",
        "ratio",
        "pretax_income",
        "revenue",
        "[1023]",
        "[1001]",
        '["pretax_income", "rev"]',
        "divide",
        "key:pretax_income|key:rev",
        "require_positive_denominator",
        "Pretax income divided by revenue.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "effective_tax_rate",
        "profitability",
        "ratio",
        "ratio",
        "income_tax",
        "pretax_income",
        "[1024]",
        "[1023]",
        '["income_tax", "pretax_income"]',
        "divide",
        "key:income_tax|key:pretax_income",
        "require_positive_denominator",
        "Income tax expense divided by pretax income.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "sga_to_revenue",
        "profitability",
        "ratio",
        "ratio",
        "selling_general_and_administrative_expense",
        "revenue",
        None,
        "[1001]",
        '["selling_general_and_administrative_expense", "rev"]',
        "divide",
        "key:selling_general_and_administrative_expense|key:rev",
        "require_positive_denominator",
        "Selling general and administrative expense divided by revenue.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "return_on_capital",
        "profitability",
        "ratio",
        "ratio",
        "operating_income",
        "long_term_debt_plus_equity",
        "[1014]",
        "[1207, 1221]",
        '["oi", "long_term_debt", "equity"]',
        "divide",
        "key:oi|sum:long_term_debt,equity",
        "require_positive_denominator",
        "Operating income divided by long-term debt plus stockholders' equity.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "inventory_turnover",
        "efficiency",
        "ratio",
        "ratio",
        "cost_of_revenue",
        "inventory",
        "[1003]",
        "[1107]",
        '["cost_of_revenue", "inventory"]',
        "divide",
        "key:cost_of_revenue|key:inventory",
        "require_positive_denominator",
        "Cost of revenue divided by inventory.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "payables_turnover",
        "efficiency",
        "ratio",
        "ratio",
        "cost_of_revenue",
        "accounts_payable",
        "[1003]",
        "[1203]",
        '["cost_of_revenue", "accounts_payable"]',
        "divide",
        "key:cost_of_revenue|key:accounts_payable",
        "require_positive_denominator",
        "Cost of revenue divided by accounts payable.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "working_capital_turnover",
        "efficiency",
        "ratio",
        "ratio",
        "revenue",
        "working_capital",
        "[1001]",
        "[1102, 1202]",
        '["rev", "current_assets", "current_liabilities"]',
        "divide",
        "key:rev|diff:current_assets,current_liabilities",
        "require_positive_denominator",
        "Revenue divided by working capital.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "long_term_debt_to_capital",
        "leverage",
        "ratio",
        "ratio",
        "long_term_debt",
        "long_term_debt_plus_equity",
        "[1207]",
        "[1207, 1221]",
        '["long_term_debt", "equity"]',
        "divide",
        "key:long_term_debt|sum:long_term_debt,equity",
        "require_positive_denominator",
        "Long-term debt divided by long-term debt plus stockholders' equity.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "net_debt_to_capital",
        "leverage",
        "ratio",
        "ratio",
        "net_debt",
        "long_term_debt_plus_equity",
        "[1207, 1104]",
        "[1207, 1221]",
        '["long_term_debt", "cash_and_equivalents", "equity"]',
        "divide",
        "diff:long_term_debt,cash_and_equivalents|sum:long_term_debt,equity",
        "require_positive_denominator",
        "Net debt divided by long-term debt plus stockholders' equity.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "long_term_debt_to_ebitda",
        "leverage",
        "ratio",
        "ratio",
        "long_term_debt",
        "ebitda",
        "[1207]",
        "[1014, 1307]",
        '["long_term_debt", "oi", "depreciation_amortization"]',
        "divide",
        "key:long_term_debt|sum:oi,depreciation_amortization",
        "require_positive_denominator",
        "Long-term debt divided by EBITDA.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "net_debt_to_ebitda",
        "leverage",
        "ratio",
        "ratio",
        "net_debt",
        "ebitda",
        "[1207, 1104]",
        "[1014, 1307]",
        '["long_term_debt", "cash_and_equivalents", "oi", "depreciation_amortization"]',
        "divide",
        "diff:long_term_debt,cash_and_equivalents|sum:oi,depreciation_amortization",
        "require_positive_denominator",
        "Net debt divided by EBITDA.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "operating_cash_flow_to_debt",
        "leverage",
        "ratio",
        "ratio",
        "operating_cash_flow",
        "long_term_debt",
        "[1301]",
        "[1207]",
        '["ocf", "long_term_debt"]',
        "divide",
        "key:ocf|key:long_term_debt",
        "require_positive_denominator",
        "Operating cash flow divided by long-term debt.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "cash_to_assets",
        "liquidity",
        "ratio",
        "ratio",
        "cash_and_equivalents",
        "assets",
        "[1104]",
        "[1101]",
        '["cash_and_equivalents", "assets"]',
        "divide",
        "key:cash_and_equivalents|key:assets",
        "require_positive_denominator",
        "Cash and equivalents divided by total assets.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "current_assets_to_assets",
        "liquidity",
        "ratio",
        "ratio",
        "current_assets",
        "assets",
        "[1102]",
        "[1101]",
        '["current_assets", "assets"]',
        "divide",
        "key:current_assets|key:assets",
        "require_positive_denominator",
        "Current assets divided by total assets.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "current_liabilities_to_assets",
        "liquidity",
        "ratio",
        "ratio",
        "current_liabilities",
        "assets",
        "[1202]",
        "[1101]",
        '["current_liabilities", "assets"]',
        "divide",
        "key:current_liabilities|key:assets",
        "require_positive_denominator",
        "Current liabilities divided by total assets.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "capex_per_share",
        "per_share",
        "per_share",
        "currency_per_share",
        "abs_capital_expenditures",
        "shares_outstanding",
        "[1305]",
        "[1039]",
        '["capex", "shares"]',
        "divide",
        "abs:capex|key:shares",
        None,
        "Absolute capital expenditures divided by shares outstanding.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "dividends_per_share",
        "per_share",
        "per_share",
        "currency_per_share",
        "abs_dividends_paid",
        "shares_outstanding",
        "[1318]",
        "[1039]",
        '["div", "shares"]',
        "divide",
        "abs:div|key:shares",
        None,
        "Absolute dividends paid divided by shares outstanding.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "cash_per_share",
        "per_share",
        "per_share",
        "currency_per_share",
        "cash_and_equivalents",
        "shares_outstanding",
        "[1104]",
        "[1039]",
        '["cash_and_equivalents", "shares"]',
        "divide",
        "key:cash_and_equivalents|key:shares",
        None,
        "Cash and equivalents divided by shares outstanding.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "price_to_cash_flow",
        "valuation",
        "ratio",
        "ratio",
        "market_cap",
        "operating_cash_flow_ttm",
        None,
        "[1301]",
        '["market_cap", "ocf"]',
        "divide",
        "key:market_cap|key:ocf",
        "require_positive_denominator",
        "Market capitalization divided by trailing-twelve-month operating cash flow.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "price_to_free_cash_flow",
        "valuation",
        "ratio",
        "ratio",
        "market_cap",
        "free_cash_flow_ttm",
        None,
        "[1301, 1305]",
        '["market_cap", "ocf", "capex"]',
        "divide",
        "key:market_cap|sum:ocf,capex",
        "require_positive_denominator",
        "Market capitalization divided by trailing-twelve-month free cash flow.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "ev_to_ebit",
        "valuation",
        "ratio",
        "ratio",
        "enterprise_value",
        "operating_income",
        None,
        "[1014]",
        '["enterprise_value", "oi"]',
        "divide",
        "key:enterprise_value|key:oi",
        "require_positive_denominator",
        "Enterprise value divided by operating income.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "ev_to_fcf",
        "valuation",
        "ratio",
        "ratio",
        "enterprise_value",
        "free_cash_flow_ttm",
        None,
        "[1301, 1305]",
        '["enterprise_value", "ocf", "capex"]',
        "divide",
        "key:enterprise_value|sum:ocf,capex",
        "require_positive_denominator",
        "Enterprise value divided by trailing-twelve-month free cash flow.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "ev_to_assets",
        "valuation",
        "ratio",
        "ratio",
        "enterprise_value",
        "assets",
        None,
        "[1101]",
        '["enterprise_value", "assets"]',
        "divide",
        "key:enterprise_value|key:assets",
        "require_positive_denominator",
        "Enterprise value divided by total assets.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "buyback_yield",
        "valuation",
        "ratio",
        "ratio",
        "abs_share_repurchases_ttm",
        "market_cap",
        "[1312]",
        None,
        '["repurch", "market_cap"]',
        "divide",
        "abs:repurch|key:market_cap",
        "require_positive_denominator",
        "Absolute trailing-twelve-month share repurchases divided by market capitalization.",
        None,
        "1900-01-01",
        None,
    ),
    (
        "shareholder_yield",
        "valuation",
        "ratio",
        "ratio",
        "abs_dividends_plus_repurchases_ttm",
        "market_cap",
        "[1312, 1318]",
        None,
        '["div", "repurch", "market_cap"]',
        "divide",
        "abs_sum:div,repurch|key:market_cap",
        "require_positive_denominator",
        "Absolute trailing-twelve-month dividends plus share repurchases divided by market capitalization.",
        None,
        "1900-01-01",
        None,
    ),
)


def _pf3_s6_upsert_formula_registry_rows(
    conn: duckdb.DuckDBPyConnection,
    rows: tuple[tuple[object, ...], ...],
    *,
    temp_table: str,
) -> None:
    conn.execute(
        f"""
        CREATE TEMP TABLE IF NOT EXISTS {temp_table} (
            formula_code VARCHAR PRIMARY KEY,
            family VARCHAR NOT NULL,
            kind VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            numerator_code VARCHAR,
            denominator_code VARCHAR,
            numerator_item_ids_json VARCHAR,
            denominator_item_ids_json VARCHAR,
            inputs_json VARCHAR NOT NULL,
            transform VARCHAR NOT NULL,
            expression VARCHAR,
            is_meaningful_rule VARCHAR,
            definition VARCHAR NOT NULL,
            citation VARCHAR,
            valid_from DATE NOT NULL,
            valid_to DATE
        )
        """
    )
    conn.execute(f"DELETE FROM {temp_table}")
    conn.executemany(
        f"""
        INSERT INTO {temp_table} (
            formula_code,
            family,
            kind,
            unit,
            numerator_code,
            denominator_code,
            numerator_item_ids_json,
            denominator_item_ids_json,
            inputs_json,
            transform,
            expression,
            is_meaningful_rule,
            definition,
            citation,
            valid_from,
            valid_to
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CAST(? AS DATE), CAST(? AS DATE))
        """,
        rows,
    )
    conn.execute(
        f"""
        UPDATE formula_registry AS target
        SET family = seed.family,
            kind = seed.kind,
            unit = seed.unit,
            numerator_code = seed.numerator_code,
            denominator_code = seed.denominator_code,
            numerator_item_ids_json = seed.numerator_item_ids_json,
            denominator_item_ids_json = seed.denominator_item_ids_json,
            inputs_json = seed.inputs_json,
            transform = seed.transform,
            expression = seed.expression,
            is_meaningful_rule = seed.is_meaningful_rule,
            definition = seed.definition,
            citation = seed.citation,
            valid_from = seed.valid_from,
            valid_to = seed.valid_to,
            run_id = NULL,
            source_loaded_at = now()
        FROM {temp_table} AS seed
        WHERE target.formula_code = seed.formula_code
        """
    )
    conn.execute(
        f"""
        INSERT INTO formula_registry (
            formula_code,
            family,
            kind,
            unit,
            numerator_code,
            denominator_code,
            numerator_item_ids_json,
            denominator_item_ids_json,
            inputs_json,
            transform,
            expression,
            is_meaningful_rule,
            definition,
            citation,
            valid_from,
            valid_to
        )
        SELECT seed.formula_code,
               seed.family,
               seed.kind,
               seed.unit,
               seed.numerator_code,
               seed.denominator_code,
               seed.numerator_item_ids_json,
               seed.denominator_item_ids_json,
               seed.inputs_json,
               seed.transform,
               seed.expression,
               seed.is_meaningful_rule,
               seed.definition,
               seed.citation,
               seed.valid_from,
               seed.valid_to
        FROM {temp_table} AS seed
        WHERE NOT EXISTS (
            SELECT 1
            FROM formula_registry AS target
            WHERE target.formula_code = seed.formula_code
        )
        """
    )
    conn.execute(f"DROP TABLE IF EXISTS {temp_table}")


def _pf3_s6_ratio_formula_catalog_expansion(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S6 S6-0: persist the expanded ratio formula catalog."""

    _formula_registry_schema_catalog(conn)
    rows_sha256 = hashlib.sha256(
        repr(_S6_0_FORMULA_REGISTRY_ROWS).encode("utf-8")
    ).hexdigest()
    expected_rows_sha256 = (
        "e6fce500fc27ccd2beaac2250b945b9e6832844f8830d09b40f60ac054ac0477"
    )
    if rows_sha256 != expected_rows_sha256:
        raise RuntimeError("PF3-S6 S6-0 formula_registry row payload hash mismatch")

    conn.execute(
        """
        CREATE TEMP TABLE IF NOT EXISTS _pf3_s6_formula_registry_seed_rows (
            formula_code VARCHAR PRIMARY KEY,
            family VARCHAR NOT NULL,
            kind VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            numerator_code VARCHAR,
            denominator_code VARCHAR,
            numerator_item_ids_json VARCHAR,
            denominator_item_ids_json VARCHAR,
            inputs_json VARCHAR NOT NULL,
            transform VARCHAR NOT NULL,
            expression VARCHAR,
            is_meaningful_rule VARCHAR,
            definition VARCHAR NOT NULL,
            citation VARCHAR,
            valid_from DATE NOT NULL,
            valid_to DATE
        )
        """
    )
    conn.execute("DELETE FROM _pf3_s6_formula_registry_seed_rows")
    conn.executemany(
        """
        INSERT INTO _pf3_s6_formula_registry_seed_rows (
            formula_code,
            family,
            kind,
            unit,
            numerator_code,
            denominator_code,
            numerator_item_ids_json,
            denominator_item_ids_json,
            inputs_json,
            transform,
            expression,
            is_meaningful_rule,
            definition,
            citation,
            valid_from,
            valid_to
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CAST(? AS DATE), CAST(? AS DATE))
        """,
        _S6_0_FORMULA_REGISTRY_ROWS,
    )
    conn.execute(
        """
        UPDATE formula_registry AS target
        SET family = seed.family,
            kind = seed.kind,
            unit = seed.unit,
            numerator_code = seed.numerator_code,
            denominator_code = seed.denominator_code,
            numerator_item_ids_json = seed.numerator_item_ids_json,
            denominator_item_ids_json = seed.denominator_item_ids_json,
            inputs_json = seed.inputs_json,
            transform = seed.transform,
            expression = seed.expression,
            is_meaningful_rule = seed.is_meaningful_rule,
            definition = seed.definition,
            citation = seed.citation,
            valid_from = seed.valid_from,
            valid_to = seed.valid_to,
            run_id = NULL,
            source_loaded_at = now()
        FROM _pf3_s6_formula_registry_seed_rows AS seed
        WHERE target.formula_code = seed.formula_code
        """
    )
    conn.execute(
        """
        INSERT INTO formula_registry (
            formula_code,
            family,
            kind,
            unit,
            numerator_code,
            denominator_code,
            numerator_item_ids_json,
            denominator_item_ids_json,
            inputs_json,
            transform,
            expression,
            is_meaningful_rule,
            definition,
            citation,
            valid_from,
            valid_to
        )
        SELECT seed.formula_code,
               seed.family,
               seed.kind,
               seed.unit,
               seed.numerator_code,
               seed.denominator_code,
               seed.numerator_item_ids_json,
               seed.denominator_item_ids_json,
               seed.inputs_json,
               seed.transform,
               seed.expression,
               seed.is_meaningful_rule,
               seed.definition,
               seed.citation,
               seed.valid_from,
               seed.valid_to
        FROM _pf3_s6_formula_registry_seed_rows AS seed
        WHERE NOT EXISTS (
            SELECT 1
            FROM formula_registry AS target
            WHERE target.formula_code = seed.formula_code
        )
        """
    )
    conn.execute("DROP TABLE IF EXISTS _pf3_s6_formula_registry_seed_rows")
    _formula_registry_catalog_view(conn)
    _catalog_fields_for_tables(conn, ("formula_registry", "v_formula_registry"))
    _refresh_schema_contract_v2_pin(conn)


def _pf3_s6_growth_engine_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S6 S6-1: first-class growth/CAGR metric surface."""

    _formula_registry_schema_catalog(conn)
    _pf3_s6_upsert_formula_registry_rows(
        conn,
        _s6_1_growth_formula_rows(),
        temp_table="_pf3_s6_growth_formula_registry_seed_rows",
    )
    _formula_registry_catalog_view(conn)
    _catalog_fields_for_tables(conn, ("formula_registry", "v_formula_registry"))

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_growth (
            growth_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            formula_code VARCHAR NOT NULL,
            family VARCHAR NOT NULL,
            kind VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            basis VARCHAR NOT NULL,
            growth_method VARCHAR NOT NULL,
            horizon_years DOUBLE NOT NULL,
            elapsed_years DOUBLE NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            base_period_start DATE,
            base_period_end DATE NOT NULL,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            value DOUBLE NOT NULL,
            current_code VARCHAR NOT NULL,
            current_value DOUBLE NOT NULL,
            base_code VARCHAR NOT NULL,
            base_value DOUBLE NOT NULL,
            is_meaningful BOOLEAN NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            vintage_class VARCHAR,
            current_available_at TIMESTAMP NOT NULL,
            base_available_at TIMESTAMP NOT NULL,
            available_at TIMESTAMP NOT NULL,
            as_of_date DATE NOT NULL,
            current_source_accession VARCHAR,
            base_source_accession VARCHAR,
            current_filed_date DATE,
            base_filed_date DATE,
            input_codes_json VARCHAR NOT NULL,
            input_lineage_json VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'fundamental_growth',
            'atx_warehouse',
            'Derived fundamental growth metrics',
            'PIT-safe YoY, QoQ, multi-year CAGR, and trailing growth-stability metrics computed from fundamental_ttm_points and fundamental_statement_points period history.',
            'security_id,formula_code,basis,horizon_years,period_end',
            'fundamental_growth',
            'as_of_date',
            'available_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'fundamental_growth',
            'gold',
            'fundamental_growth',
            'source,security_id,formula_code,basis,horizon_years,period_end',
            'First-class fundamental growth metric surface for YoY, QoQ, CAGR, and growth stability/consistency metrics.',
            '["source","security_id","formula_code","basis","horizon_years","period_end"]',
            'Rows are visible only when every consumed current/base period is visible. available_at is the maximum availability timestamp across the exact period inputs captured in input_lineage_json.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('growth_id', 'security_id', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN (
                    'period_start', 'period_end', 'base_period_start', 'base_period_end',
                    'as_of_date', 'current_filed_date', 'base_filed_date'
                ) THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                WHEN lower(c.column_name) IN ('is_meaningful', 'is_latest_revision') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN lower(c.column_name) IN ('value', 'current_value', 'base_value', 'horizon_years', 'elapsed_years') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'formula_code' THEN 'Formula code registered in formula_registry for this growth metric.'
                WHEN 'growth_method' THEN 'Growth method: yoy, qoq, cagr, stability, or consistency.'
                WHEN 'value' THEN 'Computed growth metric value.'
                WHEN 'available_at' THEN 'Maximum availability timestamp across every consumed input period.'
                WHEN 'input_lineage_json' THEN 'JSON lineage for the exact current/base periods consumed by the metric.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN (
                    'period_start', 'period_end', 'base_period_start', 'base_period_end',
                    'as_of_date', 'current_filed_date', 'base_filed_date'
                ) THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                ELSE NULL
            END,
            NULL,
            now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'fundamental_growth'
        """
    )
    for statement in (
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_fundamental_growth_key "
        "ON fundamental_growth(source, security_id, formula_code, basis, horizon_years, period_end)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_growth_security_period "
        "ON fundamental_growth(security_id, period_end)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_growth_asof "
        "ON fundamental_growth(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_growth_formula "
        "ON fundamental_growth(formula_code, growth_method, basis)",
    ):
        conn.execute(statement)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=148,
        name="pf3_s6_ratio_formula_catalog_expansion",
        up=_pf3_s6_ratio_formula_catalog_expansion,
    ),
    Migration(
        version=149,
        name="pf3_s6_growth_engine_schema_catalog",
        up=_pf3_s6_growth_engine_schema_catalog,
    ),
]
