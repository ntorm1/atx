from __future__ import annotations

import csv
import re
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from datetime import date, datetime
from pathlib import Path
from typing import Any, NoReturn

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
class FundamentalItemAlias:
    item_id: int
    canonical_code: str
    alias_scheme: str
    alias_code: str
    coalesce_priority: int
    valid_from: date | None
    valid_to: date | None


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


@dataclass(frozen=True)
class RatioInputSpec:
    """Governed PF-S1 S1-3 bridge from ratio input key to source metric + item id."""

    key: str
    canonical_metric: str
    item_id: int | None


def _coerce_date(value: date | datetime | str | None, *, field_name: str) -> date | None:
    if value is None:
        return None
    if isinstance(value, datetime):
        return value.date()
    if isinstance(value, date):
        return value
    if not isinstance(value, str):
        raise ValueError(f"invalid {field_name} date {value!r}")
    value = _none_if_blank(value)
    if value is None:
        return None
    try:
        return date.fromisoformat(value)
    except ValueError as exc:
        raise ValueError(f"invalid {field_name} date {value!r}") from exc


def _alias_is_live(alias: FundamentalItemAlias, as_of_date: date | None) -> bool:
    if as_of_date is None:
        return True
    valid_from = alias.valid_from or date.min
    valid_to = alias.valid_to or date.max
    return valid_from <= as_of_date < valid_to


def _alias_key(alias: FundamentalItemAlias) -> tuple[int, str, str, int, date | None, date | None]:
    return (
        alias.item_id,
        alias.alias_scheme,
        alias.alias_code,
        alias.coalesce_priority,
        alias.valid_from,
        alias.valid_to,
    )


def _alias_windows_overlap(left: FundamentalItemAlias, right: FundamentalItemAlias) -> bool:
    left_from = left.valid_from or date.min
    left_to = left.valid_to or date.max
    right_from = right.valid_from or date.min
    right_to = right.valid_to or date.max
    return left_from < right_to and right_from < left_to


def _validate_aliases(aliases: tuple[FundamentalItemAlias, ...]) -> None:
    seen: set[tuple[int, str, str, int, date | None, date | None]] = set()
    by_lookup: dict[tuple[str, str], list[FundamentalItemAlias]] = {}
    for alias in aliases:
        key = _alias_key(alias)
        if key in seen:
            raise ValueError(
                "Duplicate fundamental_item_alias row "
                f"for item_id={alias.item_id}, alias_scheme={alias.alias_scheme!r}, "
                f"alias_code={alias.alias_code!r}"
            )
        seen.add(key)

        lookup_key = (alias.alias_scheme, alias.alias_code)
        for existing in by_lookup.get(lookup_key, ()):
            if existing.item_id != alias.item_id and _alias_windows_overlap(existing, alias):
                raise ValueError(
                    "Overlapping alias validity for "
                    f"alias_scheme={alias.alias_scheme!r}, alias_code={alias.alias_code!r}: "
                    f"item_id={existing.item_id} overlaps item_id={alias.item_id}"
                )
        by_lookup.setdefault(lookup_key, []).append(alias)


def _row_value(
    row: Mapping[str, object] | Sequence[object],
    field_name: str,
    index: int,
) -> Any:
    if isinstance(row, Mapping):
        return row[field_name]
    return row[index]


