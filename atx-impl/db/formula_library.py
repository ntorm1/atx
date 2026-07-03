"""PF-S4 S4-0: formula_registry seed catalog -- definition-as-data for formulas.

This module is the FOUNDATION for the formula library: it defines the seed
row shape and the strict CSV contract for ``formula_registry``, mirroring
the item_registry seed pattern (``db/item_registry.py``) exactly:

* ``csv.DictReader`` with a strict ``fieldnames == SEED_COLUMNS`` contract
  (:func:`read_formula_registry_seed`).
* A frozen seed-row dataclass (:class:`FormulaRegistrySeedRow`).
* A direct-call loader -- NOT a :class:`db.dataset.Dataset` subclass, mirroring
  ``seed_fundamental_item_registry`` -- that DELETEs by the seed's own
  ``formula_code`` set then upserts, all inside one transaction
  (:func:`seed_formula_registry`).

Definition-as-data + citation precedent: like ``fundamental_items.csv``
(``item_registry.SEED_COLUMNS`` -- ``definition``/``citation`` columns), every
seed row carries a human ``definition`` and an optional academic/vendor
``citation``. Unlike the item registry (which requires a non-blank citation
on every row), ``citation`` here may be blank: the brief is explicit that
citation is empty for plain accounting ratios and required only for scores
and named academic formulas -- that requirement is enforced by review/S4-1/
S4-2 content discipline, not a blanket non-blank check in this loader.

S4-0 ships schema + the seed loader ONLY. No formula codes are ported here
(the committed seed CSV is a minimal placeholder); S4-1 mechanically
translates the 53 existing ``RatioDef`` entries into registry rows under a
byte-identity gate, and S4-2 adds new formula families as additional rows.
"""
from __future__ import annotations

import csv
import json
import re
from dataclasses import dataclass
from datetime import date
from pathlib import Path

from .connection import DuckDBStore


SEED_PATH = Path(__file__).resolve().parent / "seeds" / "formula_registry.csv"

# Seed CSV column names. NOTE these differ slightly from the warehouse table
# column names: the CSV carries the un-suffixed `numerator_item_ids` /
# `denominator_item_ids` / `inputs` (each a JSON array literal, validated at
# read time); the table stores them with an explicit `_json` suffix
# (`numerator_item_ids_json` etc.) to match the warehouse's existing
# `*_json` VARCHAR convention (input_codes_json, natural_key_json, ...).
SEED_COLUMNS = (
    "formula_code",
    "family",
    "kind",
    "unit",
    "numerator_code",
    "denominator_code",
    "numerator_item_ids",
    "denominator_item_ids",
    "inputs",
    "transform",
    "expression",
    "is_meaningful_rule",
    "definition",
    "citation",
    "valid_from",
    "valid_to",
)

# Governed enums. `kind` mirrors fundamental_ratios.RatioDef.kind exactly
# (fundamental_ratios.py RatioDef :76-77 docstring). `transform` is the
# reducer-selector mini-grammar named in the S4-0 brief: divide/sum/
# difference/pct_change cover the existing kind branches in
# compute_ratio_rows (:618-633); identity covers multi-term `expression`
# formulas (composites, DuPont) where the top-level value is not itself a
# binary reduction of numerator/denominator.
VALID_KINDS = frozenset({"ratio", "level", "difference", "growth", "per_share", "score"})
VALID_TRANSFORMS = frozenset({"divide", "sum", "difference", "pct_change", "identity"})


@dataclass(frozen=True)
class FormulaRegistrySeedRow:
    formula_code: str
    family: str
    kind: str
    unit: str
    numerator_code: str | None
    denominator_code: str | None
    numerator_item_ids: str | None
    denominator_item_ids: str | None
    inputs: str
    transform: str
    expression: str | None
    is_meaningful_rule: str | None
    definition: str
    citation: str | None
    valid_from: str
    valid_to: str | None


