from __future__ import annotations

import csv
from dataclasses import dataclass
from datetime import date
from pathlib import Path
import re

from .connection import DuckDBStore


SEED_PATH = Path(__file__).resolve().parent / "seeds" / "fundamental_items.csv"

SEED_COLUMNS = (
    "item_id",
    "canonical_code",
    "statement",
    "section",
    "data_type",
    "unit_type",
    "sign_convention",
    "is_derived",
    "definition",
    "citation",
    "alias_scheme",
    "alias_code",
    "coalesce_priority",
    "valid_from",
    "valid_to",
    "vendor",
    "vendor_field",
    "sign_note",
)

ItemRecord = tuple[
    int,
    str,
    str | None,
    str | None,
    str | None,
    str | None,
    str | None,
    bool,
    str | None,
    str | None,
]


@dataclass(frozen=True)
class FundamentalItemSeedRow:
    item_id: int
    canonical_code: str
    statement: str | None
    section: str | None
    data_type: str | None
    unit_type: str | None
    sign_convention: str | None
    is_derived: bool
    definition: str | None
    citation: str | None
    alias_scheme: str | None
    alias_code: str | None
    coalesce_priority: int | None
    valid_from: str | None
    valid_to: str | None
    vendor: str | None
    vendor_field: str | None
    sign_note: str | None


def _none_if_blank(value: str | None) -> str | None:
    if value is None:
        return None
    value = value.strip()
    return value or None


def _fail(seed_path: Path, row_number: int, message: str) -> None:
    raise ValueError(f"{seed_path} row {row_number}: {message}")


def _parse_bool(value: str, *, seed_path: Path, row_number: int) -> bool:
    token = value.strip().lower()
    if token in {"1", "true", "t", "yes", "y"}:
        return True
    if token in {"0", "false", "f", "no", "n"}:
        return False
    _fail(seed_path, row_number, f"invalid is_derived token {value!r}")


def _parse_int(
    value: str | None,
    *,
    seed_path: Path,
    row_number: int,
    field_name: str,
) -> int | None:
    value = _none_if_blank(value)
    if value is None:
        return None
    try:
        return int(value)
    except ValueError:
        _fail(seed_path, row_number, f"invalid {field_name} integer {value!r}")


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


def _is_expression_vendor_field(value: str) -> bool:
    return (
        any(marker in value for marker in (" + ", " - ", " * ", " / "))
        or re.search(r"[A-Za-z0-9_][+\-*/][A-Za-z0-9_]", value) is not None
        or "/" in value
        or "(" in value
        or ")" in value
    )


def _validate_raw_row(raw: dict[str, str], *, seed_path: Path, row_number: int) -> None:
    if None in raw:
        _fail(seed_path, row_number, f"extra CSV fields {raw[None]!r}")

    missing_columns = [column for column in SEED_COLUMNS if column not in raw]
    if missing_columns:
        _fail(seed_path, row_number, f"missing expected fields {missing_columns!r}")

    missing_values = [column for column in SEED_COLUMNS if raw[column] is None]
    if missing_values:
        _fail(seed_path, row_number, f"missing CSV values for fields {missing_values!r}")

    for column in (
        "item_id",
        "canonical_code",
        "statement",
        "section",
        "data_type",
        "unit_type",
        "sign_convention",
        "is_derived",
        "definition",
        "citation",
    ):
        if _none_if_blank(raw[column]) is None:
            _fail(seed_path, row_number, f"blank required field {column}")

    alias_scheme = _none_if_blank(raw["alias_scheme"])
    alias_code = _none_if_blank(raw["alias_code"])
    if (alias_scheme is None) != (alias_code is None):
        _fail(seed_path, row_number, "partial alias requires both alias_scheme and alias_code")
    if alias_code is not None and ("[" in alias_code or "]" in alias_code):
        _fail(seed_path, row_number, f"placeholder alias_code {alias_code!r}")
    if alias_scheme is not None and _none_if_blank(raw["coalesce_priority"]) is None:
        _fail(seed_path, row_number, "blank coalesce_priority for alias row")

    vendor = _none_if_blank(raw["vendor"])
    vendor_field = _none_if_blank(raw["vendor_field"])
    if (vendor is None) != (vendor_field is None):
        _fail(seed_path, row_number, "partial vendor mapping requires both vendor and vendor_field")
    if vendor_field is not None and ("[" in vendor_field or "]" in vendor_field):
        _fail(seed_path, row_number, f"placeholder vendor_field {vendor_field!r}")
    if vendor_field is not None and _is_expression_vendor_field(vendor_field):
        _fail(seed_path, row_number, f"expression vendor_field {vendor_field!r}")

    _validate_date(raw["valid_from"], seed_path=seed_path, row_number=row_number, field_name="valid_from")
    _validate_date(raw["valid_to"], seed_path=seed_path, row_number=row_number, field_name="valid_to")


