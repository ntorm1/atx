"""Tier1-S2 T2: the companyfacts ingest allowlist is derived, not hand-maintained."""
from __future__ import annotations

from atx_db.fundamental_statements import (
    CONCEPT_MAP_SUPPORTED_TAXONOMIES,
    DEI_COVER_PAGE_CONCEPTS,
    concept_map_projection_rows,
    default_companyfacts_concepts,
    rule_alias_concepts,
)
from atx_db.standardization import default_standardization_rules


def test_every_active_rule_alias_is_ingested():
    allowlist = set(default_companyfacts_concepts())
    missing = sorted(
        f"{scheme}:{code}"
        for scheme, code in rule_alias_concepts()
        if scheme in CONCEPT_MAP_SUPPORTED_TAXONOMIES and code not in allowlist
    )
    assert missing == []


def test_every_statement_map_projection_concept_is_ingested():
    allowlist = set(default_companyfacts_concepts())
    missing = sorted({row[1] for row in concept_map_projection_rows()} - allowlist)
    assert missing == []


def test_dei_cover_page_concepts_are_ingested():
    allowlist = set(default_companyfacts_concepts())
    assert set(DEI_COVER_PAGE_CONCEPTS).issubset(allowlist)


def test_allowlist_is_sorted_and_distinct():
    concepts = default_companyfacts_concepts()
    assert list(concepts) == sorted(concepts)
    assert len(concepts) == len(set(concepts))


def test_allowlist_is_deterministic_across_calls():
    assert default_companyfacts_concepts() == default_companyfacts_concepts()


def test_rule_alias_concepts_are_sorted_distinct_pairs():
    pairs = rule_alias_concepts()
    assert list(pairs) == sorted(pairs)
    assert len(pairs) == len(set(pairs))
    assert all(isinstance(p, tuple) and len(p) == 2 for p in pairs)


def test_rule_alias_concepts_skips_inactive_rules():
    active_pairs = {
        (alias.alias_scheme, alias.alias_code)
        for rule in default_standardization_rules()
        if rule.is_active
        for alias in rule.source_aliases
    }
    assert set(rule_alias_concepts()) == active_pairs


def test_allowlist_is_strictly_wider_than_the_projection():
    projection = {row[1] for row in concept_map_projection_rows()}
    assert set(default_companyfacts_concepts()) >= projection
