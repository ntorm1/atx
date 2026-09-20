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
EXPECTED_STATEMENT_MAP_ROWS = 286  # 214 + 72 (Tier1-S2 T6 Wave A-1; the wave CSV enumerates 72 alias rows, not 73)


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