@dataclass(frozen=True)
class Registry:
    """Pure in-memory resolver for fundamental item aliases."""

    aliases: tuple[FundamentalItemAlias, ...]
    _by_alias: dict[tuple[str, str], tuple[FundamentalItemAlias, ...]] = field(
        init=False,
        repr=False,
    )
    _by_canonical: dict[str, tuple[FundamentalItemAlias, ...]] = field(
        init=False,
        repr=False,
    )

    def __post_init__(self) -> None:
        _validate_aliases(self.aliases)
        aliases = tuple(
            sorted(
                self.aliases,
                key=lambda alias: (
                    alias.coalesce_priority,
                    alias.alias_code,
                    alias.item_id,
                    alias.alias_scheme,
                    alias.valid_from or date.min,
                    alias.valid_to or date.max,
                ),
            )
        )
        object.__setattr__(self, "aliases", aliases)

        by_alias: dict[tuple[str, str], list[FundamentalItemAlias]] = {}
        by_canonical: dict[str, list[FundamentalItemAlias]] = {}
        for alias in aliases:
            by_alias.setdefault((alias.alias_scheme, alias.alias_code), []).append(alias)
            by_canonical.setdefault(alias.canonical_code, []).append(alias)

        object.__setattr__(
            self,
            "_by_alias",
            {key: tuple(value) for key, value in by_alias.items()},
        )
        object.__setattr__(
            self,
            "_by_canonical",
            {key: tuple(value) for key, value in by_canonical.items()},
        )

    @classmethod
    def from_seed_rows(cls, rows: Iterable[FundamentalItemSeedRow]) -> Registry:
        seed_rows = tuple(rows)
        item_by_id = _dedupe_items(seed_rows)
        aliases = tuple(
            FundamentalItemAlias(
                item_id=row.item_id,
                canonical_code=item_by_id[row.item_id][1],
                alias_scheme=row.alias_scheme,
                alias_code=row.alias_code,
                coalesce_priority=row.coalesce_priority,
                valid_from=_coerce_date(row.valid_from, field_name="valid_from"),
                valid_to=_coerce_date(row.valid_to, field_name="valid_to"),
            )
            for row in seed_rows
            if row.alias_scheme is not None
            and row.alias_code is not None
            and row.coalesce_priority is not None
        )
        return cls(aliases)

    @classmethod
    def from_table_rows(
        cls,
        item_rows: Iterable[Mapping[str, object] | Sequence[object]],
        alias_rows: Iterable[Mapping[str, object] | Sequence[object]],
    ) -> Registry:
        canonical_by_item_id: dict[int, str] = {}
        item_id_by_canonical: dict[str, int] = {}
        for row in item_rows:
            item_id = int(_row_value(row, "item_id", 0))
            canonical_code = str(_row_value(row, "canonical_code", 1))
            existing_code = canonical_by_item_id.get(item_id)
            if existing_code is not None:
                if existing_code == canonical_code:
                    raise ValueError(f"Duplicate fundamental_item row for item_id={item_id}")
                raise ValueError(
                    "Conflicting fundamental_item table rows "
                    f"for item_id={item_id}: {existing_code!r} != {canonical_code!r}"
                )
            existing_item_id = item_id_by_canonical.get(canonical_code)
            if existing_item_id is not None and existing_item_id != item_id:
                raise ValueError(
                    "Conflicting canonical_code "
                    f"{canonical_code!r} for item_id={existing_item_id} and item_id={item_id}"
                )
            canonical_by_item_id[item_id] = canonical_code
            item_id_by_canonical[canonical_code] = item_id

        aliases: list[FundamentalItemAlias] = []
        for row in alias_rows:
            item_id = int(_row_value(row, "item_id", 0))
            if item_id not in canonical_by_item_id:
                raise ValueError(f"fundamental_item_alias references unknown item_id={item_id}")
            aliases.append(
                FundamentalItemAlias(
                    item_id=item_id,
                    canonical_code=canonical_by_item_id[item_id],
                    alias_scheme=str(_row_value(row, "alias_scheme", 1)),
                    alias_code=str(_row_value(row, "alias_code", 2)),
                    coalesce_priority=int(_row_value(row, "coalesce_priority", 3)),
                    valid_from=_coerce_date(_row_value(row, "valid_from", 4), field_name="valid_from"),
                    valid_to=_coerce_date(_row_value(row, "valid_to", 5), field_name="valid_to"),
                )
            )
        return cls(tuple(aliases))

    def resolve_item(
        self,
        alias_scheme: str,
        alias_code: str,
        *,
        as_of: date | datetime | str | None = None,
    ) -> int | None:
        as_of_date = _coerce_date(as_of, field_name="as_of")
        for alias in self._by_alias.get((alias_scheme, alias_code), ()):
            if _alias_is_live(alias, as_of_date):
                return alias.item_id
        return None

    def resolve_inputs(
        self,
        canonical_code: str,
        *,
        as_of: date | datetime | str | None = None,
    ) -> list[str]:
        as_of_date = _coerce_date(as_of, field_name="as_of")
        return [
            alias.alias_code
            for alias in self._by_canonical.get(canonical_code, ())
            if _alias_is_live(alias, as_of_date)
        ]


def _none_if_blank(value: str | None) -> str | None:
    if value is None:
        return None
    value = value.strip()
    return value or None


