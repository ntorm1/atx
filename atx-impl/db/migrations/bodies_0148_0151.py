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
    legacy_yoy_rows = (
        (
            "revenue_growth_yoy",
            "growth",
            "growth",
            "ratio",
            "revenue",
            "revenue_prior_year",
            "[1001]",
            "[1001]",
            '["rev", "rev_prior"]',
            "pct_change",
            "key:rev|key:rev_prior",
            "require_positive_denominator",
            "Year-over-year percentage change in revenue.",
            None,
            "1900-01-01",
            None,
        ),
        (
            "net_income_growth_yoy",
            "growth",
            "growth",
            "ratio",
            "net_income",
            "net_income_prior_year",
            "[1031]",
            "[1031]",
            '["ni", "ni_prior"]',
            "pct_change",
            "key:ni|key:ni_prior",
            "require_positive_denominator",
            "Year-over-year percentage change in net income.",
            None,
            "1900-01-01",
            None,
        ),
        (
            "operating_income_growth_yoy",
            "growth",
            "growth",
            "ratio",
            "operating_income",
            "operating_income_prior_year",
            "[1014]",
            "[1014]",
            '["oi", "oi_prior"]',
            "pct_change",
            "key:oi|key:oi_prior",
            "require_positive_denominator",
            "Year-over-year percentage change in operating income.",
            None,
            "1900-01-01",
            None,
        ),
        (
            "operating_cash_flow_growth_yoy",
            "growth",
            "growth",
            "ratio",
            "operating_cash_flow",
            "operating_cash_flow_prior_year",
            "[1301]",
            "[1301]",
            '["ocf", "ocf_prior"]',
            "pct_change",
            "key:ocf|key:ocf_prior",
            "require_positive_denominator",
            "Year-over-year percentage change in operating cash flow.",
            None,
            "1900-01-01",
            None,
        ),
        (
            "assets_growth_yoy",
            "growth",
            "growth",
            "ratio",
            "assets",
            "assets_prior_year",
            "[1101]",
            "[1101]",
            '["assets", "assets_prior"]',
            "pct_change",
            "key:assets|key:assets_prior",
            "require_positive_denominator",
            "Year-over-year percentage change in total assets.",
            None,
            "1900-01-01",
            None,
        ),
        (
            "equity_growth_yoy",
            "growth",
            "growth",
            "ratio",
            "stockholders_equity",
            "stockholders_equity_prior_year",
            "[1221]",
            "[1221]",
            '["equity", "equity_prior"]',
            "pct_change",
            "key:equity|key:equity_prior",
            "require_positive_denominator",
            "Year-over-year percentage change in stockholders' equity.",
            None,
            "1900-01-01",
            None,
        ),
    )
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
    rows: list[tuple[object, ...]] = list(legacy_yoy_rows)
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


