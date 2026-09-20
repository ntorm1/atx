"""The declarative derived-metric catalog: seed I/O, validation, and ordering."""

from __future__ import annotations

import csv
import json
import re
from collections.abc import Iterable
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

from .connection import DuckDBStore
from .derived_dsl import (
    DAILY_FUNCTIONS,
    QUARTER_FUNCTIONS,
    BinOp,
    Call,
    DslError,
    Neg,
    Node,
    Ref,
    expression_names,
    parse_expression,
)
from .item_registry import read_fundamental_item_seed
from .warehouse import now_utc_naive

__all__ = [
    "DERIVED_SEED_COLUMNS",
    "DERIVED_SEED_PATH",
    "DERIVED_SOURCE_NAME",
    "MARKET_COLUMNS",
    "METRIC_WINDOWS",
    "QUARTER_GRID_WINDOWS",
    "RECLAIMED_ITEM_CODES",
    "DerivedMetricDefinition",
    "DerivedRegistryError",
    "default_derived_definitions",
    "derived_statement_item_codes",
    "known_item_codes",
    "read_derived_seed",
    "seed_derived_metric_definitions",
    "topological_order",
    "validate_definitions",
]

DERIVED_SEED_PATH = Path(__file__).resolve().parent / "seeds" / "derived_metric_definitions.csv"
DERIVED_SEED_COLUMNS = (
    "metric_code",
    "family",
    "expression",
    "window",
    "inputs",
    "requires_market",
    "description",
    "version",
)
METRIC_WINDOWS = frozenset({"q", "ttm", "annual", "instant", "avg2", "daily"})
QUARTER_GRID_WINDOWS = frozenset({"q", "ttm", "avg2", "instant"})
MARKET_COLUMNS = frozenset(
    {"close", "adj_close", "log_return", "volume", "archive_shares", "dei_shares", "shares_outstanding"}
)
_METRIC_CODE_RE = re.compile(r"^[a-z][a-z0-9_]{2,63}$")
_NAMESPACES = ("item:", "metric:", "market:")

#: Stable provenance tag for rows this engine writes, consumed by downstream
#: readers (Tasks 5/6/7) that need to attribute a value to this seed generation.
DERIVED_SOURCE_NAME = "atx-db declarative derived metrics v1"

#: Registry codes on the ``derived`` statement that the engine deliberately owns.
#: Every member must be in :func:`derived_statement_item_codes`; those rows have
#: no source alias and no source item, so they can never emit a standardized row.
RECLAIMED_ITEM_CODES = frozenset(
    {
        "market_cap",
        "enterprise_value",
        "ev_ebitda",
        "ev_sales",
        "roa",
        "roe",
        "roic",
        "net_margin",
        "ebitda_margin",
        "current_ratio",
        "interest_coverage",
        "dividend_yield",
        "sales_per_share",
        "cash_per_share",
        "tangible_book_value_per_share",
    }
)


class DerivedRegistryError(ValueError):
    """A malformed derived-metric catalog."""


@dataclass(frozen=True)
class DerivedMetricDefinition:
    metric_code: str
    family: str
    expression: str
    window: str
    inputs: tuple[str, ...]
    requires_market: bool
    description: str
    version: str

    def _bare(self, prefix: str) -> tuple[str, ...]:
        return tuple(value[len(prefix) :] for value in self.inputs if value.startswith(prefix))

    @property
    def item_inputs(self) -> tuple[str, ...]:
        return self._bare("item:")

    @property
    def metric_inputs(self) -> tuple[str, ...]:
        return self._bare("metric:")

    @property
    def market_inputs(self) -> tuple[str, ...]:
        return self._bare("market:")

    @property
    def bare_names(self) -> tuple[str, ...]:
        return tuple(sorted(self.item_inputs + self.metric_inputs + self.market_inputs))


def _fail(message: str) -> None:
    raise DerivedRegistryError(message)


