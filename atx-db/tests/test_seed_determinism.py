"""Tier1-S2 T3: all four fundamentals seeds are canonically sorted and duplicate-free."""
from __future__ import annotations

import csv
import json
import subprocess
import sys
from collections import Counter
from pathlib import Path

from atx_db.item_registry import SEED_PATH as ITEM_SEED_PATH
from atx_db.item_registry import read_fundamental_item_seed
from atx_db.standardization import RULE_PATH, read_standardization_rules
from atx_db.statement_map_seed import read_statement_map_seed, statement_map_sort_key

PROJECT_ROOT = Path(__file__).resolve().parents[1]
NORMALIZER = PROJECT_ROOT / "scripts" / "normalize_fundamental_seeds.py"
BASIS_ORDER = ("annual", "quarterly", "ttm", "instant")


def _rule_rows() -> list[dict[str, str]]:
    with RULE_PATH.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def _item_rows() -> list[dict[str, str]]:
    with ITEM_SEED_PATH.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def test_rules_csv_is_sorted_by_item_then_basis():
    keys = [(int(r["item_id"]), BASIS_ORDER.index(r["basis"])) for r in _rule_rows()]
    assert keys == sorted(keys)


def test_rule_alias_json_is_sorted_by_priority_then_scheme_then_code():
    for row in _rule_rows():
        aliases = json.loads(row["source_aliases_json"] or "[]")
        keys = [(int(a.get("priority", 100)), a["alias_scheme"], a["alias_code"]) for a in aliases]
        assert keys == sorted(keys), row["rule_id"]


def test_rule_alias_json_has_no_duplicate_alias_within_a_rule():
    for row in _rule_rows():
        aliases = json.loads(row["source_aliases_json"] or "[]")
        pairs = [(a["alias_scheme"], a["alias_code"]) for a in aliases]
        assert len(pairs) == len(set(pairs)), row["rule_id"]


def test_item_seed_is_grouped_by_item_and_sorted_within_the_group():
    rows = _item_rows()
    keys = [
        (
            int(r["item_id"]),
            0 if r["alias_scheme"].strip() else (1 if r["vendor"].strip() else 2),
            int(r["coalesce_priority"]) if r["coalesce_priority"].strip() else 0,
            r["alias_scheme"],
            r["alias_code"],
            r["vendor"],
            r["vendor_field"],
        )
        for r in rows
    ]
    assert keys == sorted(keys)


def test_no_duplicate_item_alias_pairs_in_the_item_seed():
    counts = Counter(
        (int(r["item_id"]), r["alias_scheme"], r["alias_code"])
        for r in _item_rows()
        if r["alias_scheme"].strip()
    )
    assert [k for k, n in counts.items() if n > 1] == []


def test_no_alias_code_maps_to_two_items_in_the_item_seed():
    owners: dict[tuple[str, str], set[int]] = {}
    for row in read_fundamental_item_seed():
        if row.alias_scheme is None or row.alias_code is None:
            continue
        owners.setdefault((row.alias_scheme, row.alias_code), set()).add(row.item_id)
    conflicts = {k: sorted(v) for k, v in owners.items() if len(v) > 1}
    assert conflicts == {}


def test_statement_map_seed_is_sorted():
    keys = [statement_map_sort_key(row) for row in read_statement_map_seed()]
    assert keys == sorted(keys)


def test_rules_have_one_active_rule_per_item_and_basis():
    seen: set[tuple[int, str]] = set()
    for rule in read_standardization_rules():
        if not rule.is_active:
            continue
        key = (rule.item_id, rule.basis)
        assert key not in seen, key
        seen.add(key)


def test_normalizer_check_mode_reports_the_seeds_are_already_canonical():
    result = subprocess.run(
        [sys.executable, str(NORMALIZER), "--check"],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert json.loads(result.stdout)["dirty"] == []
