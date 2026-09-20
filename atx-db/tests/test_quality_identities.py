"""Tier1-S4 T6: accounting identities, cross-source shares, and coverage gates.

``fundamental_standardized`` has no ``revision_sequence`` column (verified against
``migrations/bodies_0001_0137.py::_fundamental_standardized_schema_catalog`` -- that column
lives on ``fundamental_points``/TTM tables, not here), so ``_insert_standardized`` below omits
it; ``checks_identities.identity_violation_sql`` resolves "the current fact" via a plain
``is_latest_revision`` filter, mirroring ``item_coverage.load_item_coverage_inputs``.
"""

from __future__ import annotations

import pytest


def _spec(name):
    from atx_db.quality.checks_identities import identity_check_specs

    return next(spec for spec in identity_check_specs() if spec.check_name == name)


def _observed(store, name):
    return float(store.con.execute(_spec(name).sql).fetchone()[0])


def _insert_standardized(store, rows):
    # input_codes_json/input_item_ids_json/rule_id/combination_rule are NOT NULL on
    # fundamental_standardized (verified against
    # migrations/bodies_0001_0137.py::_fundamental_standardized_schema_catalog) but are not
    # exercised by the identity checks, so fixed placeholder values are used.
    values = ",".join(
        "('{sid}-{code}-{pe}','test','sec','{sid}','{cik}',{item},'{code}','{basis}',"
        "DATE '{pe}',{value},TIMESTAMP '{pe} 22:00:00',DATE '{pe}',true,"
        "'acc-{sid}-{pe}','[]','[]','test-rule','direct')".format(**row)
        for row in rows
    )
    store.con.execute(
        "INSERT INTO fundamental_standardized (standardized_id, source, upstream_source, "
        "security_id, cik, item_id, canonical_code, basis, period_end, value, available_at, "
        "as_of_date, is_latest_revision, source_accession, input_codes_json, "
        "input_item_ids_json, rule_id, combination_rule) VALUES " + values
    )


def _balance_rows(sid, assets, liabilities, equity, minority=None):
    rows = [
        dict(sid=sid, cik="0000000001", item=1101, code="total_assets", basis="instant", pe="2024-03-31", value=assets),
        dict(
            sid=sid,
            cik="0000000001",
            item=1201,
            code="total_liabilities",
            basis="instant",
            pe="2024-03-31",
            value=liabilities,
        ),
        dict(
            sid=sid,
            cik="0000000001",
            item=1221,
            code="stockholders_equity",
            basis="instant",
            pe="2024-03-31",
            value=equity,
        ),
    ]
    if minority is not None:
        rows.append(
            dict(
                sid=sid,
                cik="0000000001",
                item=1213,
                code="minority_interest_bs",
                basis="instant",
                pe="2024-03-31",
                value=minority,
            )
        )
    return rows


def test_a_balanced_balance_sheet_is_not_a_violation(tmp_store):
    _insert_standardized(tmp_store, _balance_rows("SEC-1", 1_000_000_000, 600_000_000, 400_000_000))
    assert _observed(tmp_store, "balance_sheet_identity_violations") == 0.0


def test_minority_interest_is_part_of_the_identity(tmp_store):
    _insert_standardized(
        tmp_store,
        _balance_rows("SEC-1", 1_000_000_000, 600_000_000, 350_000_000, minority=50_000_000),
    )
    assert _observed(tmp_store, "balance_sheet_identity_violations") == 0.0


def test_a_break_beyond_tolerance_is_a_violation(tmp_store):
    _insert_standardized(tmp_store, _balance_rows("SEC-1", 1_000_000_000, 600_000_000, 300_000_000))
    assert _observed(tmp_store, "balance_sheet_identity_violations") == 1.0


def test_a_break_inside_the_relative_tolerance_is_not_a_violation(tmp_store):
    # 4e6 break on 1e9 assets; greatest(0.5% * 1e9, 1e6) = 5e6, so this passes.
    _insert_standardized(tmp_store, _balance_rows("SEC-1", 1_000_000_000, 600_000_000, 396_000_000))
    assert _observed(tmp_store, "balance_sheet_identity_violations") == 0.0


def test_a_small_filing_still_gets_the_one_million_dollar_floor(tmp_store):
    # 0.5% of 1e7 is 5e4, so the 1e6 absolute floor governs; a 9e5 break must pass.
    _insert_standardized(tmp_store, _balance_rows("SEC-1", 10_000_000, 6_000_000, 3_100_000))
    assert _observed(tmp_store, "balance_sheet_identity_violations") == 0.0


def test_a_filing_missing_an_input_is_never_a_violation(tmp_store):
    rows = _balance_rows("SEC-1", 1_000_000_000, 600_000_000, 400_000_000)
    _insert_standardized(tmp_store, rows[:2])
    assert _observed(tmp_store, "balance_sheet_identity_violations") == 0.0


def test_the_gross_profit_identity_catches_a_break(tmp_store):
    rows = [
        dict(
            sid="SEC-2",
            cik="0000000002",
            item=1001,
            code="revenue",
            basis="quarterly",
            pe="2024-03-31",
            value=500_000_000,
        ),
        dict(
            sid="SEC-2",
            cik="0000000002",
            item=1003,
            code="cost_of_revenue_cogs",
            basis="quarterly",
            pe="2024-03-31",
            value=300_000_000,
        ),
        dict(
            sid="SEC-2",
            cik="0000000002",
            item=1004,
            code="gross_profit__1004",
            basis="quarterly",
            pe="2024-03-31",
            value=150_000_000,
        ),
    ]
    _insert_standardized(tmp_store, rows)
    assert _observed(tmp_store, "gross_profit_identity_violations") == 1.0