def _fail(seed_path: Path, row_number: int, message: str) -> NoReturn:
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
        _fail(seed_path, row_number, f"extra CSV fields {raw.get(None)!r}")  # type: ignore[call-overload]

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
    item_id_by_canonical: dict[str, int] = {}
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
        existing_item_id = item_id_by_canonical.get(row.canonical_code)
        if existing_item_id is not None and existing_item_id != row.item_id:
            raise ValueError(
                "Conflicting canonical_code "
                f"{row.canonical_code!r} for item_id={existing_item_id} and item_id={row.item_id}"
            )
        items[row.item_id] = item
        item_id_by_canonical[row.canonical_code] = row.item_id
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

    import pyarrow as pa  # type: ignore[import-untyped]

    item_columns = (
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
    )
    alias_columns = (
        "item_id",
        "alias_scheme",
        "alias_code",
        "coalesce_priority",
        "valid_from",
        "valid_to",
    )
    vendor_columns = ("item_id", "vendor", "vendor_field", "sign_note")

    def _arrow_table(
        records: Iterable[Sequence[object]],
        columns: Sequence[str],
    ) -> Any:
        if not records:
            return pa.table({column: [] for column in columns})
        return pa.Table.from_pylist([dict(zip(columns, record, strict=True)) for record in records])

    item_records = sorted(items.values(), key=lambda values: values[0])
    item_frame = _arrow_table(item_records, item_columns)
    alias_frame = _arrow_table(sorted(aliases), alias_columns)
    vendor_frame = _arrow_table(sorted(vendor_maps), vendor_columns)

    with store.transaction():
        store.con.register("_fundamental_item_seed", item_frame)
        store.con.register("_fundamental_item_alias_seed", alias_frame)
        store.con.register("_fundamental_item_vendor_seed", vendor_frame)
        try:
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
                SELECT
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
                FROM _fundamental_item_seed
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
                """
            )

            store.con.execute(
                """
                UPDATE fundamental_item_alias existing
                SET coalesce_priority=seed.coalesce_priority,
                    valid_to=CAST(seed.valid_to AS DATE)
                FROM _fundamental_item_alias_seed seed
                WHERE existing.item_id IS NOT DISTINCT FROM seed.item_id
                  AND existing.alias_scheme IS NOT DISTINCT FROM seed.alias_scheme
                  AND existing.alias_code IS NOT DISTINCT FROM seed.alias_code
                  AND existing.valid_from IS NOT DISTINCT FROM CAST(seed.valid_from AS DATE)
                  AND (
                      existing.coalesce_priority IS DISTINCT FROM seed.coalesce_priority
                      OR existing.valid_to IS DISTINCT FROM CAST(seed.valid_to AS DATE)
                  )
                """
            )
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
                SELECT
                    seed.item_id,
                    seed.alias_scheme,
                    seed.alias_code,
                    seed.coalesce_priority,
                    CAST(seed.valid_from AS DATE),
                    CAST(seed.valid_to AS DATE)
                FROM _fundamental_item_alias_seed seed
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM fundamental_item_alias existing
                    WHERE existing.item_id IS NOT DISTINCT FROM seed.item_id
                      AND existing.alias_scheme IS NOT DISTINCT FROM seed.alias_scheme
                      AND existing.alias_code IS NOT DISTINCT FROM seed.alias_code
                      AND existing.coalesce_priority IS NOT DISTINCT FROM seed.coalesce_priority
                      AND existing.valid_from IS NOT DISTINCT FROM CAST(seed.valid_from AS DATE)
                      AND existing.valid_to IS NOT DISTINCT FROM CAST(seed.valid_to AS DATE)
                )
                """
            )

            store.con.execute(
                """
                UPDATE fundamental_item_vendor_map existing
                SET sign_note=seed.sign_note
                FROM _fundamental_item_vendor_seed seed
                WHERE existing.item_id IS NOT DISTINCT FROM seed.item_id
                  AND existing.vendor IS NOT DISTINCT FROM seed.vendor
                  AND existing.vendor_field IS NOT DISTINCT FROM seed.vendor_field
                  AND existing.sign_note IS DISTINCT FROM seed.sign_note
                """
            )
            store.con.execute(
                """
                INSERT INTO fundamental_item_vendor_map (
                    item_id,
                    vendor,
                    vendor_field,
                    sign_note
                )
                SELECT seed.item_id,seed.vendor,seed.vendor_field,seed.sign_note
                FROM _fundamental_item_vendor_seed seed
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM fundamental_item_vendor_map existing
                    WHERE existing.item_id IS NOT DISTINCT FROM seed.item_id
                      AND existing.vendor IS NOT DISTINCT FROM seed.vendor
                      AND existing.vendor_field IS NOT DISTINCT FROM seed.vendor_field
                )
                """
            )
        finally:
            store.con.unregister("_fundamental_item_seed")
            store.con.unregister("_fundamental_item_alias_seed")
            store.con.unregister("_fundamental_item_vendor_seed")

    return len(items)


