"""Tier1-S3 T4: the shipped derived catalog is complete, well-formed and spec-aligned."""

from __future__ import annotations

import pytest

from atx_db.derived_dsl import parse_expression
from atx_db.derived_registry import (
    DERIVED_SOURCE_NAME,
    METRIC_WINDOWS,
    RECLAIMED_ITEM_CODES,
    default_derived_definitions,
    derived_statement_item_codes,
    known_item_codes,
    topological_order,
    validate_definitions,
)

EXPECTED_METRIC_COUNT = 173
EXPECTED_DAILY_COUNT = 30

# Every family named in the spec section "Derived metric catalog", plus the
# rollup family that carries the TTM sums and avg2 balances the spec requires.
EXPECTED_FAMILIES = {
    "rollup",
    "per_share",
    "profitability",
    "growth",
    "leverage",
    "quality",
    "investment",
    "payout",
    "market",
}

# Named, one-for-one, from the spec's family lists.
SPEC_REQUIRED_METRICS = {
    "eps_ttm",
    "sales_per_share",
    "book_per_share",
    "cfo_per_share",
    "fcf_per_share",
    "dividends_per_share_ttm",
    "market_cap",
    "enterprise_value",
    "ev_ebitda",
    "ev_sales",
    "pe_ttm",
    "pb",
    "ps_ttm",
    "pcf_ttm",
    "fcf_yield",
    "dividend_yield",
    "earnings_yield",
    "shareholder_yield",
    "gross_margin",
    "operating_margin",
    "net_margin",
    "ebitda_margin",
    "roa",
    "roe",
    "roic",
    "roic_ex_goodwill",
    "gross_profitability",
    "cash_profitability",
    "asset_turnover",
    "revenue_growth_yoy",
    "gross_profit_growth_yoy",
    "operating_income_growth_yoy",
    "net_income_growth_yoy",
    "eps_diluted_growth_yoy",
    "cfo_growth_yoy",
    "fcf_growth_yoy",
    "asset_growth",
    "shares_growth_yoy",
    "book_value_growth_yoy",
    "capex_growth_yoy",
    "revenue_cagr_1y",
    "revenue_cagr_3y",
    "total_debt_q",
    "net_debt",
    "debt_to_equity",
    "debt_to_assets",
    "net_debt_ebitda",
    "interest_coverage",
    "current_ratio",
    "quick_ratio",
    "cash_ratio",
    "total_accruals",
    "percent_accruals",
    "noa",
    "delta_noa",
    "piotroski_f",
    "altman_z",
    "beneish_m",
    "ohlson_o",
    "earnings_variability",
    "capex_to_depreciation",
    "capex_to_sales",
    "external_financing",
    "net_equity_issuance",
    "net_debt_issuance",
    "payout_ratio",
    "buyback_yield",
    "total_payout_yield",
}

MARKET_DAILY_COLUMNS = {
    "market_cap",
    "enterprise_value",
    "pe_ttm",
    "pb",
    "ps_ttm",
    "pcf_ttm",
    "ev_ebitda",
    "ev_sales",
    "fcf_yield",
    "dividend_yield",
    "earnings_yield",
    "shareholder_yield",
    "net_payout_yield",
    "total_payout_yield",
    "buyback_yield",
    "book_to_market",
    "rd_to_market_equity",
    "gross_profit_to_ev",
    "cfo_to_ev",
    "ebit_to_ev",
    "sales_to_ev",
    "altman_z",
    "total_return_1m",
    "total_return_3m",
    "total_return_6m",
    "total_return_12m",
    "momentum_12_1",
    "realized_vol_60d",
    "realized_vol_252d",
    "dollar_volume_20d",
}


# Mirrors atx_db.migrations.bodies_0302._DAILY_METRIC_COLUMNS verbatim (the
# wide-column list `market_daily_metrics` is built from). That name is
# module-private -- not part of any migrations `__all__` -- so it is not
# imported directly here; this copy is the drift guard and must be kept in
# sync by hand whenever either side changes.
PINNED_MARKET_DAILY_COLUMNS_0302 = (
    "market_cap",
    "enterprise_value",
    "pe_ttm",
    "pb",
    "ps_ttm",
    "pcf_ttm",
    "ev_ebitda",
    "ev_sales",
    "fcf_yield",
    "dividend_yield",
    "earnings_yield",
    "shareholder_yield",
    "net_payout_yield",
    "total_payout_yield",
    "buyback_yield",
    "book_to_market",
    "rd_to_market_equity",
    "gross_profit_to_ev",
    "cfo_to_ev",
    "ebit_to_ev",
    "sales_to_ev",
    "altman_z",
    "total_return_1m",
    "total_return_3m",
    "total_return_6m",
    "total_return_12m",
    "momentum_12_1",
    "realized_vol_60d",
    "realized_vol_252d",
    "dollar_volume_20d",
)


@pytest.fixture(scope="module")
def definitions():
    return default_derived_definitions()


def test_catalog_has_the_expected_size(definitions):
    assert len(definitions) == EXPECTED_METRIC_COUNT
    daily = [d for d in definitions if d.window == "daily"]
    assert len(daily) == EXPECTED_DAILY_COUNT


def test_catalog_validates(definitions):
    validate_definitions(definitions, item_codes=known_item_codes())


def test_every_expression_parses(definitions):
    for definition in definitions:
        parse_expression(definition.expression)


def test_families_are_exactly_the_spec_families(definitions):
    assert {definition.family for definition in definitions} == EXPECTED_FAMILIES


def test_every_spec_named_metric_is_present(definitions):
    codes = {definition.metric_code for definition in definitions}
    assert sorted(SPEC_REQUIRED_METRICS - codes) == []


def test_daily_metric_codes_match_the_published_columns(definitions):
    daily = {d.metric_code for d in definitions if d.window == "daily"}
    assert daily == MARKET_DAILY_COLUMNS


def test_daily_metric_codes_match_the_pinned_migration_columns(definitions):
    daily = {d.metric_code for d in definitions if d.window == "daily"}
    assert daily == set(PINNED_MARKET_DAILY_COLUMNS_0302)
    assert len(PINNED_MARKET_DAILY_COLUMNS_0302) == len(set(PINNED_MARKET_DAILY_COLUMNS_0302))


def test_every_window_is_known(definitions):
    assert {definition.window for definition in definitions} <= METRIC_WINDOWS


def test_every_reclaimed_code_that_is_used_is_a_dead_derived_row(definitions):
    codes = {definition.metric_code for definition in definitions}
    used = codes & known_item_codes()
    assert used <= RECLAIMED_ITEM_CODES
    assert used <= derived_statement_item_codes()


def test_catalog_is_acyclic_and_orders_stably(definitions):
    ordered = topological_order(definitions)
    assert len(ordered) == len(definitions)
    positions = {d.metric_code: i for i, d in enumerate(ordered)}
    for definition in definitions:
        for dependency in definition.metric_inputs:
            assert positions[dependency] < positions[definition.metric_code]


def test_descriptions_and_versions_are_populated(definitions):
    for definition in definitions:
        assert definition.description.endswith(".")
        assert definition.version == "1"


def test_no_metric_is_defined_twice(definitions):
    codes = [definition.metric_code for definition in definitions]
    assert len(codes) == len(set(codes))


def test_derived_source_name_is_stable():
    assert DERIVED_SOURCE_NAME == "atx-db declarative derived metrics v1"
