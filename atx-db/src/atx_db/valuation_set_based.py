"""Set-based production refresh SQL for the daily valuation-multiples surface."""

from __future__ import annotations

import json
from typing import Any

TTM_INPUTS = {
    "rev": "revenue",
    "ni": "net_income",
    "oi": "operating_income",
    "ocf": "operating_cash_flow",
    "capex": "capital_expenditures",
    "div": "dividends_paid",
    "repurch": "share_repurchases",
}
BALANCE_INPUTS = {"assets": "assets", "equity": "stockholders_equity"}
XBRL_INPUTS = {
    "long_term_debt": "long_term_debt",
    "cash_and_equivalents": "cash_and_equivalents",
    "depreciation_amortization": "depreciation_amortization",
}

INPUT_CODES = {
    "price": "market_cap.close",
    "market_cap": "market_cap.market_cap",
    "enterprise_value": "enterprise_value.enterprise_value",
    "rev": "fundamental_ttm_points.revenue",
    "ni": "fundamental_ttm_points.net_income",
    "oi": "fundamental_ttm_points.operating_income",
    "ocf": "fundamental_ttm_points.operating_cash_flow",
    "capex": "fundamental_ttm_points.capital_expenditures",
    "div": "fundamental_ttm_points.dividends_paid",
    "repurch": "fundamental_ttm_points.share_repurchases",
    "assets": "fundamental_statement_points.assets",
    "equity": "fundamental_statement_points.stockholders_equity",
    "long_term_debt": "fundamental_xbrl_metric.long_term_debt",
    "cash_and_equivalents": "fundamental_xbrl_metric.cash_and_equivalents",
    "depreciation_amortization": "fundamental_xbrl_metric.depreciation_amortization",
}


def _normalized_symbols(symbols: tuple[str, ...] | None) -> tuple[str, ...]:
    return tuple(sorted({str(symbol).strip().upper() for symbol in symbols or () if str(symbol).strip()}))


def _pivot_projection(metric_column: str, mappings: dict[str, str]) -> str:
    expressions: list[str] = []
    for key, metric in mappings.items():
        expressions.extend(
            [
                f"max(CASE WHEN {metric_column} = '{metric}' THEN fact.value END) AS {key}",
                f"max(CASE WHEN {metric_column} = '{metric}' THEN fact.available_at END) AS {key}_av",
                f"max(CASE WHEN {metric_column} = '{metric}' THEN fact.input_id END) AS {key}_id",
                f"max(CASE WHEN {metric_column} = '{metric}' THEN fact.source END) AS {key}_source",
            ]
        )
    return ",\n                ".join(expressions)


