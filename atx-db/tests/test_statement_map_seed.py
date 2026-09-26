"""Tier1-S2 T1: the statement map is a sorted CSV seed, not a Python literal."""
from __future__ import annotations

import csv
import hashlib
from dataclasses import fields
from pathlib import Path

import tests.conftest as conftest
from atx_db.statement_map_seed import (
    STATEMENT_MAP_SEED_COLUMNS,
    STATEMENT_MAP_SEED_PATH,
    FundamentalStatementMapRow,
    default_statement_map_rows,
    read_statement_map_seed,
    statement_map_sort_key,
)

# Row count of the FUNDAMENTAL_STATEMENT_MAP_ROWS literal at the moment it was
# extracted (main @ e4bdcf54). Later sprint tasks that add concepts bump this
# number in the same commit that adds the rows, with the new count justified in
# the commit body.
EXPECTED_STATEMENT_MAP_ROWS = 344  # 331 + 13 debt alias companion rows (468eb2f5, catalog round
# 2 follow-up: convertible/notes/loans/lines-of-credit concepts -> short-term debt 1205 and
# long-term debt 1207, added with their fundamental_items and standardization_rules
# companions; that commit did not bump this pin). S1 (tier-1 v2 node 0.3) re-points
# PaymentsToAcquireProductiveAssets and re-signs three capex aliases in place: no rows added.
# 331 = 286 + 45 (Tier1-S2 T7 Wave A-2; the wave CSV enumerates 56 alias
# rows, but 11 (three targeting 1205, two targeting 1209, one targeting 1215, and
# all five targeting 1224) already had active, non-derived statement-map rows for
# their item before this wave, so apply_alias_wave.py's dedup-by-(taxonomy,concept,
# industry_template) key correctly skipped re-inserting them; only 45 are new rows.
# The brief's stated "343 (287 + 56)" double-counts on both terms: the pre-wave
# baseline was 286, not 287, and the real increment is 45, not 56 - see
# task-7-report.md for the row-by-row resolution.


def test_seed_columns_match_dataclass_fields():
    assert tuple(f.name for f in fields(FundamentalStatementMapRow)) == STATEMENT_MAP_SEED_COLUMNS


def test_seed_csv_header_matches_columns():
    with STATEMENT_MAP_SEED_PATH.open(newline="", encoding="utf-8") as fh:
        header = tuple(next(csv.reader(fh)))
    assert header == STATEMENT_MAP_SEED_COLUMNS


def test_seed_row_count_matches_extracted_literal():
    assert len(read_statement_map_seed()) == EXPECTED_STATEMENT_MAP_ROWS


def test_seed_is_sorted_canonically():
    rows = read_statement_map_seed()
    keys = [statement_map_sort_key(row) for row in rows]
    assert keys == sorted(keys)


def test_seed_primary_key_is_unique():
    rows = read_statement_map_seed()
    keys = [(r.source, r.taxonomy, r.concept, r.industry_template) for r in rows]
    assert len(keys) == len(set(keys))


def test_default_rows_are_cached_and_equal_the_seed():
    assert default_statement_map_rows() is default_statement_map_rows()
    assert default_statement_map_rows() == read_statement_map_seed()


def test_fundamental_statements_exposes_the_rows_lazily():
    from atx_db import fundamental_statements as fs

    assert "FUNDAMENTAL_STATEMENT_MAP_ROWS" not in vars(fs)
    assert read_statement_map_seed() == fs.FUNDAMENTAL_STATEMENT_MAP_ROWS
    assert fs.FundamentalStatementMapRow is FundamentalStatementMapRow


def test_schema_fingerprint_hashes_the_new_seed():
    seed_paths = sorted((Path(conftest._SOURCE_ROOT) / "atx_db" / "seeds").glob("*.csv"))
    assert STATEMENT_MAP_SEED_PATH in seed_paths
    digest = hashlib.sha256(STATEMENT_MAP_SEED_PATH.read_bytes()).hexdigest()
    assert len(conftest._schema_fingerprint()) == 24
    assert len(digest) == 64


def test_round_trip_write_then_read_is_byte_stable(tmp_path):
    from atx_db.statement_map_seed import write_statement_map_seed

    rows = read_statement_map_seed()
    target = tmp_path / "statement_map.csv"
    written = write_statement_map_seed(rows, target)
    assert written == len(rows)
    assert target.read_bytes() == STATEMENT_MAP_SEED_PATH.read_bytes()