def _pf3_s6_metric_lineage_view(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S6 S6-2: queryable metric -> formula + source lineage surface."""

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_metric_lineage AS
        WITH formula_defs AS (
            SELECT *
            FROM v_formula_registry
        )
        SELECT
            sha256(concat_ws('|', 'fundamental_ratios', r.ratio_id)) AS metric_lineage_id,
            'fundamental_ratios' AS metric_source_table,
            r.ratio_id AS metric_row_id,
            r.source,
            r.security_id,
            r.symbol,
            r.cik,
            r.ratio_code AS metric_code,
            r.ratio_category AS metric_family,
            r.ratio_kind AS metric_kind,
            r.unit,
            r.basis,
            CAST(NULL AS VARCHAR) AS growth_method,
            CAST(NULL AS DOUBLE) AS horizon_years,
            r.period_start,
            r.period_end,
            r.as_of_date AS metric_as_of_date,
            r.available_at AS metric_available_at,
            r.available_at AS available_at,
            r.is_latest_revision,
            r.vintage_class,
            r.source_accession AS current_source_accession,
            CAST(NULL AS VARCHAR) AS base_source_accession,
            r.filed_date AS current_filed_date,
            CAST(NULL AS DATE) AS base_filed_date,
            r.input_codes_json,
            r.input_item_ids_json,
            coalesce(r.input_item_ids_json, r.input_codes_json) AS source_fact_refs_json,
            CAST(to_json(struct_pack(
                source_accession := r.source_accession,
                filed_date := CAST(r.filed_date AS VARCHAR),
                input_codes_json := r.input_codes_json,
                input_item_ids_json := r.input_item_ids_json
            )) AS VARCHAR) AS input_lineage_json,
            r.run_id,
            r.source_loaded_at,
            f.formula_code,
            f.family AS formula_family,
            f.kind AS formula_kind,
            f.unit AS formula_unit,
            f.transform AS formula_transform,
            f.expression AS formula_expression,
            f.inputs_json AS formula_inputs_json,
            f.definition AS formula_definition,
            f.citation AS formula_citation,
            f.valid_from AS formula_valid_from,
            f.valid_to AS formula_valid_to
        FROM fundamental_ratios r
        LEFT JOIN formula_defs f
          ON f.formula_code = r.ratio_code

        UNION ALL

        SELECT
            sha256(concat_ws('|', 'fundamental_growth', g.growth_id)) AS metric_lineage_id,
            'fundamental_growth' AS metric_source_table,
            g.growth_id AS metric_row_id,
            g.source,
            g.security_id,
            g.symbol,
            g.cik,
            g.formula_code AS metric_code,
            g.family AS metric_family,
            g.kind AS metric_kind,
            g.unit,
            g.basis,
            g.growth_method,
            g.horizon_years,
            g.period_start,
            g.period_end,
            g.as_of_date AS metric_as_of_date,
            g.available_at AS metric_available_at,
            g.available_at AS available_at,
            g.is_latest_revision,
            g.vintage_class,
            g.current_source_accession,
            g.base_source_accession,
            g.current_filed_date,
            g.base_filed_date,
            g.input_codes_json,
            CAST(NULL AS VARCHAR) AS input_item_ids_json,
            g.input_lineage_json AS source_fact_refs_json,
            g.input_lineage_json,
            g.run_id,
            g.source_loaded_at,
            f.formula_code,
            f.family AS formula_family,
            f.kind AS formula_kind,
            f.unit AS formula_unit,
            f.transform AS formula_transform,
            f.expression AS formula_expression,
            f.inputs_json AS formula_inputs_json,
            f.definition AS formula_definition,
            f.citation AS formula_citation,
            f.valid_from AS formula_valid_from,
            f.valid_to AS formula_valid_to
        FROM fundamental_growth g
        LEFT JOIN formula_defs f
          ON f.formula_code = g.formula_code

        UNION ALL

        SELECT
            sha256(concat_ws('|', 'valuation_multiples', v.valuation_multiple_id)) AS metric_lineage_id,
            'valuation_multiples' AS metric_source_table,
            v.valuation_multiple_id AS metric_row_id,
            v.source,
            v.security_id,
            v.symbol,
            CAST(NULL AS VARCHAR) AS cik,
            v.formula_code AS metric_code,
            v.category AS metric_family,
            v.kind AS metric_kind,
            v.unit,
            CAST(NULL AS VARCHAR) AS basis,
            CAST(NULL AS VARCHAR) AS growth_method,
            CAST(NULL AS DOUBLE) AS horizon_years,
            v.period_start,
            v.period_end,
            v.as_of_date AS metric_as_of_date,
            v.available_at AS metric_available_at,
            v.available_at AS available_at,
            v.is_latest_revision,
            'most_recently_restated' AS vintage_class,
            CAST(NULL AS VARCHAR) AS current_source_accession,
            CAST(NULL AS VARCHAR) AS base_source_accession,
            CAST(NULL AS DATE) AS current_filed_date,
            CAST(NULL AS DATE) AS base_filed_date,
            v.input_codes_json,
            CAST(NULL AS VARCHAR) AS input_item_ids_json,
            v.input_lineage_json AS source_fact_refs_json,
            v.input_lineage_json,
            v.run_id,
            v.source_loaded_at,
            f.formula_code,
            f.family AS formula_family,
            f.kind AS formula_kind,
            f.unit AS formula_unit,
            f.transform AS formula_transform,
            f.expression AS formula_expression,
            f.inputs_json AS formula_inputs_json,
            f.definition AS formula_definition,
            f.citation AS formula_citation,
            f.valid_from AS formula_valid_from,
            f.valid_to AS formula_valid_to
        FROM valuation_multiples v
        LEFT JOIN formula_defs f
          ON f.formula_code = v.formula_code
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'v_metric_lineage',
            'view',
            'metric_lineage',
            'metric_source_table,metric_row_id',
            'Queryable derived-metric lineage surface joining metric rows to formula_registry definitions and source-input lineage JSON.',
            '["metric_source_table","metric_row_id"]',
            'Use metric_lineage_asof or filter metric_available_at <= decision timestamp and metric_as_of_date <= decision date. Formula definition validity is carried by formula_valid_from/formula_valid_to.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("v_metric_lineage",))
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            ('v_metric_lineage', 'metric_source_table', 'category', 'Source metric table: fundamental_ratios, fundamental_growth, or valuation_multiples.', false, NULL, NULL, now()),
            ('v_metric_lineage', 'metric_row_id', 'identifier', 'Primary row identifier from the source metric table.', false, NULL, NULL, now()),
            ('v_metric_lineage', 'metric_code', 'identifier', 'Metric/formula code emitted by the source metric table.', false, NULL, 'formula_registry.formula_code', now()),
            ('v_metric_lineage', 'metric_available_at', 'timestamp', 'Visibility timestamp of the metric row, derived as max availability of its consumed inputs.', false, 'timestamp', NULL, now()),
            ('v_metric_lineage', 'source_fact_refs_json', 'json', 'Source fact references or input lineage JSON for the metric row.', true, 'json', NULL, now()),
            ('v_metric_lineage', 'input_lineage_json', 'json', 'Metric input lineage JSON normalized across derived metric surfaces.', true, 'json', NULL, now()),
            ('v_metric_lineage', 'formula_expression', 'text', 'Formula expression from formula_registry.', true, NULL, 'formula_registry.expression', now()),
            ('v_metric_lineage', 'formula_valid_from', 'date', 'Formula definition valid-from date.', true, 'date', 'formula_registry.valid_from', now()),
            ('v_metric_lineage', 'formula_valid_to', 'date', 'Formula definition valid-to date.', true, 'date', 'formula_registry.valid_to', now())
        """
    )
    _refresh_schema_contract_v2_pin(conn)