_DEFAULT_REGISTRY: Registry | None = None


def default_registry(*, seed_path: Path | str = SEED_PATH) -> Registry:
    """Build or return the process-local registry cache from the committed seed."""

    global _DEFAULT_REGISTRY
    if _DEFAULT_REGISTRY is None or Path(seed_path) != SEED_PATH:
        registry = Registry.from_seed_rows(read_fundamental_item_seed(seed_path))
        if Path(seed_path) != SEED_PATH:
            return registry
        _DEFAULT_REGISTRY = registry
    return _DEFAULT_REGISTRY


def resolve_item(
    alias_scheme: str,
    alias_code: str,
    *,
    as_of: date | datetime | str | None = None,
    registry: Registry | None = None,
) -> int | None:
    """Resolve a taxonomy alias to a canonical item id."""

    active_registry = registry or default_registry()
    return active_registry.resolve_item(alias_scheme, alias_code, as_of=as_of)


def resolve_inputs(
    canonical_code: str,
    *,
    as_of: date | datetime | str | None = None,
    registry: Registry | None = None,
) -> list[str]:
    """Return alias codes for a canonical item in deterministic COALESCE order."""

    active_registry = registry or default_registry()
    return active_registry.resolve_inputs(canonical_code, as_of=as_of)


# PF-S1 S1-3 ratio-input authority.
#
# These specs deliberately preserve the exact canonical_metric strings consumed by
# fundamental_ratios before S1-3. The item_id column is additive provenance only:
# it records the controller-approved bridge into fundamental_item without changing
# the pivot strings, ratio values, ratio_id inputs, or available_at computation.
# Do not infer these ids by string-joining to fundamental_item.canonical_code: the
# vocabularies differ and `net_income` is an estimates false friend (item 2009).
_TTM_RATIO_INPUT_SPECS = (
    RatioInputSpec("rev", "revenue", 1001),
    RatioInputSpec("ni", "net_income", 1031),
    RatioInputSpec("oi", "operating_income", 1014),
    RatioInputSpec("ocf", "operating_cash_flow", 1301),
    RatioInputSpec("capex", "capital_expenditures", 1305),
    RatioInputSpec("div", "dividends_paid", 1318),
    RatioInputSpec("repurch", "share_repurchases", 1312),
)

_BALANCE_RATIO_INPUT_SPECS = (
    # The pre-S1-3 literals were `assets` / `liabilities`, even though the
    # statement map's canonical metrics are total_assets / total_liabilities.
    # Keep the old strings for byte identity; attach the agreed item ids only.
    RatioInputSpec("assets", "assets", 1101),
    RatioInputSpec("liabilities", "liabilities", 1201),
    RatioInputSpec("equity", "stockholders_equity", 1221),
    RatioInputSpec("shares", "shares_outstanding", 1039),
)

_XBRL_BALANCE_RATIO_INPUT_SPECS = (
    RatioInputSpec("current_assets", "current_assets", 1102),
    RatioInputSpec("current_liabilities", "current_liabilities", 1202),
    RatioInputSpec("cash_and_equivalents", "cash_and_equivalents", 1104),
    RatioInputSpec("inventory", "inventory", 1107),
    RatioInputSpec("long_term_debt", "long_term_debt", 1207),
    RatioInputSpec("retained_earnings", "retained_earnings", 1217),
    # Controller decision: leave this XBRL-only ratio input unmapped in S1-3.
    RatioInputSpec("common_shares_outstanding", "common_shares_outstanding", None),
    RatioInputSpec("property_plant_equipment_net", "property_plant_equipment_net", 1110),
    RatioInputSpec("accounts_receivable", "accounts_receivable", 1106),
    # PF-S4 S4-2: accounts payable, wired the same way as the other XBRL-balance
    # inputs above -- `ap` is a genuine companyfacts canonical_metric (concept_map.csv
    # AccountsPayableCurrent -> ap, item_id 1203), needed for days-payables-outstanding
    # / the cash-conversion cycle.
    RatioInputSpec("accounts_payable", "ap", 1203),
    # PF-S4 S4-2: goodwill and other (non-goodwill) intangibles, needed for
    # tangible_book_value_per_share (concept_map.csv Goodwill -> goodwill item 1114,
    # IntangibleAssetsNetExcludingGoodwill -> intangibles_other item 1115).
    RatioInputSpec("goodwill", "goodwill", 1114),
    RatioInputSpec("intangibles_other", "intangibles_other", 1115),
)

