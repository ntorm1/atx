"""PF-S3 S3-0 concept coverage gates.

The companyfacts re-fetch that materializes the wider concept set is an
operator-run step (for example symbol_source='loaded_facts' with a local
companyfacts_zip). These tests stay fully offline and only verify the committed
projection and loader defaults.
"""
from __future__ import annotations

import csv
from pathlib import Path

from db.fundamental_statements import (
    CONCEPT_MAP_SEED_COLUMNS,
    FUNDAMENTAL_STATEMENT_MAP_ROWS,
    concept_map_projection_rows,
)
from db.fundamentals import CANONICAL_CONCEPTS, DEFAULT_CONCEPTS, SUPPORTED_FACT_TAXONOMIES
from db.item_registry import read_fundamental_item_seed


CONCEPT_MAP_PATH = Path(__file__).resolve().parents[1] / "seeds" / "concept_map.csv"


def _read_concept_map_seed() -> tuple[tuple[str, str, str, int, str, str], ...]:
    with CONCEPT_MAP_PATH.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        assert tuple(reader.fieldnames or ()) == CONCEPT_MAP_SEED_COLUMNS
        return tuple(
            (
                row["taxonomy"],
                row["concept"],
                row["canonical_metric"],
                int(row["item_id"]),
                row["statement_type"],
                row["industry_template"],
            )
            for row in reader
        )


def _active_all_statement_map_concepts() -> set[tuple[str, str]]:
    supported = set(SUPPORTED_FACT_TAXONOMIES)
    return {
        (row.taxonomy, row.concept)
        for row in FUNDAMENTAL_STATEMENT_MAP_ROWS
        if row.industry_template == "ALL"
        and row.taxonomy in supported
        and row.is_active
        and not row.is_derived
        and not row.concept.startswith("__")
    }


def test_default_concepts_cover_active_all_statement_map_concepts():
    default_concepts = set(DEFAULT_CONCEPTS)
    missing = {
        f"{taxonomy}:{concept}"
        for taxonomy, concept in _active_all_statement_map_concepts()
        if concept not in default_concepts
    }
    assert not missing, f"active ALL-template concepts missing from DEFAULT_CONCEPTS: {sorted(missing)}"


def test_default_concepts_match_reviewable_concept_map_projection():
    rows = _read_concept_map_seed()
    seed_concepts = tuple(sorted({row[1] for row in rows}))
    assert DEFAULT_CONCEPTS == CANONICAL_CONCEPTS
    assert DEFAULT_CONCEPTS == seed_concepts


def test_concept_map_csv_round_trips_generated_projection():
    rows = _read_concept_map_seed()
    assert rows == concept_map_projection_rows()
    keys = [(row[0], row[1], row[4], row[5]) for row in rows]
    assert len(keys) == len(set(keys))


def test_concept_map_rows_match_statement_map_and_registry_item_ids():
    rows = _read_concept_map_seed()
    map_by_key = {
        (row.taxonomy, row.concept, row.industry_template): row
        for row in FUNDAMENTAL_STATEMENT_MAP_ROWS
    }
    registry_item_ids = {row.item_id for row in read_fundamental_item_seed()}

    for taxonomy, concept, canonical_metric, item_id, statement_type, industry_template in rows:
        statement_row = map_by_key[(taxonomy, concept, industry_template)]
        assert canonical_metric == statement_row.canonical_metric
        assert item_id == statement_row.item_id
        assert statement_type == statement_row.statement_type
        assert item_id in registry_item_ids


def test_ifrs_remains_excluded_from_loader_and_seed_projection():
    rows = _read_concept_map_seed()
    assert "ifrs-full" not in SUPPORTED_FACT_TAXONOMIES
    assert {row[0] for row in rows} <= set(SUPPORTED_FACT_TAXONOMIES)
    assert all(row[0] != "ifrs-full" for row in rows)
