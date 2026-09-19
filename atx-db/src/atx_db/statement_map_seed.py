"""Tier1-S2 T1: CSV-backed seed for the canonical XBRL statement map.

The 214-row FUNDAMENTAL_STATEMENT_MAP_ROWS literal used to live inside the
6,456-line fundamental_statements.py, which made it the highest-churn file in
the fundamentals chain. It is data, so it belongs beside the other registry
seeds and inside the conftest schema fingerprint.
"""
from __future__ import annotations

import csv
from collections.abc import Iterable
from dataclasses import dataclass, fields
from functools import lru_cache
from pathlib import Path
from typing import NoReturn

STATEMENT_MAP_SEED_PATH = Path(__file__).resolve().parent / "seeds" / "statement_map.csv"


@dataclass(frozen=True)
class FundamentalStatementMapRow:
    source: str
    taxonomy: str
    concept: str
    statement_type: str
    statement_section: str
    canonical_metric: str
    canonical_label: str
    period_type: str
    normal_balance: str
    unit_type: str
    value_multiplier: float
    concept_priority: int
    is_core_metric: bool
    is_active: bool
    notes: str | None = None
    item_id: int | None = None
    industry_template: str = "ALL"
    is_derived: bool = False
    derivation_expr: str | None = None


STATEMENT_MAP_SEED_COLUMNS: tuple[str, ...] = tuple(f.name for f in fields(FundamentalStatementMapRow))

_TRUE_TOKENS = frozenset({"1", "true", "t", "yes", "y"})
_FALSE_TOKENS = frozenset({"0", "false", "f", "no", "n"})


def statement_map_sort_key(row: FundamentalStatementMapRow) -> tuple[str, str, str, str]:
    """Canonical seed ordering: source, taxonomy, concept, industry_template."""

    return (row.source, row.taxonomy, row.concept, row.industry_template)


def _fail(seed_path: Path, row_number: int, message: str) -> NoReturn:
    raise ValueError(f"{seed_path} row {row_number}: {message}")


def _none_if_blank(value: str | None) -> str | None:
    if value is None:
        return None
    value = value.strip()
    return value or None


def _parse_bool(value: str, *, seed_path: Path, row_number: int, field_name: str) -> bool:
    token = value.strip().lower()
    if token in _TRUE_TOKENS:
        return True
    if token in _FALSE_TOKENS:
        return False
    _fail(seed_path, row_number, f"invalid {field_name} boolean {value!r}")


def _parse_int_or_none(value: str, *, seed_path: Path, row_number: int, field_name: str) -> int | None:
    token = _none_if_blank(value)
    if token is None:
        return None
    try:
        return int(token)
    except ValueError:
        _fail(seed_path, row_number, f"invalid {field_name} integer {value!r}")


def _parse_row(raw: dict[str, str], *, seed_path: Path, row_number: int) -> FundamentalStatementMapRow:
    missing = [column for column in STATEMENT_MAP_SEED_COLUMNS if raw.get(column) is None]
    if missing:
        _fail(seed_path, row_number, f"missing CSV values for fields {missing!r}")
    for column in ("source", "taxonomy", "concept", "statement_type", "statement_section",
                   "canonical_metric", "canonical_label", "period_type", "normal_balance",
                   "unit_type", "industry_template"):
        if _none_if_blank(raw[column]) is None:
            _fail(seed_path, row_number, f"blank required field {column}")
    try:
        value_multiplier = float(raw["value_multiplier"])
    except ValueError:
        _fail(seed_path, row_number, f"invalid value_multiplier {raw['value_multiplier']!r}")
    concept_priority = _parse_int_or_none(
        raw["concept_priority"], seed_path=seed_path, row_number=row_number, field_name="concept_priority"
    )
    if concept_priority is None:
        _fail(seed_path, row_number, "blank required field concept_priority")
    return FundamentalStatementMapRow(
        source=raw["source"].strip(),
        taxonomy=raw["taxonomy"].strip(),
        concept=raw["concept"].strip(),
        statement_type=raw["statement_type"].strip(),
        statement_section=raw["statement_section"].strip(),
        canonical_metric=raw["canonical_metric"].strip(),
        canonical_label=raw["canonical_label"].strip(),
        period_type=raw["period_type"].strip(),
        normal_balance=raw["normal_balance"].strip(),
        unit_type=raw["unit_type"].strip(),
        value_multiplier=value_multiplier,
        concept_priority=concept_priority,
        is_core_metric=_parse_bool(raw["is_core_metric"], seed_path=seed_path, row_number=row_number, field_name="is_core_metric"),
        is_active=_parse_bool(raw["is_active"], seed_path=seed_path, row_number=row_number, field_name="is_active"),
        notes=_none_if_blank(raw["notes"]),
        item_id=_parse_int_or_none(raw["item_id"], seed_path=seed_path, row_number=row_number, field_name="item_id"),
        industry_template=raw["industry_template"].strip(),
        is_derived=_parse_bool(raw["is_derived"], seed_path=seed_path, row_number=row_number, field_name="is_derived"),
        derivation_expr=_none_if_blank(raw["derivation_expr"]),
    )


def read_statement_map_seed(
    path: Path | str = STATEMENT_MAP_SEED_PATH,
) -> tuple[FundamentalStatementMapRow, ...]:
    """Read the committed offline statement-map seed with stdlib csv."""

    seed_path = Path(path)
    with seed_path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if tuple(reader.fieldnames or ()) != STATEMENT_MAP_SEED_COLUMNS:
            raise ValueError(f"{seed_path} has unexpected columns: {reader.fieldnames}")
        return tuple(
            _parse_row(row, seed_path=seed_path, row_number=row_number)
            for row_number, row in enumerate(reader, start=2)
        )


def _serialize(value: object) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float):
        return repr(value)
    return str(value)


def write_statement_map_seed(
    rows: Iterable[FundamentalStatementMapRow],
    path: Path | str = STATEMENT_MAP_SEED_PATH,
) -> int:
    """Write rows in canonical sorted order; return the row count.

    Uses the csv module default CRLF terminator, matching the other four
    committed seeds -- fundamental_items.csv, standardization_rules.csv,
    concept_map.csv and formula_registry.csv are all CRLF.
    """

    ordered = sorted(rows, key=statement_map_sort_key)
    seed_path = Path(path)
    with seed_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(STATEMENT_MAP_SEED_COLUMNS)
        for row in ordered:
            writer.writerow([_serialize(getattr(row, column)) for column in STATEMENT_MAP_SEED_COLUMNS])
    return len(ordered)


@lru_cache(maxsize=1)
def default_statement_map_rows() -> tuple[FundamentalStatementMapRow, ...]:
    """Process-local cache of the committed statement-map seed."""

    return read_statement_map_seed(STATEMENT_MAP_SEED_PATH)
