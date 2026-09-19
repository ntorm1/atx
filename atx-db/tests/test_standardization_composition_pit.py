"""Tier1-S2 T5 follow-up: set-based coalesce_or_* PIT behavior.

New file per review follow-up so as not to touch tests/test_standardization.py
(another implementer is changing its pinned counts concurrently). Reuses the
seeding/rule helpers already defined there.
"""
from __future__ import annotations

import datetime as dt

import pytest

from atx_db._standardization_set_based import refresh_standardized_set_based
from atx_db.standardization import FundamentalStandardizationOptions
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
