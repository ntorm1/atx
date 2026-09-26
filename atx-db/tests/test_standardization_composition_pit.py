"""Tier1-S2 T5 follow-up: set-based coalesce_or_* PIT behavior.

New file per review follow-up so as not to touch tests/test_standardization.py
(another implementer is changing its pinned counts concurrently). Reuses the
seeding/rule helpers already defined there.
"""
from __future__ import annotations

import dataclasses
import datetime as dt

import pytest

from atx_db._standardization_set_based import refresh_standardized_set_based
from atx_db.standardization import FundamentalStandardizationOptions, rule_dependency_levels
from tests.test_standardization import _rule, _seed_statement_points


def test_direct_alias_present_suppresses_composition(tmp_store):
    """A visible direct GrossProfit tag wins; the SQL composition is not emitted."""
    t1 = dt.datetime(2026, 2, 1)
    _seed_statement_points(
        tmp_store,
        [
            ("us-gaap", "Revenues", "revenue", 1001, 1000.0),
            ("us-gaap", "CostOfRevenue", "cost_of_revenue", 1003, 700.0),
            ("us-gaap", "GrossProfit", "gross_profit", 1004, 450.0),
        ],
        id_prefix="direct",
        available_at=t1,
    )
    gross_profit_rule = _rule(
        1004,
        combination_rule="coalesce_or_difference",
        source_item_ids=(1001, 1003),
        canonical_code="gross_profit__1004",
    )
    refresh_standardized_set_based(
        tmp_store,
        FundamentalStandardizationOptions(symbols=("TST",)),
        (gross_profit_rule,),
    )
    rows = tmp_store.con.execute(
        """
        SELECT value, available_at, combination_rule
        FROM fundamental_standardized
        WHERE item_id = 1004
        ORDER BY available_at
        """
    ).fetchall()

    assert len(rows) == 1
    assert rows[0][0] == pytest.approx(450.0)
    assert rows[0][1] == t1
    assert rows[0][2] == "coalesce_or_difference"


def test_direct_tag_arriving_later_supersedes_the_composed_revision(tmp_store):
    """Composition fires at t1 (no direct tag yet); the direct tag arrives at
    t2 > t1 and must supersede it as the newest, is_latest revision -- while
    the t1 composed revision remains in history with its own available_at."""
    t1 = dt.datetime(2026, 2, 1)
    t2 = dt.datetime(2026, 5, 1)
    _seed_statement_points(
        tmp_store,
        [
            ("us-gaap", "Revenues", "revenue", 1001, 1000.0),
            ("us-gaap", "CostOfRevenue", "cost_of_revenue", 1003, 700.0),
        ],
        id_prefix="composed",
        available_at=t1,
    )
    _seed_statement_points(
        tmp_store,
        [
            ("us-gaap", "GrossProfit", "gross_profit", 1004, 460.0),
        ],
        id_prefix="late-direct",
        available_at=t2,
    )
    gross_profit_rule = _rule(
        1004,
        combination_rule="coalesce_or_difference",
        source_item_ids=(1001, 1003),
        canonical_code="gross_profit__1004",
    )
    refresh_standardized_set_based(
        tmp_store,
        FundamentalStandardizationOptions(symbols=("TST",)),
        (gross_profit_rule,),
    )
    revisions = tmp_store.con.execute(
        """
        SELECT value, available_at, combination_rule, revision_sequence, is_latest_revision
        FROM fundamental_standardized
        WHERE item_id = 1004
        ORDER BY available_at
        """
    ).fetchall()

    assert len(revisions) == 2

    composed = revisions[0]
    assert composed[0] == pytest.approx(300.0)
    assert composed[1] == t1
    assert composed[2] == "coalesce_or_difference"
    assert composed[4] is False

    direct = revisions[1]
    assert direct[0] == pytest.approx(460.0)
    assert direct[1] == t2
    assert direct[2] == "coalesce_or_difference"
    assert direct[4] is True
    assert direct[3] > composed[3]


def test_composition_reads_another_rules_output_in_dependency_order(tmp_store):
    """S1: common equity = derived stockholders' equity (a rule output) - preferred - temp equity.

    The dependent rule is listed first; dependency levels still evaluate stockholders'
    equity (1222 - 1213) before common equity reads it. The n-ary difference zero-fills the
    absent third input under ``zero_fill_subtrahends``, and ``input_codes_json`` states the
    derivation basis. A cyclic output dependency is rejected at load.
    """
    _seed_statement_points(
        tmp_store,
        [
            ("us-gaap", "StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest",
             "equity_incl_minority", 1222, 1000.0),
            ("us-gaap", "MinorityInterest", "minority_int_bs", 1213, 100.0),
            ("us-gaap", "PreferredStockValue", "pref_stock", 1214, 50.0),
        ],
        id_prefix="dep",
    )
    stockholders_equity = _rule(
        1221, combination_rule="coalesce_or_difference", source_item_ids=(1222, 1213),
        canonical_code="stockholders_equity",
    )
    common_equity = dataclasses.replace(
        _rule(1220, combination_rule="coalesce_or_difference", source_item_ids=(1221, 1214, 1224),
              canonical_code="common_equity"),
        missing_policy="zero_fill_subtrahends",
        source_input_kinds=("output", "item", "item"),
    )
    assert rule_dependency_levels((common_equity, stockholders_equity)) == {
        common_equity.rule_id: 1, stockholders_equity.rule_id: 0,
    }
    cyclic = dataclasses.replace(
        stockholders_equity, source_item_ids=(1220, 1213), source_input_kinds=("output", "item")
    )
    with pytest.raises(ValueError, match="cyclic standardization rule dependency"):
        rule_dependency_levels((common_equity, cyclic))

    refresh_standardized_set_based(
        tmp_store,
        FundamentalStandardizationOptions(symbols=("TST",)),
        (common_equity, stockholders_equity),
    )
    rows = tmp_store.con.execute(
        """
        SELECT item_id, value, input_codes_json, input_item_ids_json
        FROM fundamental_standardized
        WHERE item_id IN (1220, 1221)
        ORDER BY item_id
        """
    ).fetchall()

    assert rows == [
        (1220, pytest.approx(850.0), '["atx-rule-output:stockholders_equity","us-gaap:PreferredStockValue"]',
         "[1221,1214]"),
        (1221, pytest.approx(900.0),
         '["us-gaap:StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest","us-gaap:MinorityInterest"]',
         "[1222,1213]"),
    ]
