#!/usr/bin/env python
"""Rewrite the four fundamentals seeds in canonical sorted order.

Run after appending rows to any of:
  src/atx_db/seeds/statement_map.csv
  src/atx_db/seeds/fundamental_items.csv
  src/atx_db/seeds/standardization_rules.csv

concept_map.csv is never hand-edited: it is regenerated here as the exact
projection of the statement map, which is what
tests/test_concept_coverage.py::test_concept_map_csv_round_trips_generated_projection
asserts.

Ordering contracts
  statement_map.csv        (industry_template, statement_type, taxonomy, concept)
  fundamental_items.csv    (item_id, row_kind, coalesce_priority, alias_scheme,
                            alias_code, vendor, vendor_field) where row_kind is
                            0 alias / 1 vendor / 2 bare
  standardization_rules.csv(item_id, BASIS_ORDER.index(basis)); inside each rule
                            source_aliases_json is sorted by
                            (priority, alias_scheme, alias_code)
  concept_map.csv           whatever concept_map_projection_rows() emits
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.fundamental_statements import CONCEPT_MAP_SEED_COLUMNS, concept_map_projection_rows
from atx_db.item_registry import SEED_COLUMNS as ITEM_SEED_COLUMNS
from atx_db.item_registry import SEED_PATH as ITEM_SEED_PATH
from atx_db.standardization import RULE_COLUMNS, RULE_PATH
from atx_db.statement_map_seed import (
    STATEMENT_MAP_SEED_PATH,
    read_statement_map_seed,
    statement_map_sort_key,
    write_statement_map_seed,
)

SEEDS_DIR = Path(__file__).resolve().parents[1] / "src" / "atx_db" / "seeds"
CONCEPT_MAP_PATH = SEEDS_DIR / "concept_map.csv"

BASIS_ORDER = ("annual", "quarterly", "ttm", "instant")
RULE_SORT_KEY_COLUMNS = ("item_id", "basis")
ITEM_SORT_KEY_COLUMNS = (
    "item_id",
    "row_kind",
    "coalesce_priority",
    "alias_scheme",
    "alias_code",
    "vendor",
    "vendor_field",
)


def _read_rows(path: Path, columns: tuple[str, ...]) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if tuple(reader.fieldnames or ()) != columns:
            raise SystemExit(f"{path} has unexpected columns: {reader.fieldnames}")
        return list(reader)


def _write_rows(path: Path, columns: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(columns))
        writer.writeheader()
        writer.writerows(rows)


def _rule_sort_key(row: dict[str, str]) -> tuple[int, int]:
    return (int(row["item_id"]), BASIS_ORDER.index(row["basis"]))


def _normalized_alias_json(raw: str) -> str:
    aliases = json.loads(raw or "[]")
    ordered = sorted(
        aliases,
        key=lambda a: (int(a.get("priority", 100)), str(a["alias_scheme"]), str(a["alias_code"])),
    )
    rebuilt = [
        {
            "alias_code": str(a["alias_code"]),
            "alias_scheme": str(a["alias_scheme"]),
            "priority": int(a.get("priority", 100)),
        }
        for a in ordered
    ]
    return json.dumps(rebuilt, separators=(",", ":"), sort_keys=True)


def _item_sort_key(row: dict[str, str]) -> tuple[int, int, int, str, str, str, str]:
    has_alias = bool(row["alias_scheme"].strip())
    has_vendor = bool(row["vendor"].strip())
    row_kind = 0 if has_alias else (1 if has_vendor else 2)
    priority = int(row["coalesce_priority"]) if row["coalesce_priority"].strip() else 0
    return (
        int(row["item_id"]),
        row_kind,
        priority,
        row["alias_scheme"],
        row["alias_code"],
        row["vendor"],
        row["vendor_field"],
    )


def _canonical_payloads() -> dict[Path, bytes]:
    payloads: dict[Path, bytes] = {}

    statement_rows = sorted(read_statement_map_seed(), key=statement_map_sort_key)
    scratch = STATEMENT_MAP_SEED_PATH.with_suffix(".csv.normalize-tmp")
    write_statement_map_seed(statement_rows, scratch)
    payloads[STATEMENT_MAP_SEED_PATH] = scratch.read_bytes()
    scratch.unlink()

    rule_rows = _read_rows(RULE_PATH, RULE_COLUMNS)
    for row in rule_rows:
        row["source_aliases_json"] = _normalized_alias_json(row["source_aliases_json"])
    rule_rows.sort(key=_rule_sort_key)
    scratch = RULE_PATH.with_suffix(".csv.normalize-tmp")
    _write_rows(scratch, RULE_COLUMNS, rule_rows)
    payloads[RULE_PATH] = scratch.read_bytes()
    scratch.unlink()

    item_rows = _read_rows(ITEM_SEED_PATH, ITEM_SEED_COLUMNS)
    item_rows.sort(key=_item_sort_key)
    scratch = ITEM_SEED_PATH.with_suffix(".csv.normalize-tmp")
    _write_rows(scratch, ITEM_SEED_COLUMNS, item_rows)
    payloads[ITEM_SEED_PATH] = scratch.read_bytes()
    scratch.unlink()

    concept_rows = [dict(zip(CONCEPT_MAP_SEED_COLUMNS, values, strict=True)) for values in concept_map_projection_rows()]
    for row in concept_rows:
        row["item_id"] = str(row["item_id"])
    scratch = CONCEPT_MAP_PATH.with_suffix(".csv.normalize-tmp")
    _write_rows(scratch, CONCEPT_MAP_SEED_COLUMNS, concept_rows)
    payloads[CONCEPT_MAP_PATH] = scratch.read_bytes()
    scratch.unlink()

    return payloads


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="report drift without writing")
    args = parser.parse_args(argv)

    payloads = _canonical_payloads()
    dirty = sorted(
        path.name for path, payload in payloads.items() if path.read_bytes() != payload
    )
    if not args.check:
        for path, payload in payloads.items():
            if path.read_bytes() != payload:
                path.write_bytes(payload)
    print(json.dumps({"dirty": dirty, "checked": sorted(p.name for p in payloads)}, indent=2))
    if args.check and dirty:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
