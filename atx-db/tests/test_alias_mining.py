"""Tier1-S2 T4: deterministic alias mining over a tiny synthetic corpus."""
from __future__ import annotations

import csv
import datetime as dt

import pytest

from atx_db.alias_mining import (
    ALIAS_CANDIDATE_COLUMNS,
    AliasMiningOptions,
    cosine_similarity,
    inverse_document_frequency,
    load_concept_profiles,
    mine_alias_candidates,
    tf_idf_vector,
    tokenize_identifier,
    write_alias_candidates,
)

FACTS = [
    # (cik, taxonomy, concept, label, fiscal_year, value)
    ("0000320193", "us-gaap", "RevenueFromContractWithCustomerExcludingAssessedTax", "Revenue from contract with customer", 2020, 2.7e11),
    ("0000789019", "us-gaap", "RevenueFromContractWithCustomerExcludingAssessedTax", "Revenue from contract with customer", 2021, 1.6e11),
    ("0000034088", "us-gaap", "Revenues", "Revenues", 2016, 2.0e11),
    ("0000034088", "us-gaap", "Revenues", "Revenues", 2017, 2.4e11),
    ("0000019617", "us-gaap", "RevenuesNetOfInterestExpense", "Revenues net of interest expense", 2019, 1.1e11),
    ("0000019617", "us-gaap", "InterestAndDividendIncomeOperating", "Interest and dividend income operating", 2019, 5.5e10),
    ("0000004977", "us-gaap", "PremiumsEarnedNet", "Premiums earned net", 2018, 1.8e10),
]

# calculation arcs: (parent_concept, child_concept)
ARCS = [
    ("Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax"),
    ("Revenues", "RevenuesNetOfInterestExpense"),
    ("InterestIncomeExpenseNet", "InterestAndDividendIncomeOperating"),
]


def _seed_corpus(store):
    from atx_db.fundamental_statements import seed_fundamental_statement_map
    from atx_db.item_registry import seed_fundamental_item_registry

    seed_fundamental_item_registry(store)
    seed_fundamental_statement_map(store)
    for index, (cik, taxonomy, concept, label, fiscal_year, value) in enumerate(FACTS):
        store.con.execute(
            """
            INSERT INTO sec_company_facts (
                source, security_id, entity_id, cik, taxonomy, concept, label, description,
                unit, period_start, period_end, filed_date, fiscal_year, fiscal_period,
                form, accession_number, frame, value, available_at, run_id,
                source_url, source_loaded_at
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            """,
            [
                "SEC companyfacts", f"sec-{cik}", None, cik, taxonomy, concept, label, None,
                "USD", dt.date(fiscal_year, 1, 1), dt.date(fiscal_year, 12, 31),
                dt.date(fiscal_year + 1, 2, 1), fiscal_year, "FY", "10-K",
                f"acc-{index}", None, value, dt.datetime(fiscal_year + 1, 2, 1),
                None, "https://example.invalid", dt.datetime(2026, 1, 1),
            ],
        )
    for index, (parent, child) in enumerate(ARCS):
        store.con.execute(
            """
            INSERT INTO xbrl_taxonomy_relationships (
                relationship_id, taxonomy_package_id, taxonomy, release_year, linkbase_type,
                source_file, role_uri, role_name, role_href, arcrole, from_label, to_label,
                parent_href, parent_taxonomy, parent_concept, parent_concept_kind,
                child_href, child_taxonomy, child_concept, child_concept_kind,
                order_value, weight, priority, preferred_label, use, closed, context_element,
                usable, target_role, touches_observed_concept, source_url, source_loaded_at
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            """,
            [
                f"rel-{index}", "us-gaap-2026", "us-gaap", 2026, "calculation",
                "us-gaap-cal.xml", "http://example.invalid/role/IS", "IS", None,
                "http://www.xbrl.org/2003/arcrole/summation-item", parent, child,
                None, "us-gaap", parent, "concept",
                None, "us-gaap", child, "concept",
                1.0, 1.0, 0, None, None, None, None, None, None, False,
                "https://example.invalid", dt.datetime(2026, 1, 1),
            ],
        )


def test_tokenize_identifier_splits_camel_case_and_lowercases():
    assert tokenize_identifier("RevenueFromContractWithCustomerExcludingAssessedTax") == (
        "revenue", "contract", "customer", "excluding", "assessed", "tax",
    )
    assert tokenize_identifier("Revenues net of interest expense") == (
        "revenues", "net", "interest", "expense",
    )


def test_idf_and_cosine_are_pure_and_deterministic():
    docs = [("revenue", "total"), ("revenue", "cost"), ("cash", "equivalents")]
    idf = inverse_document_frequency(docs)
    assert idf["cash"] > idf["revenue"]
    a = tf_idf_vector(("revenue", "total"), idf)
    b = tf_idf_vector(("revenue", "total"), idf)
    assert cosine_similarity(a, b) == pytest.approx(1.0)
    c = tf_idf_vector(("cash", "equivalents"), idf)
    assert cosine_similarity(a, c) == pytest.approx(0.0)


def test_load_concept_profiles_counts_filers_years_and_calculation_parent(tmp_store):
    _seed_corpus(tmp_store)
    profiles = {p.concept: p for p in load_concept_profiles(tmp_store, AliasMiningOptions(minimum_filer_count=1))}

    revenues = profiles["Revenues"]
    assert revenues.filer_count == 1
    assert revenues.fact_count == 2
    assert revenues.first_fiscal_year == 2016
    assert revenues.last_fiscal_year == 2017
    assert revenues.parent_concept is None

    asc606 = profiles["RevenueFromContractWithCustomerExcludingAssessedTax"]
    assert asc606.filer_count == 2
    assert asc606.parent_concept == "Revenues"
    assert asc606.statement_placement == "income_statement"


def test_mine_ranks_revenue_variants_against_item_1001(tmp_store):
    _seed_corpus(tmp_store)
    candidates = mine_alias_candidates(tmp_store, AliasMiningOptions(minimum_filer_count=1, top_n=5))
    for_1001 = [c for c in candidates if c.item_id == 1001]
    assert [c.rank for c in for_1001] == list(range(1, len(for_1001) + 1))
    assert "RevenuesNetOfInterestExpense" in {c.concept for c in for_1001}
    mapped = {c.concept: c.already_mapped for c in for_1001}
    assert mapped["Revenues"] is True
    assert mapped["RevenuesNetOfInterestExpense"] is False


def test_mining_is_byte_identical_across_two_runs(tmp_store, tmp_path):
    _seed_corpus(tmp_store)
    options = AliasMiningOptions(minimum_filer_count=1, top_n=5)
    first = tmp_path / "a.csv"
    second = tmp_path / "b.csv"
    write_alias_candidates(mine_alias_candidates(tmp_store, options), first)
    write_alias_candidates(mine_alias_candidates(tmp_store, options), second)
    assert first.read_bytes() == second.read_bytes()
    with first.open(newline="", encoding="utf-8") as fh:
        assert tuple(next(csv.reader(fh))) == ALIAS_CANDIDATE_COLUMNS


def test_mining_never_touches_the_rule_or_item_seeds(tmp_store):
    from atx_db.item_registry import SEED_PATH
    from atx_db.standardization import RULE_PATH

    before = (SEED_PATH.read_bytes(), RULE_PATH.read_bytes())
    _seed_corpus(tmp_store)
    mine_alias_candidates(tmp_store, AliasMiningOptions(minimum_filer_count=1))
    assert (SEED_PATH.read_bytes(), RULE_PATH.read_bytes()) == before