def test_the_cash_flow_identity_ties_out(tmp_store):
    rows = [
        dict(
            sid="SEC-3",
            cik="0000000003",
            item=1301,
            code="cash_flow_from_operations",
            basis="quarterly",
            pe="2024-03-31",
            value=100_000_000,
        ),
        dict(
            sid="SEC-3",
            cik="0000000003",
            item=1303,
            code="cash_flow_from_investing",
            basis="quarterly",
            pe="2024-03-31",
            value=-40_000_000,
        ),
        dict(
            sid="SEC-3",
            cik="0000000003",
            item=1304,
            code="cash_flow_from_financing",
            basis="quarterly",
            pe="2024-03-31",
            value=-20_000_000,
        ),
        dict(
            sid="SEC-3",
            cik="0000000003",
            item=1323,
            code="fx_effect_on_cash",
            basis="quarterly",
            pe="2024-03-31",
            value=1_000_000,
        ),
        dict(
            sid="SEC-3",
            cik="0000000003",
            item=1324,
            code="net_change_in_cash",
            basis="quarterly",
            pe="2024-03-31",
            value=41_000_000,
        ),
    ]
    _insert_standardized(tmp_store, rows)
    assert _observed(tmp_store, "cash_flow_identity_violations") == 0.0


def _insert_market_daily(store, rows):
    values = ",".join(
        "('{sid}-{td}','atx-db daily market panel v1','{sid}','{sym}',DATE '{td}',{ratio},"
        "TIMESTAMP '{td} 22:00:00','h',DATE '{td}',true)".format(**row)
        for row in rows
    )
    store.con.execute(
        "INSERT INTO market_daily_metrics (market_daily_id, source, security_id, symbol, "
        "trade_date, shares_reconciliation_ratio, available_at, inputs_hash, as_of_date, "
        "is_latest_revision) VALUES " + values
    )


def test_shares_agreement_inside_five_percent_passes(tmp_store):
    _insert_market_daily(
        tmp_store,
        [
            dict(sid="SEC-1", sym="A", td="2024-01-02", ratio=1.02),
            dict(sid="SEC-1", sym="A", td="2024-01-03", ratio=0.99),
        ],
    )
    assert _observed(tmp_store, "shares_cross_source_disagreement") == 0.0


def test_a_security_outside_five_percent_fails_the_share_check(tmp_store):
    _insert_market_daily(
        tmp_store,
        [
            dict(sid="SEC-1", sym="A", td="2024-01-02", ratio=1.02),
            dict(sid="SEC-2", sym="B", td="2024-01-02", ratio=1.40),
            dict(sid="SEC-2", sym="B", td="2024-01-03", ratio=1.38),
        ],
    )
    assert _observed(tmp_store, "shares_cross_source_disagreement") == pytest.approx(0.5)


def test_securities_with_only_one_share_source_are_out_of_scope(tmp_store):
    _insert_market_daily(tmp_store, [dict(sid="SEC-1", sym="A", td="2024-01-02", ratio="NULL")])
    assert _observed(tmp_store, "shares_cross_source_disagreement") == 0.0


def test_a_derived_family_with_no_values_is_reported(tmp_store):
    tmp_store.con.execute(
        "INSERT INTO derived_metric_definitions (metric_code, family, expression, metric_window, "
        "inputs_json, requires_market, description, version, topological_rank) VALUES "
        "('gross_margin','profitability','item:gross_profit__1004 / item:revenue','q','[]',"
        "false,'d','1',1),"
        "('payout_ratio','payout','item:dividends_paid / item:net_income_total','q','[]',"
        "false,'d','1',2)"
    )
    tmp_store.con.execute(
        "INSERT INTO derived_metric_values (derived_value_id, source, security_id, metric_code, "
        "metric_window, period_end, value, available_at, inputs_hash, as_of_date) VALUES "
        "('v1','atx-db declarative derived metrics v1','SEC-1','gross_margin','q',"
        "DATE '2024-03-31',0.4,TIMESTAMP '2024-05-01 22:00:00','h',DATE '2024-03-31')"
    )
    assert _observed(tmp_store, "derived_metric_families_without_values") == 1.0


def test_item_coverage_shortfall_is_measured(tmp_store):
    from atx_db.item_coverage import ITEM_COVERAGE_TARGET_ITEMS

    tmp_store.con.execute(
        "INSERT INTO fundamental_item_coverage (coverage_id, source, universe_id, item_id, "
        "canonical_code, basis, fiscal_year, n_securities, n_with_value, coverage_pct) VALUES "
        "('c1','t','us_listed_v1',1101,'total_assets','instant',2020,100,99,99.0)"
    )
    assert _observed(tmp_store, "fundamental_item_coverage_below_target") == float(ITEM_COVERAGE_TARGET_ITEMS - 1)


def test_every_identity_spec_declares_its_required_tables():
    from atx_db.quality.checks_identities import identity_check_specs

    specs = identity_check_specs()
    assert len(specs) == 6
    for spec in specs:
        assert spec.required_tables
        assert spec.warn_if_missing is True
        assert spec.severity in {"warning", "error", "critical"}


def test_the_identity_specs_are_part_of_the_production_sweep():
    from atx_db.quality._checks import _check_specs

    names = {
        spec.check_name
        for spec in _check_specs(daily_macro_stale_days=10, monthly_macro_stale_days=70, valuation_stale_gap_days=30)
    }
    assert "balance_sheet_identity_violations" in names
    assert "shares_cross_source_disagreement" in names
    assert "derived_metric_families_without_values" in names
    assert "fundamental_item_coverage_below_target" in names