def _none_if_blank(value: str | None) -> str | None:
    if value is None:
        return None
    value = value.strip()
    return value or None


def _fail(seed_path: Path, row_number: int, message: str) -> None:
    raise ValueError(f"{seed_path} row {row_number}: {message}")


def _validate_date(value: str | None, *, seed_path: Path, row_number: int, field_name: str) -> None:
    value = _none_if_blank(value)
    if value is None:
        return
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}", value) is None:
        _fail(seed_path, row_number, f"invalid {field_name} date {value!r}")
    try:
        date.fromisoformat(value)
    except ValueError:
        _fail(seed_path, row_number, f"invalid {field_name} date {value!r}")


def _validate_json_array(
    value: str | None, *, seed_path: Path, row_number: int, field_name: str
) -> None:
    if value is None:
        return
    try:
        parsed = json.loads(value)
    except (TypeError, ValueError) as exc:
        _fail(seed_path, row_number, f"invalid {field_name} JSON {value!r} ({exc})")
        return
    if not isinstance(parsed, list):
        _fail(seed_path, row_number, f"invalid {field_name} JSON {value!r}: expected a JSON array")


def _validate_raw_row(raw: dict[str, str], *, seed_path: Path, row_number: int) -> None:
    if None in raw:
        _fail(seed_path, row_number, f"extra CSV fields {raw[None]!r}")

    missing_columns = [column for column in SEED_COLUMNS if column not in raw]
    if missing_columns:
        _fail(seed_path, row_number, f"missing expected fields {missing_columns!r}")

    missing_values = [column for column in SEED_COLUMNS if raw[column] is None]
    if missing_values:
        _fail(seed_path, row_number, f"missing CSV values for fields {missing_values!r}")

    for column in ("formula_code", "family", "kind", "unit", "transform", "definition", "inputs"):
        if _none_if_blank(raw[column]) is None:
            _fail(seed_path, row_number, f"blank required field {column}")

    kind = raw["kind"].strip()
    if kind not in VALID_KINDS:
        _fail(
            seed_path,
            row_number,
            f"invalid kind {kind!r}; expected one of {sorted(VALID_KINDS)}",
        )

    transform = raw["transform"].strip()
    if transform not in VALID_TRANSFORMS:
        _fail(
            seed_path,
            row_number,
            f"invalid transform {transform!r}; expected one of {sorted(VALID_TRANSFORMS)}",
        )

    numerator_code = _none_if_blank(raw["numerator_code"])
    denominator_code = _none_if_blank(raw["denominator_code"])
    if (numerator_code is None) != (denominator_code is None):
        _fail(seed_path, row_number, "partial numerator/denominator code pair")

    _validate_json_array(
        raw["inputs"], seed_path=seed_path, row_number=row_number, field_name="inputs"
    )
    _validate_json_array(
        _none_if_blank(raw["numerator_item_ids"]),
        seed_path=seed_path,
        row_number=row_number,
        field_name="numerator_item_ids",
    )
    _validate_json_array(
        _none_if_blank(raw["denominator_item_ids"]),
        seed_path=seed_path,
        row_number=row_number,
        field_name="denominator_item_ids",
    )

    _validate_date(raw["valid_from"], seed_path=seed_path, row_number=row_number, field_name="valid_from")
    _validate_date(raw["valid_to"], seed_path=seed_path, row_number=row_number, field_name="valid_to")