def read_derived_seed(path: Path | str = DERIVED_SEED_PATH) -> tuple[DerivedMetricDefinition, ...]:
    seed_path = Path(path)
    definitions: list[DerivedMetricDefinition] = []
    with seed_path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if tuple(reader.fieldnames or ()) != DERIVED_SEED_COLUMNS:
            _fail(f"{seed_path} header must be {DERIVED_SEED_COLUMNS}, got {reader.fieldnames}")
        for row_number, raw in enumerate(reader, start=2):
            flag = (raw["requires_market"] or "").strip()
            if flag not in ("true", "false"):
                _fail(f"{seed_path}:{row_number} requires_market must be 'true' or 'false', got {flag!r}")
            inputs = tuple(part for part in (raw["inputs"] or "").split("|") if part)
            definitions.append(
                DerivedMetricDefinition(
                    metric_code=(raw["metric_code"] or "").strip(),
                    family=(raw["family"] or "").strip(),
                    expression=(raw["expression"] or "").strip(),
                    window=(raw["window"] or "").strip(),
                    inputs=inputs,
                    requires_market=flag == "true",
                    description=(raw["description"] or "").strip(),
                    version=(raw["version"] or "").strip(),
                )
            )
    return tuple(definitions)


@lru_cache(maxsize=1)
def default_derived_definitions() -> tuple[DerivedMetricDefinition, ...]:
    return read_derived_seed()


@lru_cache(maxsize=1)
def known_item_codes(seed_path: str | None = None) -> frozenset[str]:
    rows = read_fundamental_item_seed() if seed_path is None else read_fundamental_item_seed(seed_path)
    return frozenset(row.canonical_code for row in rows)


@lru_cache(maxsize=1)
def derived_statement_item_codes(seed_path: str | None = None) -> frozenset[str]:
    rows = read_fundamental_item_seed() if seed_path is None else read_fundamental_item_seed(seed_path)
    return frozenset(row.canonical_code for row in rows if row.statement == "derived")


def _window_calls(node: Node) -> tuple[Call, ...]:
    found: list[Call] = []

    def walk(current: Node) -> None:
        if isinstance(current, Call):
            if current.name in QUARTER_FUNCTIONS or current.name in DAILY_FUNCTIONS:
                found.append(current)
            for argument in current.args:
                walk(argument)
        elif isinstance(current, BinOp):
            walk(current.left)
            walk(current.right)
        elif isinstance(current, Neg):
            walk(current.operand)

    walk(node)
    return tuple(found)


def _direct_refs(node: Node) -> tuple[str, ...]:
    if isinstance(node, Ref):
        return (node.name,)
    if isinstance(node, Neg):
        return _direct_refs(node.operand)
    if isinstance(node, BinOp):
        return _direct_refs(node.left) + _direct_refs(node.right)
    if isinstance(node, Call):
        names: list[str] = []
        for argument in node.args:
            names.extend(_direct_refs(argument))
        return tuple(names)
    return ()