_XBRL_FLOW_RATIO_INPUT_SPECS = (
    RatioInputSpec("gross_profit", "gross_profit", 1004),
    RatioInputSpec("cost_of_revenue", "cost_of_revenue", 1003),
    RatioInputSpec("interest_expense", "interest_expense", 1018),
    RatioInputSpec("depreciation_amortization", "depreciation_amortization", 1307),
    # Controller decision: do not fabricate a combined-SG&A linkage in S1-3.
    RatioInputSpec(
        "selling_general_and_administrative_expense",
        "selling_general_and_administrative_expense",
        None,
    ),
    # PF-S4 S4-2: pretax income and income tax expense, wired the same way as the
    # other XBRL-flow inputs above (concept_map.csv IncomeLossFromContinuing...
    # BeforeIncomeTaxes... -> pretax_income item 1023; IncomeTaxExpenseBenefit ->
    # income_tax item 1024). Needed for the 5-way extended DuPont tax/interest
    # burden terms and the cash-interest-coverage ratio.
    RatioInputSpec("pretax_income", "pretax_income", 1023),
    RatioInputSpec("income_tax", "income_tax", 1024),
    # PF-S4 S4-2: weighted-average basic/diluted share counts (a DURATION concept,
    # distinct from the period-end `shares`/`common_shares_outstanding` balance
    # inputs above), needed for eps_basic/eps_diluted (concept_map.csv
    # WeightedAverageNumberOfSharesOutstandingBasic -> shares_basic_avg item 1040,
    # WeightedAverageNumberOfDilutedSharesOutstanding -> shares_diluted_avg item 1041).
    RatioInputSpec("shares_basic_avg", "shares_basic_avg", 1040),
    RatioInputSpec("shares_diluted_avg", "shares_diluted_avg", 1041),
)

_RATIO_INPUT_SPECS_BY_GROUP = {
    "ttm": _TTM_RATIO_INPUT_SPECS,
    "balance": _BALANCE_RATIO_INPUT_SPECS,
    "xbrl_balance": _XBRL_BALANCE_RATIO_INPUT_SPECS,
    "xbrl_flow": _XBRL_FLOW_RATIO_INPUT_SPECS,
}

_RATIO_INPUT_SPEC_BY_KEY = {
    spec.key: spec
    for specs in _RATIO_INPUT_SPECS_BY_GROUP.values()
    for spec in specs
}


def _ratio_specs(group: str) -> tuple[RatioInputSpec, ...]:
    try:
        return _RATIO_INPUT_SPECS_BY_GROUP[group]
    except KeyError as exc:
        groups = ", ".join(sorted(_RATIO_INPUT_SPECS_BY_GROUP))
        raise ValueError(f"unknown ratio input group {group!r}; expected one of: {groups}") from exc


def ratio_input_metrics(group: str) -> dict[str, str]:
    """Return ratio input key -> exact source-table canonical_metric for a group."""

    return {spec.key: spec.canonical_metric for spec in _ratio_specs(group)}


def ratio_input_item_ids(group: str) -> dict[str, int | None]:
    """Return ratio input key -> governed fundamental_item item_id for a group."""

    return {spec.key: spec.item_id for spec in _ratio_specs(group)}


def _ratio_input_base_key(input_key: str) -> str:
    return input_key[:-6] if input_key.endswith("_prior") else input_key


def input_item_ids_for_ratio(inputs: Iterable[str]) -> list[int]:
    """Resolve a RatioDef.inputs tuple to sorted distinct governed item ids.

    Missing inputs and documented S1-3 gaps are omitted. This makes the additive
    fundamental_ratios.input_item_ids_json honest metadata while leaving
    input_codes_json as the exact raw dependency list.
    """

    item_ids = {
        spec.item_id
        for input_key in inputs
        if (spec := _RATIO_INPUT_SPEC_BY_KEY.get(_ratio_input_base_key(str(input_key)))) is not None
        and spec.item_id is not None
    }
    return sorted(item_ids)
