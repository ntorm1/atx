"""Tier1-S2 T7 review fix: apply_alias_wave.py must not drop existing rule aliases.

A `--rules` row that replaces an existing rule_id updates its structure
(basis, combination_rule, source_item_ids_json, etc.) but must merge into,
not overwrite, whatever `source_aliases_json` that rule already carries on
disk. Before the fix, a replacement row whose own `source_aliases_json` did
not restate a pre-existing alias would silently drop it - and because the
`--aliases` pass only ever *appends*, a second run of the very same wave
would then re-derive from a smaller base and disagree with the first run
(`rule_alias_json_updates` nonzero, content not byte-identical).

This test runs the real apply_wave() function against throwaway copies of
the committed seeds (never the real seed files - a mistake here must not be
able to corrupt src/atx_db/seeds/*.csv), with the real
normalize_fundamental_seeds.py subprocess call stubbed out (it is a separate
process that re-imports atx_db fresh and would therefore target the real
seed paths, not the tmp copies; normalization ordering is orthogonal to what
this test asserts).
"""

from __future__ import annotations

import csv
import functools
import importlib.util
import json
from pathlib import Path

import pytest

from atx_db.item_registry import SEED_PATH as REAL_ITEM_SEED_PATH
from atx_db.standardization import RULE_PATH as REAL_RULE_PATH
from atx_db.statement_map_seed import STATEMENT_MAP_SEED_PATH as REAL_STATEMENT_MAP_SEED_PATH

PROJECT_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = PROJECT_ROOT / "scripts" / "apply_alias_wave.py"

# A real, stable rule with two pre-existing aliases to exercise the merge.
TARGET_RULE_ID = "std_instant_1206"
PRE_EXISTING_ALIASES = {
    ("us-gaap", "LongTermDebtCurrent"),
    ("us-gaap", "LongTermDebtAndCapitalLeaseObligationsCurrent"),
}


def _load_module():
    spec = importlib.util.spec_from_file_location("apply_alias_wave_under_test", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


@pytest.fixture()
def sandboxed(tmp_path, monkeypatch):
    """apply_alias_wave.apply_wave(), rebound to throwaway seed copies."""
    tmp_items = tmp_path / "fundamental_items.csv"
    tmp_rules = tmp_path / "standardization_rules.csv"
    tmp_map = tmp_path / "statement_map.csv"
    tmp_items.write_bytes(REAL_ITEM_SEED_PATH.read_bytes())
    tmp_rules.write_bytes(REAL_RULE_PATH.read_bytes())
    tmp_map.write_bytes(REAL_STATEMENT_MAP_SEED_PATH.read_bytes())

    module = _load_module()
    monkeypatch.setattr(module, "ITEM_SEED_PATH", tmp_items)
    monkeypatch.setattr(module, "RULE_PATH", tmp_rules)
    monkeypatch.setattr(module, "STATEMENT_MAP_SEED_PATH", tmp_map)
    # apply_wave() calls read_statement_map_seed() with no argument, so its
    # own default (bound to the real seed path at import time) must be
    # rebound too - rebind the name in the module's own namespace.
    monkeypatch.setattr(
        module,
        "read_statement_map_seed",
        functools.partial(module.read_statement_map_seed, path=tmp_map),
    )
    # Never spawn the real normalizer against the real repo seeds from a test.
    monkeypatch.setattr(module.subprocess, "run", lambda *a, **k: None)
    return module, tmp_items, tmp_rules, tmp_map


def _rule_aliases(rules_path: Path, rule_id: str) -> set[tuple[str, str]]:
    with rules_path.open(newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            if row["rule_id"] == rule_id:
                aliases = json.loads(row["source_aliases_json"] or "[]")
                return {(a["alias_scheme"], a["alias_code"]) for a in aliases}
    raise AssertionError(f"{rule_id} not found in {rules_path}")


def test_rules_replace_merges_rather_than_drops_existing_aliases(sandboxed, tmp_path):
    module, tmp_items, tmp_rules, tmp_map = sandboxed

    # A synthetic --rules replacement for an existing rule_id that does NOT
    # restate either of its two pre-existing aliases (source_aliases_json is
    # empty) - simulating exactly the brief's wave-authoring pattern (a
    # rules CSV that only carries the *new* structural fields).
    synthetic_rules = tmp_path / "synthetic_rules.csv"
    with synthetic_rules.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(
            [
                "rule_id",
                "item_id",
                "canonical_code",
                "basis",
                "source_aliases_json",
                "source_item_ids_json",
                "combination_rule",
                "sign_rule",
                "scale_rule",
                "missing_policy",
                "is_active",
                "valid_from",
                "valid_to",
            ]
        )
        writer.writerow(
            [
                TARGET_RULE_ID,
                "1206",
                "current_portion_of_lt_debt",
                "instant",
                "[]",
                "[]",
                "coalesce_priority",
                "statement_normalized",
                "identity",
                "skip",
                "true",
                "1900-01-01",
                "",
            ]
        )

    # Empty (header-only) --aliases input: this test is only exercising the
    # --rules replace/merge path, not the alias-append path.
    empty_aliases = tmp_path / "synthetic_aliases.csv"
    with empty_aliases.open("w", newline="", encoding="utf-8") as fh:
        csv.writer(fh).writerow(list(module.WAVE_COLUMNS))

    first = module.apply_wave(aliases_path=empty_aliases, items_path=None, rules_path=synthetic_rules)
    assert first["rules_added_or_replaced"] == 1

    # (a) the pre-existing alias survives the replacement instead of being
    # reset to the synthetic row's empty source_aliases_json.
    assert _rule_aliases(tmp_rules, TARGET_RULE_ID) == PRE_EXISTING_ALIASES

    items_after_first = tmp_items.read_bytes()
    rules_after_first = tmp_rules.read_bytes()
    map_after_first = tmp_map.read_bytes()

    second = module.apply_wave(aliases_path=empty_aliases, items_path=None, rules_path=synthetic_rules)

    # (b) a second run is a true no-op: zero update stats and byte-identical
    # seed content, not just a zero rule_alias_json_updates stat while
    # content actually still changed underneath it.
    assert second == {
        "items_added": 0,
        "rules_added_or_replaced": 1,
        "statement_map_rows_added": 0,
        "registry_alias_rows_added": 0,
        "rule_alias_json_updates": 0,
    }
    assert _rule_aliases(tmp_rules, TARGET_RULE_ID) == PRE_EXISTING_ALIASES
    assert tmp_items.read_bytes() == items_after_first
    assert tmp_rules.read_bytes() == rules_after_first
    assert tmp_map.read_bytes() == map_after_first