def validate_definitions(
    definitions: Iterable[DerivedMetricDefinition],
    *,
    item_codes: frozenset[str],
    reclaimable_codes: frozenset[str] | None = None,
) -> None:
    rows = tuple(definitions)
    reclaimable = RECLAIMED_ITEM_CODES if reclaimable_codes is None else reclaimable_codes
    by_code: dict[str, DerivedMetricDefinition] = {}
    for definition in rows:
        if not _METRIC_CODE_RE.fullmatch(definition.metric_code):
            _fail(f"metric_code {definition.metric_code!r} must match {_METRIC_CODE_RE.pattern}")
        if definition.metric_code in by_code:
            _fail(f"duplicate metric_code {definition.metric_code!r}")
        if definition.metric_code in item_codes and definition.metric_code not in reclaimable:
            _fail(
                f"metric_code {definition.metric_code!r} collides with a fundamental_items canonical_code; "
                "derived metric codes and standardized item codes share one namespace for consumers. "
                "Add it to RECLAIMED_ITEM_CODES only if it is a dead 'derived'-statement registry row."
            )
        by_code[definition.metric_code] = definition

    for definition in rows:
        where = f"metric {definition.metric_code!r}"
        if definition.window not in METRIC_WINDOWS:
            _fail(f"{where}: window {definition.window!r} not in {sorted(METRIC_WINDOWS)}")
        if definition.requires_market != (definition.window == "daily"):
            _fail(f"{where}: requires_market must be true if and only if window is 'daily'")
        for value in definition.inputs:
            if not value.startswith(_NAMESPACES):
                _fail(f"{where}: input {value!r} must start with one of {_NAMESPACES}")
        if definition.market_inputs and definition.window != "daily":
            _fail(f"{where}: market: inputs are only valid on the daily window")
        for column in definition.market_inputs:
            if column not in MARKET_COLUMNS:
                _fail(f"{where}: unknown market column {column!r}; expected one of {sorted(MARKET_COLUMNS)}")
        for code in definition.item_inputs:
            if code not in item_codes:
                _fail(f"{where}: item code {code!r} is not a canonical_code in seeds/fundamental_items.csv")
        for code in definition.metric_inputs:
            if code not in by_code:
                _fail(f"{where}: metric code {code!r} is not defined in the derived seed")
        if len(definition.bare_names) != len(set(definition.bare_names)):
            _fail(f"{where}: a bare name is declared in two namespaces")

        try:
            node = parse_expression(definition.expression)
        except DslError as error:
            raise DerivedRegistryError(f"{where}: {error}") from error
        used = set(expression_names(node))
        declared = set(definition.bare_names)
        for name in sorted(used - declared):
            _fail(f"{where}: expression references {name!r} which is not declared in inputs")
        for name in sorted(declared - used):
            _fail(f"{where}: input {name!r} is declared but never referenced by the expression")

        for call in _window_calls(node):
            if call.name in DAILY_FUNCTIONS and definition.window != "daily":
                _fail(f"{where}: daily function {call.name!r} requires window 'daily'")
            if call.name in QUARTER_FUNCTIONS:
                if definition.window not in QUARTER_GRID_WINDOWS:
                    _fail(
                        f"{where}: quarter function {call.name!r} requires a quarter-grid window "
                        f"({sorted(QUARTER_GRID_WINDOWS)}), got {definition.window!r}"
                    )
                declared_metric_inputs = set(definition.metric_inputs)
                for name in _direct_refs(call.args[0]):
                    if name not in declared_metric_inputs:
                        # Only a name declared via a ``metric:`` input is a dependency on
                        # another derived metric's window. A bare name that happens to
                        # collide with some other row's metric_code -- e.g. an
                        # ``item:`` input whose code equals a reclaimed-statement
                        # metric_code (see RECLAIMED_ITEM_CODES) -- is not that metric
                        # reference and must not be checked against its window.
                        continue
                    referenced = by_code.get(name)
                    if referenced is not None and referenced.window not in QUARTER_GRID_WINDOWS:
                        _fail(
                            f"{where}: {call.name!r} is applied to metric {name!r} whose window "
                            f"{referenced.window!r} is not on the quarterly period_end grid"
                        )


def topological_order(
    definitions: Iterable[DerivedMetricDefinition],
) -> tuple[DerivedMetricDefinition, ...]:
    rows = {definition.metric_code: definition for definition in definitions}
    pending = {code: set(row.metric_inputs) & set(rows) for code, row in rows.items()}
    ordered: list[DerivedMetricDefinition] = []
    ready = sorted(code for code, deps in pending.items() if not deps)
    while ready:
        code = ready.pop(0)
        ordered.append(rows[code])
        del pending[code]
        released: list[str] = []
        for other, deps in pending.items():
            if code in deps:
                deps.discard(code)
                if not deps:
                    released.append(other)
        for other in sorted(released):
            ready.append(other)
        ready.sort()
    if pending:
        _fail(f"derived metric dependency cycle among: {sorted(pending)}")
    return tuple(ordered)


def seed_derived_metric_definitions(
    store: DuckDBStore,
    definitions: Iterable[DerivedMetricDefinition] | None = None,
) -> int:
    rows = tuple(definitions) if definitions is not None else default_derived_definitions()
    validate_definitions(rows, item_codes=known_item_codes())
    ordered = topological_order(rows)
    rank_by_code = {definition.metric_code: index for index, definition in enumerate(ordered)}
    stamp = now_utc_naive()
    payload = [
        (
            definition.metric_code,
            definition.family,
            definition.expression,
            definition.window,
            json.dumps(list(definition.inputs), separators=(",", ":")),
            definition.requires_market,
            definition.description,
            definition.version,
            rank_by_code[definition.metric_code],
            stamp,
        )
        for definition in sorted(rows, key=lambda row: row.metric_code)
    ]
    with store.transaction():
        store.con.execute("DELETE FROM derived_metric_definitions")
        store.con.executemany(
            """
            INSERT INTO derived_metric_definitions (
                metric_code, family, expression, metric_window, inputs_json,
                requires_market, description, version, topological_rank, seeded_at
            ) VALUES (?,?,?,?,?,?,?,?,?,?)
            """,
            payload,
        )
    return len(payload)