def _pf3_s6_metric_catalog_and_gates(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S6 S6-3: metric catalog, lookup indexes, and gate registry rows."""

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_metric_catalog AS
        SELECT
            formula_code AS metric_code,
            family,
            kind,
            unit,
            CASE
                WHEN kind IN ('growth', 'difference') THEN 'signed'
                WHEN kind = 'score' THEN 'bounded'
                WHEN unit IN ('ratio', 'currency_per_share') THEN 'signed'
                WHEN unit = 'currency' THEN 'signed'
                ELSE 'bounded'
            END AS sign_convention,
            CASE
                WHEN unit IN ('ratio', 'currency_per_share') THEN '1'
                WHEN unit = 'currency' THEN 'USD'
                WHEN unit = 'score' THEN 'score'
                ELSE unit
            END AS scale,
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
            valid_to,
            CAST(NULL AS TIMESTAMP) AS source_loaded_at
        FROM v_formula_registry
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'v_metric_catalog',
            'view',
            'metric_catalog',
            'metric_code',
            'Queryable catalog of every formula-backed metric with family, unit, sign convention, definition, citation, expression, and validity window.',
            '["metric_code"]',
            'Definition validity follows formula_registry valid_from/valid_to. This is catalog metadata, not metric value availability.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("v_metric_catalog",))
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            ('v_metric_catalog', 'metric_code', 'identifier', 'Metric/formula code.', false, NULL, 'formula_registry.formula_code', now()),
            ('v_metric_catalog', 'family', 'category', 'Metric family.', false, NULL, 'formula_registry.family', now()),
            ('v_metric_catalog', 'unit', 'category', 'Metric unit.', false, NULL, 'formula_registry.unit', now()),
            ('v_metric_catalog', 'sign_convention', 'category', 'Semantic sign convention used by downstream gates.', false, NULL, NULL, now()),
            ('v_metric_catalog', 'scale', 'category', 'Semantic scale used by downstream gates.', false, NULL, NULL, now()),
            ('v_metric_catalog', 'definition', 'text', 'Human-readable metric definition.', false, NULL, 'formula_registry.definition', now()),
            ('v_metric_catalog', 'citation', 'text', 'Metric citation where available.', true, NULL, 'formula_registry.citation', now())
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_fundamental_growth_code_available "
        "ON fundamental_growth(formula_code, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_growth_source_available "
        "ON fundamental_growth(source, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_valuation_multiples_formula_available "
        "ON valuation_multiples(formula_code, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_ratios_code_available "
        "ON fundamental_ratios(ratio_code, available_at)",
    ):
        conn.execute(statement)
    conn.execute(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name, dataset_id, table_name, severity, threshold_value,
            comparator, enabled, failure_status, source, updated_at
        )
        VALUES
            (
                'metric_lineage_completeness',
                'metric_lineage',
                'v_metric_lineage',
                'critical',
                0.0,
                'eq',
                true,
                'failed',
                'pf3_s6',
                now()
            ),
            (
                'fundamental_ratio_reconciliation',
                'fundamental_ratios',
                'fundamental_ratios',
                'critical',
                0.0,
                'eq',
                true,
                'failed',
                'pf3_s6',
                now()
            )
        """
    )
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
    Migration(
        version=150,
        name="pf3_s6_metric_lineage_view",
        up=_pf3_s6_metric_lineage_view,
    ),
    Migration(
        version=151,
        name="pf3_s6_metric_catalog_and_gates",
        up=_pf3_s6_metric_catalog_and_gates,
    ),
]