def _seed_row(raw: dict[str, str], *, seed_path: Path, row_number: int) -> FormulaRegistrySeedRow:
    _validate_raw_row(raw, seed_path=seed_path, row_number=row_number)
    valid_from = _none_if_blank(raw["valid_from"])
    if valid_from is None:
        _fail(seed_path, row_number, "blank required field valid_from")
    return FormulaRegistrySeedRow(
        formula_code=raw["formula_code"].strip(),
        family=raw["family"].strip(),
        kind=raw["kind"].strip(),
        unit=raw["unit"].strip(),
        numerator_code=_none_if_blank(raw["numerator_code"]),
        denominator_code=_none_if_blank(raw["denominator_code"]),
        numerator_item_ids=_none_if_blank(raw["numerator_item_ids"]),
        denominator_item_ids=_none_if_blank(raw["denominator_item_ids"]),
        inputs=raw["inputs"].strip(),
        transform=raw["transform"].strip(),
        expression=_none_if_blank(raw["expression"]),
        is_meaningful_rule=_none_if_blank(raw["is_meaningful_rule"]),
        definition=raw["definition"].strip(),
        citation=_none_if_blank(raw["citation"]),
        valid_from=valid_from,
        valid_to=_none_if_blank(raw["valid_to"]),
    )


def read_formula_registry_seed(path: Path | str = SEED_PATH) -> tuple[FormulaRegistrySeedRow, ...]:
    """Read the committed offline formula registry seed with stdlib csv."""

    seed_path = Path(path)
    with seed_path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if tuple(reader.fieldnames or ()) != SEED_COLUMNS:
            raise ValueError(f"{seed_path} has unexpected columns: {reader.fieldnames}")
        return tuple(
            _seed_row(row, seed_path=seed_path, row_number=row_number)
            for row_number, row in enumerate(reader, start=2)
        )


def _dedupe_formula_rows(
    rows: tuple[FormulaRegistrySeedRow, ...],
) -> dict[str, FormulaRegistrySeedRow]:
    """Collapse seed rows to one per ``formula_code``, rejecting conflicting duplicates.

    An exact repeated row (identical in every field) is tolerated the same
    way item_registry tolerates repeated alias rows; a duplicate
    ``formula_code`` with DIFFERENT field values is a seed authoring error
    and fails closed.
    """
    by_code: dict[str, FormulaRegistrySeedRow] = {}
    for row in rows:
        existing = by_code.get(row.formula_code)
        if existing is not None and existing != row:
            raise ValueError(f"Duplicate conflicting formula_registry seed rows for formula_code={row.formula_code}")
        by_code[row.formula_code] = row
    return by_code


def seed_formula_registry(
    store: DuckDBStore,
    *,
    seed_path: Path | str = SEED_PATH,
) -> int:
    """Seed the formula registry from the committed CSV.

    Mirrors ``item_registry.seed_fundamental_item_registry``: DELETE the rows
    for every ``formula_code`` present in the seed file, then re-INSERT them,
    all inside one transaction. Codes NOT present in the seed file (e.g. a
    formula retired from the CSV but still present from a prior load) are
    left untouched, matching the item registry's DELETE-by-ids-then-upsert
    contract.
    """

    rows = read_formula_registry_seed(seed_path)
    by_code = _dedupe_formula_rows(rows)
    seed_codes = sorted(by_code)

    with store.transaction():
        store.con.execute(
            "DELETE FROM formula_registry WHERE formula_code = ANY(?)",
            [seed_codes],
        )
        for code in seed_codes:
            row = by_code[code]
            store.con.execute(
                """
                INSERT INTO formula_registry (
                    formula_code,
                    family,
                    kind,
                    unit,
                    numerator_code,
                    denominator_code,
                    numerator_item_ids_json,
                    denominator_item_ids_json,
                    inputs_json,
                    transform,
                    expression,
                    is_meaningful_rule,
                    definition,
                    citation,
                    valid_from,
                    valid_to
                )
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CAST(? AS DATE), CAST(? AS DATE))
                """,
                [
                    row.formula_code,
                    row.family,
                    row.kind,
                    row.unit,
                    row.numerator_code,
                    row.denominator_code,
                    row.numerator_item_ids,
                    row.denominator_item_ids,
                    row.inputs,
                    row.transform,
                    row.expression,
                    row.is_meaningful_rule,
                    row.definition,
                    row.citation,
                    row.valid_from,
                    row.valid_to,
                ],
            )

    return len(seed_codes)