def valuation_input_stage_sql(options: Any) -> tuple[str, list[object]]:
    """Return SQL that materializes one PIT-wide input row per market-cap row."""

    symbols = _normalized_symbols(options.symbols)
    if options.symbols and not symbols:
        return "", []

    predicates = [
        "m.is_latest_revision",
        "m.security_id IS NOT NULL",
        "m.trade_date IS NOT NULL",
        "m.market_cap IS NOT NULL",
        "m.market_cap > 0",
        "isfinite(m.market_cap)",
        "m.available_at IS NOT NULL",
    ]
    params: list[object] = []
    if options.market_cap_sources:
        placeholders = ", ".join("?" for _ in options.market_cap_sources)
        predicates.append(f"m.source IN ({placeholders})")
        params.extend(options.market_cap_sources)
    if symbols:
        placeholders = ", ".join("?" for _ in symbols)
        predicates.append(f"upper(m.symbol) IN ({placeholders})")
        params.extend(symbols)
    if options.start_date is not None:
        predicates.append("m.trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("m.trade_date <= ?")
        params.append(options.end_date)
    market_cap_where = "\n              AND ".join(predicates)

    ttm_projection = _pivot_projection("canonical_metric", TTM_INPUTS)
    balance_projection = _pivot_projection("canonical_metric", BALANCE_INPUTS)
    xbrl_projection = _pivot_projection("canonical_metric", XBRL_INPUTS)
    all_keys = (*TTM_INPUTS, *BALANCE_INPUTS, *XBRL_INPUTS)
    input_columns = ",\n                ".join(
        f"{relation}.{key}, {relation}.{key}_av, {relation}.{key}_id, {relation}.{key}_source"
        for relation, keys in (("ttm", TTM_INPUTS), ("bal", BALANCE_INPUTS), ("xbrl", XBRL_INPUTS))
        for key in keys
    )
    availability = ", ".join(f"coalesce({key}_av, TIMESTAMP '1900-01-01')" for key in all_keys)
    sort_ids = ", ".join(f"coalesce({key}_id, '')" for key in all_keys)
    selected_fields = ",\n                ".join(
        f"f.{key}, f.{key}_av, f.{key}_id, f.{key}_source"
        for key in all_keys
    )

    sql = f"""
        CREATE OR REPLACE TEMP TABLE valuation_input_refresh_stage AS
        WITH market_caps AS (
            SELECT
                m.market_cap_id,
                m.source AS market_cap_source,
                m.security_id,
                m.symbol,
                m.trade_date,
                m.close AS price,
                m.market_cap,
                m.available_at AS market_cap_available_at,
                m.price_available_at,
                m.input_lineage_json AS market_cap_input_lineage_json
            FROM market_cap m
            WHERE {market_cap_where}
        ),
        ttm_latest AS (
            SELECT
                t.security_id,
                t.ttm_end_date AS period_end,
                t.canonical_metric,
                arg_max(
                    struct_pack(
                        value := t.ttm_value,
                        available_at := t.available_at,
                        input_id := t.ttm_point_id,
                        source := t.source,
                        period_start := t.ttm_start_date,
                        fiscal_year := t.fiscal_year,
                        fiscal_period := t.fiscal_period
                    ),
                    struct_pack(
                        available_at := t.available_at,
                        revision_sequence := coalesce(t.revision_sequence, 0),
                        source := t.source,
                        input_id := t.ttm_point_id
                    )
                ) AS fact
            FROM fundamental_ttm_points t
            WHERE t.is_latest_revision
              AND t.canonical_metric IN (
                    'revenue', 'net_income', 'operating_income',
                    'operating_cash_flow', 'capital_expenditures',
                    'dividends_paid', 'share_repurchases'
              )
              AND t.ttm_value IS NOT NULL
              AND isfinite(t.ttm_value)
              AND t.available_at IS NOT NULL
            GROUP BY t.security_id, t.ttm_end_date, t.canonical_metric
        ),
        ttm AS (
            SELECT
                security_id,
                period_end,
                min(fact.period_start) AS period_start,
                max(fact.fiscal_year) AS fiscal_year,
                max(fact.fiscal_period) AS fiscal_period,
                {ttm_projection}
            FROM ttm_latest
            GROUP BY security_id, period_end
        ),
        balance_latest AS (
            SELECT
                s.security_id,
                s.period_end,
                s.canonical_metric,
                arg_max(
                    struct_pack(
                        value := s.value,
                        available_at := s.available_at,
                        input_id := s.statement_point_id,
                        source := s.source,
                        fiscal_year := s.fiscal_year,
                        fiscal_period := s.fiscal_period
                    ),
                    struct_pack(
                        available_at := s.available_at,
                        revision_sequence := coalesce(s.revision_sequence, 0),
                        source := s.source,
                        input_id := s.statement_point_id
                    )
                ) AS fact
            FROM fundamental_statement_points s
            WHERE s.is_latest_revision
              AND s.period_type = 'instant'
              AND s.canonical_metric IN ('assets', 'stockholders_equity')
              AND s.value IS NOT NULL
              AND isfinite(s.value)
              AND s.available_at IS NOT NULL
            GROUP BY s.security_id, s.period_end, s.canonical_metric
        ),
        bal AS (
            SELECT
                security_id,
                period_end,
                max(fact.fiscal_year) AS fiscal_year,
                max(fact.fiscal_period) AS fiscal_period,
                {balance_projection}
            FROM balance_latest
            GROUP BY security_id, period_end
        ),
        xbrl_latest AS (
            SELECT
                x.security_id,
                x.period_end,
                x.canonical_metric,
                arg_max(
                    struct_pack(
                        value := x.value,
                        available_at := x.available_at,
                        input_id := x.metric_id,
                        source := x.source,
                        period_start := x.period_start,
                        fiscal_year := x.fiscal_year,
                        fiscal_period := x.fiscal_period
                    ),
                    struct_pack(
                        period_start := coalesce(x.period_start, DATE '1900-01-01'),
                        available_at := x.available_at,
                        revision_sequence := coalesce(x.revision_seq, 0),
                        source := x.source,
                        input_id := x.metric_id
                    )
                ) AS fact
            FROM fundamental_xbrl_metric x
            WHERE x.is_latest_revision
              AND x.canonical_metric IN (
                    'long_term_debt', 'cash_and_equivalents', 'depreciation_amortization'
              )
              AND x.value IS NOT NULL
              AND isfinite(x.value)
              AND x.available_at IS NOT NULL
            GROUP BY x.security_id, x.period_end, x.canonical_metric
        ),
        xbrl AS (
            SELECT
                security_id,
                period_end,
                min(fact.period_start) AS period_start,
                max(fact.fiscal_year) AS fiscal_year,
                max(fact.fiscal_period) AS fiscal_period,
                {xbrl_projection}
            FROM xbrl_latest
            GROUP BY security_id, period_end
        ),
        periods AS (
            SELECT security_id, period_end FROM ttm
            UNION
            SELECT security_id, period_end FROM bal
            UNION
            SELECT security_id, period_end FROM xbrl
        ),
        fundamentals_base AS (
            SELECT
                p.security_id,
                coalesce(ttm.period_start, xbrl.period_start) AS period_start,
                p.period_end,
                coalesce(ttm.fiscal_year, bal.fiscal_year, xbrl.fiscal_year) AS fiscal_year,
                coalesce(ttm.fiscal_period, bal.fiscal_period, xbrl.fiscal_period) AS fiscal_period,
                {input_columns}
            FROM periods p
            LEFT JOIN ttm USING (security_id, period_end)
            LEFT JOIN bal USING (security_id, period_end)
            LEFT JOIN xbrl USING (security_id, period_end)
        ),
        fundamentals AS (
            SELECT
                f.*,
                greatest({availability}) AS fundamental_available_at,
                concat_ws('|', {sort_ids}) AS fundamental_sort_key
            FROM fundamentals_base f
        ),
        matched AS (
            SELECT
                mc.*,
                f.period_start,
                f.period_end,
                f.fiscal_year,
                f.fiscal_period,
                {selected_fields}
            FROM market_caps mc
            ASOF JOIN fundamentals f
              ON f.security_id = mc.security_id
             AND f.period_end <= mc.trade_date
        ),
        enterprise_values AS (
            SELECT
                e.market_cap_source,
                e.security_id,
                e.trade_date,
                arg_max(
                    struct_pack(
                        enterprise_value_id := e.enterprise_value_id,
                        enterprise_value_source := e.source,
                        enterprise_value := e.enterprise_value,
                        enterprise_value_av := e.available_at,
                        enterprise_value_input_lineage_json := e.input_lineage_json
                    ),
                    struct_pack(
                        available_at := e.available_at,
                        source := e.source,
                        input_id := e.enterprise_value_id
                    )
                ) AS ev
            FROM enterprise_value e
            WHERE e.is_latest_revision
              AND e.enterprise_value IS NOT NULL
              AND isfinite(e.enterprise_value)
            GROUP BY e.market_cap_source, e.security_id, e.trade_date
        )
        SELECT
            m.*,
            e.ev.enterprise_value_id,
            e.ev.enterprise_value_source,
            coalesce(
                e.ev.enterprise_value,
                CASE
                    WHEN m.long_term_debt IS NOT NULL AND m.cash_and_equivalents IS NOT NULL
                    THEN m.market_cap + m.long_term_debt - m.cash_and_equivalents
                END
            ) AS enterprise_value,
            coalesce(
                e.ev.enterprise_value_av,
                CASE
                    WHEN m.long_term_debt_av IS NOT NULL AND m.cash_and_equivalents_av IS NOT NULL
                    THEN greatest(
                        m.market_cap_available_at,
                        m.long_term_debt_av,
                        m.cash_and_equivalents_av
                    )
                END
            ) AS enterprise_value_av,
            e.ev.enterprise_value_input_lineage_json
        FROM matched m
        LEFT JOIN enterprise_values e
          ON e.market_cap_source = m.market_cap_source
         AND e.security_id = m.security_id
         AND e.trade_date = m.trade_date
    """
    return sql, params


FORMULAS = (
    ("price_to_earnings", "valuation", "ratio", "ratio", "market_cap", "net_income_ttm", ("market_cap", "ni"), "i.market_cap / i.ni", "i.market_cap", "i.ni", "NULL", "i.ni IS NOT NULL AND i.ni <> 0", "i.ni > 0", ("i.market_cap_available_at", "i.ni_av")),
    ("price_to_book", "valuation", "ratio", "ratio", "market_cap", "stockholders_equity", ("market_cap", "equity"), "i.market_cap / i.equity", "i.market_cap", "i.equity", "NULL", "i.equity IS NOT NULL AND i.equity <> 0", "i.equity > 0", ("i.market_cap_available_at", "i.equity_av")),
    ("price_to_sales", "valuation", "ratio", "ratio", "market_cap", "revenue_ttm", ("market_cap", "rev"), "i.market_cap / i.rev", "i.market_cap", "i.rev", "NULL", "i.rev IS NOT NULL AND i.rev <> 0", "i.rev > 0", ("i.market_cap_available_at", "i.rev_av")),
    ("enterprise_value", "valuation", "difference", "currency", "market_cap_plus_long_term_debt", "cash_and_equivalents", ("market_cap", "long_term_debt", "cash_and_equivalents"), "i.enterprise_value", "i.enterprise_value + coalesce(i.cash_and_equivalents, 0)", "coalesce(i.cash_and_equivalents, 0)", "i.enterprise_value", "i.enterprise_value IS NOT NULL", "i.market_cap > 0", ("i.enterprise_value_av",)),
    ("ev_to_ebitda", "valuation", "ratio", "ratio", "enterprise_value", "ebitda", ("market_cap", "long_term_debt", "cash_and_equivalents", "oi", "depreciation_amortization"), "i.enterprise_value / (i.oi + i.depreciation_amortization)", "i.enterprise_value", "i.oi + i.depreciation_amortization", "i.enterprise_value", "i.enterprise_value IS NOT NULL AND i.oi IS NOT NULL AND i.depreciation_amortization IS NOT NULL AND i.oi + i.depreciation_amortization <> 0", "i.market_cap > 0 AND i.oi + i.depreciation_amortization > 0", ("i.enterprise_value_av", "i.oi_av", "i.depreciation_amortization_av")),
    ("ev_to_sales", "valuation", "ratio", "ratio", "enterprise_value", "revenue_ttm", ("market_cap", "long_term_debt", "cash_and_equivalents", "rev"), "i.enterprise_value / i.rev", "i.enterprise_value", "i.rev", "i.enterprise_value", "i.enterprise_value IS NOT NULL AND i.rev IS NOT NULL AND i.rev <> 0", "i.market_cap > 0 AND i.rev > 0", ("i.enterprise_value_av", "i.rev_av")),
    ("fcf_yield", "valuation", "ratio", "ratio", "free_cash_flow_ttm", "market_cap", ("ocf", "capex", "market_cap"), "(i.ocf + i.capex) / i.market_cap", "i.ocf + i.capex", "i.market_cap", "NULL", "i.ocf IS NOT NULL AND i.capex IS NOT NULL AND i.market_cap <> 0", "i.market_cap > 0", ("i.ocf_av", "i.capex_av", "i.market_cap_available_at")),
    ("earnings_yield", "valuation", "ratio", "ratio", "net_income_ttm", "market_cap", ("ni", "market_cap"), "i.ni / i.market_cap", "i.ni", "i.market_cap", "NULL", "i.ni IS NOT NULL AND i.market_cap <> 0", "i.market_cap > 0", ("i.ni_av", "i.market_cap_available_at")),
    ("dividend_yield", "valuation", "ratio", "ratio", "abs_dividends_paid_ttm", "market_cap", ("div", "market_cap"), "abs(i.div) / i.market_cap", "abs(i.div)", "i.market_cap", "NULL", "i.div IS NOT NULL AND i.market_cap <> 0", "i.market_cap > 0", ("i.div_av", "i.market_cap_available_at")),
    ("price_to_cash_flow", "valuation", "ratio", "ratio", "market_cap", "operating_cash_flow_ttm", ("market_cap", "ocf"), "i.market_cap / i.ocf", "i.market_cap", "i.ocf", "NULL", "i.ocf IS NOT NULL AND i.ocf <> 0", "i.ocf > 0", ("i.market_cap_available_at", "i.ocf_av")),
    ("price_to_free_cash_flow", "valuation", "ratio", "ratio", "market_cap", "free_cash_flow_ttm", ("market_cap", "ocf", "capex"), "i.market_cap / (i.ocf + i.capex)", "i.market_cap", "i.ocf + i.capex", "NULL", "i.ocf IS NOT NULL AND i.capex IS NOT NULL AND i.ocf + i.capex <> 0", "i.ocf + i.capex > 0", ("i.market_cap_available_at", "i.ocf_av", "i.capex_av")),
    ("ev_to_ebit", "valuation", "ratio", "ratio", "enterprise_value", "operating_income", ("enterprise_value", "oi"), "i.enterprise_value / i.oi", "i.enterprise_value", "i.oi", "i.enterprise_value", "i.enterprise_value IS NOT NULL AND i.oi IS NOT NULL AND i.oi <> 0", "i.market_cap > 0 AND i.oi > 0", ("i.enterprise_value_av", "i.oi_av")),
    ("ev_to_fcf", "valuation", "ratio", "ratio", "enterprise_value", "free_cash_flow_ttm", ("enterprise_value", "ocf", "capex"), "i.enterprise_value / (i.ocf + i.capex)", "i.enterprise_value", "i.ocf + i.capex", "i.enterprise_value", "i.enterprise_value IS NOT NULL AND i.ocf IS NOT NULL AND i.capex IS NOT NULL AND i.ocf + i.capex <> 0", "i.market_cap > 0 AND i.ocf + i.capex > 0", ("i.enterprise_value_av", "i.ocf_av", "i.capex_av")),
    ("ev_to_assets", "valuation", "ratio", "ratio", "enterprise_value", "assets", ("enterprise_value", "assets"), "i.enterprise_value / i.assets", "i.enterprise_value", "i.assets", "i.enterprise_value", "i.enterprise_value IS NOT NULL AND i.assets IS NOT NULL AND i.assets <> 0", "i.market_cap > 0 AND i.assets > 0", ("i.enterprise_value_av", "i.assets_av")),
    ("buyback_yield", "valuation", "ratio", "ratio", "abs_share_repurchases_ttm", "market_cap", ("repurch", "market_cap"), "abs(i.repurch) / i.market_cap", "abs(i.repurch)", "i.market_cap", "NULL", "i.repurch IS NOT NULL AND i.market_cap <> 0", "i.market_cap > 0", ("i.repurch_av", "i.market_cap_available_at")),
    ("shareholder_yield", "valuation", "ratio", "ratio", "abs_dividends_plus_repurchases_ttm", "market_cap", ("div", "repurch", "market_cap"), "(abs(i.div) + abs(i.repurch)) / i.market_cap", "abs(i.div) + abs(i.repurch)", "i.market_cap", "NULL", "i.div IS NOT NULL AND i.repurch IS NOT NULL AND i.market_cap <> 0", "i.market_cap > 0", ("i.div_av", "i.repurch_av", "i.market_cap_available_at")),
)


def _lineage_for_key(key: str) -> str:
    if key == "market_cap":
        return """json_object(
            'table', 'market_cap', 'source', i.market_cap_source,
            'market_cap_id', i.market_cap_id, 'security_id', i.security_id,
            'trade_date', i.trade_date, 'available_at', i.market_cap_available_at,
            'price_available_at', i.price_available_at,
            'upstream_lineage', i.market_cap_input_lineage_json
        )"""
    if key == "enterprise_value":
        return """json_object(
            'table', CASE WHEN i.enterprise_value_id IS NULL THEN 'derived' ELSE 'enterprise_value' END,
            'source', i.enterprise_value_source,
            'enterprise_value_id', i.enterprise_value_id,
            'value', i.enterprise_value, 'available_at', i.enterprise_value_av,
            'upstream_lineage', i.enterprise_value_input_lineage_json
        )"""
    if key in TTM_INPUTS:
        return f"""json_object(
            'table', 'fundamental_ttm_points', 'ttm_point_id', i.{key}_id,
            'source', i.{key}_source, 'canonical_metric', '{TTM_INPUTS[key]}',
            'period_end', i.period_end, 'available_at', i.{key}_av
        )"""
    if key in BALANCE_INPUTS:
        return f"""json_object(
            'table', 'fundamental_statement_points', 'statement_point_id', i.{key}_id,
            'source', i.{key}_source, 'canonical_metric', '{BALANCE_INPUTS[key]}',
            'period_end', i.period_end, 'available_at', i.{key}_av
        )"""
    return f"""json_object(
        'table', 'fundamental_xbrl_metric', 'metric_id', i.{key}_id,
        'source', i.{key}_source, 'canonical_metric', '{XBRL_INPUTS[key]}',
        'period_end', i.period_end, 'available_at', i.{key}_av
    )"""


def _formula_branch(definition: tuple[Any, ...]) -> str:
    (
        code,
        category,
        kind,
        unit,
        numerator_code,
        denominator_code,
        input_keys,
        value_expr,
        numerator_expr,
        denominator_expr,
        enterprise_value_expr,
        condition,
        meaningful_expr,
        available_columns,
    ) = definition
    input_codes = json.dumps(
        {key: INPUT_CODES[key] for key in ("price", *input_keys) if key in INPUT_CODES},
        separators=(",", ":"),
        sort_keys=True,
    ).replace("'", "''")
    lineage_pairs = ",\n                    ".join(
        f"'{key}', {_lineage_for_key(key)}" for key in input_keys
    )
    available_at = f"greatest({', '.join(available_columns)})"
    return f"""
        SELECT
            sha256(concat_ws(
                '|', c.source, i.market_cap_source, i.security_id,
                CAST(i.trade_date AS VARCHAR), '{code}'
            )) AS valuation_multiple_id,
            c.source,
            i.market_cap_source,
            i.market_cap_id,
            i.security_id,
            i.symbol,
            i.trade_date,
            '{code}' AS formula_code,
            '{category}' AS category,
            '{kind}' AS kind,
            '{unit}' AS unit,
            i.period_start,
            i.period_end,
            i.fiscal_year,
            i.fiscal_period,
            {value_expr} AS value,
            '{numerator_code}' AS numerator_code,
            {numerator_expr} AS numerator_value,
            '{denominator_code}' AS denominator_code,
            {denominator_expr} AS denominator_value,
            i.price,
            i.market_cap,
            {enterprise_value_expr} AS enterprise_value,
            ({meaningful_expr}) AS is_meaningful,
            true AS is_latest_revision,
            i.trade_date AS as_of_date,
            {available_at} AS available_at,
            i.market_cap_available_at,
            i.price_available_at,
            '{input_codes}' AS input_codes_json,
            CAST(json_object(
                'formula_code', '{code}',
                'inputs', json_object(
                    {lineage_pairs}
                )
            ) AS VARCHAR) AS input_lineage_json,
            c.run_id
        FROM valuation_input_refresh_stage i
        CROSS JOIN config c
        WHERE {condition}
          AND {available_at} IS NOT NULL
          AND isfinite({value_expr})
    """


def valuation_formula_stage_sql(source: str, run_id: str | None) -> tuple[str, list[object]]:
    branches = "\n        UNION ALL\n".join(_formula_branch(definition) for definition in FORMULAS)
    return (
        f"""
        CREATE OR REPLACE TEMP TABLE valuation_multiples_refresh_stage AS
        WITH config AS (
            SELECT CAST(? AS VARCHAR) AS source, CAST(? AS VARCHAR) AS run_id
        )
        {branches}
        """,
        [source, run_id],
    )
