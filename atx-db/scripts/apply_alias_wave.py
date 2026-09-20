#!/usr/bin/env python
"""Merge a curated alias wave into the fundamentals seeds, idempotently.

Inputs (all CSV, all committed under research/ for provenance):

  --aliases  item_id,taxonomy,concept,priority,statement_type,statement_section,
             canonical_metric,canonical_label,period_type,normal_balance,
             unit_type,industry_template,registry_alias,notes
             Attribute columns may be blank; they are then inherited from the
             item's first existing active statement-map row. registry_alias is
             'true' unless the concept must stay template-scoped: a concept that
             already belongs to a different item under the ALL template cannot
             also live in fundamental_items.csv, which has no template column and
             whose uniqueness is enforced by item_registry._validate_aliases.
  --items    exactly the fundamental_items.csv columns - new canonical items.
  --rules    exactly the standardization_rules.csv columns - new or replacement
             rules. A rule whose rule_id already exists is replaced in place.

Effects, all additive:
  1. new item rows appended to seeds/fundamental_items.csv
  2. new rule rows appended to (or replacing) seeds/standardization_rules.csv
  3. one seeds/statement_map.csv row per wave alias
  4. one seeds/fundamental_items.csv alias row per wave alias with registry_alias=true
  5. the alias appended to source_aliases_json of every active rule for that item
  6. scripts/normalize_fundamental_seeds.py applied

Re-running the same wave is a no-op.
"""
from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.fundamental_statements import SOURCE_NAME
from atx_db.item_registry import SEED_COLUMNS as ITEM_SEED_COLUMNS
from atx_db.item_registry import SEED_PATH as ITEM_SEED_PATH
from atx_db.standardization import RULE_COLUMNS, RULE_PATH
from atx_db.statement_map_seed import (
    STATEMENT_MAP_SEED_PATH,
    FundamentalStatementMapRow,
    read_statement_map_seed,
    write_statement_map_seed,
)

PROJECT_ROOT = Path(__file__).resolve().parents[1]

WAVE_COLUMNS = (
    "item_id",
    "taxonomy",
    "concept",
    "priority",
    "statement_type",
    "statement_section",
    "canonical_metric",
    "canonical_label",
    "period_type",
    "normal_balance",
    "unit_type",
    "industry_template",
    "registry_alias",
    "notes",
)
INHERITED_COLUMNS = (
    "statement_type",
    "statement_section",
    "canonical_metric",
    "canonical_label",
    "period_type",
    "normal_balance",
    "unit_type",
    "industry_template",
)
NORMALIZER = PROJECT_ROOT / "scripts" / "normalize_fundamental_seeds.py"


def _read_csv(path: Path, columns: tuple[str, ...]) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if tuple(reader.fieldnames or ()) != columns:
            raise SystemExit(f"{path} has unexpected columns: {reader.fieldnames}")
        return list(reader)


def _write_csv(path: Path, columns: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(columns))
        writer.writeheader()
        writer.writerows(rows)


def _truthy(value: str) -> bool:
    return value.strip().lower() in {"1", "true", "t", "yes", "y", ""}


def _inherit(wave_row: dict[str, str], template: FundamentalStatementMapRow | None) -> dict[str, str]:
    resolved = dict(wave_row)
    for column in INHERITED_COLUMNS:
        if resolved[column].strip():
            continue
        if template is None:
            raise SystemExit(
                f"item_id={wave_row['item_id']} concept={wave_row['concept']}: "
                f"{column} is blank and the item has no existing statement-map row to inherit from"
            )
        resolved[column] = str(getattr(template, column))
    return resolved