def _seed_row(raw: dict[str, str], *, seed_path: Path, row_number: int) -> FundamentalItemSeedRow:
    _validate_raw_row(raw, seed_path=seed_path, row_number=row_number)
    item_id = _parse_int(
        raw["item_id"],
        seed_path=seed_path,
        row_number=row_number,
        field_name="item_id",
    )
    if item_id is None:
        _fail(seed_path, row_number, "blank required field item_id")
    return FundamentalItemSeedRow(
        item_id=item_id,
        canonical_code=raw["canonical_code"].strip(),
        statement=_none_if_blank(raw["statement"]),
        section=_none_if_blank(raw["section"]),
        data_type=_none_if_blank(raw["data_type"]),
        unit_type=_none_if_blank(raw["unit_type"]),
        sign_convention=_none_if_blank(raw["sign_convention"]),
        is_derived=_parse_bool(raw["is_derived"], seed_path=seed_path, row_number=row_number),
        definition=_none_if_blank(raw["definition"]),
        citation=_none_if_blank(raw["citation"]),
        alias_scheme=_none_if_blank(raw["alias_scheme"]),
        alias_code=_none_if_blank(raw["alias_code"]),
        coalesce_priority=_parse_int(
            raw["coalesce_priority"],
            seed_path=seed_path,
            row_number=row_number,
            field_name="coalesce_priority",
        ),
        valid_from=_none_if_blank(raw["valid_from"]),
        valid_to=_none_if_blank(raw["valid_to"]),
        vendor=_none_if_blank(raw["vendor"]),
        vendor_field=_none_if_blank(raw["vendor_field"]),
        sign_note=_none_if_blank(raw["sign_note"]),
    )


def read_fundamental_item_seed(path: Path | str = SEED_PATH) -> tuple[FundamentalItemSeedRow, ...]:
    """Read the committed offline fundamental item seed with stdlib csv."""

    seed_path = Path(path)
    with seed_path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if tuple(reader.fieldnames or ()) != SEED_COLUMNS:
            raise ValueError(f"{seed_path} has unexpected columns: {reader.fieldnames}")
        return tuple(
            _seed_row(row, seed_path=seed_path, row_number=row_number)
            for row_number, row in enumerate(reader, start=2)
        )


def _dedupe_items(
    rows: tuple[FundamentalItemSeedRow, ...],
) -> dict[int, ItemRecord]:
    items: dict[int, ItemRecord] = {}
    for row in rows:
        item = (
            row.item_id,
            row.canonical_code,
            row.statement,
            row.section,
            row.data_type,
            row.unit_type,
            row.sign_convention,
            row.is_derived,
            row.definition,
            row.citation,
        )
        existing = items.get(row.item_id)
        if existing is not None and existing != item:
            raise ValueError(f"Conflicting fundamental_item seed rows for item_id={row.item_id}")
        items[row.item_id] = item
    return items


def seed_fundamental_item_registry(
    store: DuckDBStore,
    *,
    seed_path: Path | str = SEED_PATH,
) -> int:
    """Seed the canonical fundamental item registry from the committed CSV."""

    rows = read_fundamental_item_seed(seed_path)
    items = _dedupe_items(rows)
    aliases = {
        (
            row.item_id,
            row.alias_scheme,
            row.alias_code,
            row.coalesce_priority,
            row.valid_from,
            row.valid_to,
        )
        for row in rows
        if row.alias_scheme is not None and row.alias_code is not None
    }
    vendor_maps = {
        (row.item_id, row.vendor, row.vendor_field, row.sign_note)
        for row in rows
        if row.vendor is not None and row.vendor_field is not None
    }

    seed_item_ids = sorted(items)

    with store.transaction():
        store.con.execute(
            "DELETE FROM fundamental_item_alias WHERE item_id = ANY(?)",
            [seed_item_ids],
        )
        store.con.execute(
            "DELETE FROM fundamental_item_vendor_map WHERE item_id = ANY(?)",
            [seed_item_ids],
        )
        for item in sorted(items.values(), key=lambda values: values[0]):
            store.con.execute(
                """
                INSERT INTO fundamental_item (
                    item_id,
                    canonical_code,
                    statement,
                    section,
                    data_type,
                    unit_type,
                    sign_convention,
                    is_derived,
                    definition,
                    citation
                )
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT (item_id) DO UPDATE SET
                    canonical_code = excluded.canonical_code,
                    statement = excluded.statement,
                    section = excluded.section,
                    data_type = excluded.data_type,
                    unit_type = excluded.unit_type,
                    sign_convention = excluded.sign_convention,
                    is_derived = excluded.is_derived,
                    definition = excluded.definition,
                    citation = excluded.citation
                """,
                list(item),
            )

        for alias in sorted(aliases):
            store.con.execute(
                """
                INSERT INTO fundamental_item_alias (
                    item_id,
                    alias_scheme,
                    alias_code,
                    coalesce_priority,
                    valid_from,
                    valid_to
                )
                VALUES (?, ?, ?, ?, CAST(? AS DATE), CAST(? AS DATE))
                """,
                list(alias),
            )

        for vendor_map in sorted(vendor_maps):
            store.con.execute(
                """
                INSERT INTO fundamental_item_vendor_map (
                    item_id,
                    vendor,
                    vendor_field,
                    sign_note
                )
                VALUES (?, ?, ?, ?)
                """,
                list(vendor_map),
            )

    return len(items)