def apply_wave(aliases_path: Path, items_path: Path | None, rules_path: Path | None) -> dict[str, int]:
    item_rows = _read_csv(ITEM_SEED_PATH, ITEM_SEED_COLUMNS)
    rule_rows = _read_csv(RULE_PATH, RULE_COLUMNS)
    map_rows = list(read_statement_map_seed())

    added_items = 0
    if items_path is not None:
        existing = {(r["item_id"], r["alias_scheme"], r["alias_code"], r["vendor"], r["vendor_field"]) for r in item_rows}
        for row in _read_csv(items_path, ITEM_SEED_COLUMNS):
            key = (row["item_id"], row["alias_scheme"], row["alias_code"], row["vendor"], row["vendor_field"])
            if key in existing:
                continue
            item_rows.append(row)
            existing.add(key)
            added_items += 1

    added_rules = 0
    if rules_path is not None:
        by_id = {row["rule_id"]: index for index, row in enumerate(rule_rows)}
        for row in _read_csv(rules_path, RULE_COLUMNS):
            if row["rule_id"] in by_id:
                rule_rows[by_id[row["rule_id"]]] = row
            else:
                by_id[row["rule_id"]] = len(rule_rows)
                rule_rows.append(row)
            added_rules += 1

    item_attributes: dict[str, dict[str, str]] = {}
    for row in item_rows:
        item_attributes.setdefault(row["item_id"], row)

    template_by_item: dict[int, FundamentalStatementMapRow] = {}
    for row in map_rows:
        if row.item_id is not None and row.is_active and int(row.item_id) not in template_by_item:
            template_by_item[int(row.item_id)] = row

    map_keys = {(r.source, r.taxonomy, r.concept, r.industry_template) for r in map_rows}
    alias_keys = {(r["item_id"], r["alias_scheme"], r["alias_code"]) for r in item_rows if r["alias_scheme"].strip()}

    added_map = 0
    added_alias = 0
    touched_rules = 0
    for wave_row in _read_csv(aliases_path, WAVE_COLUMNS):
        item_id = int(wave_row["item_id"])
        resolved = _inherit(wave_row, template_by_item.get(item_id))
        key = (SOURCE_NAME, resolved["taxonomy"], resolved["concept"], resolved["industry_template"])
        if key not in map_keys:
            map_rows.append(
                FundamentalStatementMapRow(
                    source=SOURCE_NAME,
                    taxonomy=resolved["taxonomy"],
                    concept=resolved["concept"],
                    statement_type=resolved["statement_type"],
                    statement_section=resolved["statement_section"],
                    canonical_metric=resolved["canonical_metric"],
                    canonical_label=resolved["canonical_label"],
                    period_type=resolved["period_type"],
                    normal_balance=resolved["normal_balance"],
                    unit_type=resolved["unit_type"],
                    value_multiplier=1.0,
                    concept_priority=int(resolved["priority"]),
                    is_core_metric=True,
                    is_active=True,
                    notes=resolved["notes"] or None,
                    item_id=item_id,
                    industry_template=resolved["industry_template"],
                    is_derived=False,
                    derivation_expr=None,
                )
            )
            map_keys.add(key)
            added_map += 1

        if _truthy(resolved["registry_alias"]):
            alias_key = (resolved["item_id"], resolved["taxonomy"], resolved["concept"])
            if alias_key not in alias_keys:
                base = item_attributes[resolved["item_id"]]
                alias_row = {column: "" for column in ITEM_SEED_COLUMNS}
                for column in ("item_id", "canonical_code", "statement", "section", "data_type",
                               "unit_type", "sign_convention", "is_derived", "definition", "citation"):
                    alias_row[column] = base[column]
                alias_row["alias_scheme"] = resolved["taxonomy"]
                alias_row["alias_code"] = resolved["concept"]
                alias_row["coalesce_priority"] = resolved["priority"]
                alias_row["valid_from"] = "1900-01-01"
                alias_row["valid_to"] = ""
                item_rows.append(alias_row)
                alias_keys.add(alias_key)
                added_alias += 1

        for rule_row in rule_rows:
            if int(rule_row["item_id"]) != item_id or rule_row["is_active"].strip().lower() != "true":
                continue
            aliases = json.loads(rule_row["source_aliases_json"] or "[]")
            if any(a["alias_scheme"] == resolved["taxonomy"] and a["alias_code"] == resolved["concept"] for a in aliases):
                continue
            aliases.append(
                {
                    "alias_code": resolved["concept"],
                    "alias_scheme": resolved["taxonomy"],
                    "priority": int(resolved["priority"]),
                }
            )
            rule_row["source_aliases_json"] = json.dumps(aliases, separators=(",", ":"), sort_keys=True)
            touched_rules += 1

    _write_csv(ITEM_SEED_PATH, ITEM_SEED_COLUMNS, item_rows)
    _write_csv(RULE_PATH, RULE_COLUMNS, rule_rows)
    write_statement_map_seed(map_rows, STATEMENT_MAP_SEED_PATH)
    subprocess.run([sys.executable, str(NORMALIZER)], cwd=str(PROJECT_ROOT), check=True, capture_output=True)

    return {
        "items_added": added_items,
        "rules_added_or_replaced": added_rules,
        "statement_map_rows_added": added_map,
        "registry_alias_rows_added": added_alias,
        "rule_alias_json_updates": touched_rules,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aliases", type=Path, required=True)
    parser.add_argument("--items", type=Path)
    parser.add_argument("--rules", type=Path)
    args = parser.parse_args(argv)
    print(json.dumps(apply_wave(args.aliases, args.items, args.rules), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
