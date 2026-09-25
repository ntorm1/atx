"""Private fiscal-year evidence and metadata for the canonical PIT evaluator.

Only a top-level ttm(scalar) receives a direct FY alternative. Scalar metric
references are expanded from the existing registry, never published as annual
values under their quarterly codes. The arithmetic DSL remains unchanged.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, replace
from functools import lru_cache
from typing import Any

from . import _split_epochs
from .derived_dsl import (
    SCALAR_FUNCTIONS,
    BinOp,
    Call,
    LowerContext,
    Neg,
    Node,
    Number,
    Ref,
    expression_names,
    lower,
    parse_expression,
)
from .derived_registry import DerivedMetricDefinition, default_derived_definitions
from .item_registry import read_fundamental_item_seed

WEIGHTED_SHARES = frozenset({"weighted_avg_shares_basic", "weighted_avg_shares_diluted"})

#: Share-count items. ``unit_type='quantity'`` also covers non-share counts
#: (leasable area, subscribers), so the share counts are named explicitly.
SHARE_COUNT_CODES = WEIGHTED_SHARES | frozenset({
    "shares_outstanding_period_end", "treasury_stock_shares", "entity_public_float_shares",
    "class_a_common_shares_outstanding", "class_b_common_shares_outstanding",
    "class_c_common_shares_outstanding", "class_d_common_shares_outstanding",
})

#: Exponent of "shares" in a value's unit: -1 per share, +1 a share count, 0
#: basis-free, None mixed or unknown (treated as split-sensitive).
ShareExponent = int | None

_BASIS_FREE_CALLS = frozenset({"yoy", "qoq", "cagr", "tret", "rvol", "indicator_gt", "indicator_lt"})
_SAME_UNIT_CALLS = frozenset({"coalesce", "min", "max"})


@dataclass(frozen=True)
class AnnualPlan:
    alternative: Node | None = None
    item_codes: tuple[str, ...] = ()
    weighted_codes: tuple[str, ...] = ()
    #: Share exponent of each operand under the catalog being refreshed. Not in
    #: the definition hash; None falls back to the shipped catalog.
    share_exponents: tuple[tuple[str, ShareExponent], ...] | None = None

    @property
    def codes(self) -> tuple[str, ...]:
        return tuple(sorted(set(self.item_codes) | set(self.weighted_codes)))


EMPTY_PLAN = AnnualPlan()


@lru_cache(maxsize=1)
def _item_share_exponents() -> dict[str, int]:
    exponents = {row.canonical_code: -1 for row in read_fundamental_item_seed() if row.unit_type == "per_share"}
    exponents.update(dict.fromkeys(SHARE_COUNT_CODES, 1))
    return exponents


def share_exponent(node: Node, exponent_of: Any) -> ShareExponent:
    """Unit algebra over a formula: which values move with a stock split.

    ``exponent_of(name)`` resolves an operand. Products add exponents and
    quotients subtract them (per share x shares is basis-free), sums and
    selections need one unit, windows keep their operand's unit, and relative
    changes and indicators are basis-free. The split sensitivity of each
    comparison inside the formula is proven separately by ``_combine``.
    """
    def same(nodes: tuple[Node, ...]) -> ShareExponent:
        units = {share_exponent(item, exponent_of) for item in nodes if not isinstance(item, Number)}
        return units.pop() if len(units) == 1 else (0 if not units else None)

    def ratio(numerator: Node, denominator: Node, op: str) -> ShareExponent:
        left, right = share_exponent(numerator, exponent_of), share_exponent(denominator, exponent_of)
        if left is None or right is None:
            return None
        return left + right if op == "*" else left - right

    if isinstance(node, Number):
        return 0
    if isinstance(node, Ref):
        return exponent_of(node.name)
    if isinstance(node, Neg):
        return share_exponent(node.operand, exponent_of)
    if isinstance(node, BinOp):
        return same((node.left, node.right)) if node.op in "+-" else ratio(node.left, node.right, node.op)
    if isinstance(node, Call):
        if node.name in _BASIS_FREE_CALLS:
            return 0
        if node.name == "safe_div":
            return ratio(node.args[0], node.args[1], "/")
        if node.name in _SAME_UNIT_CALLS:
            return same(node.args)
        inner = share_exponent(node.args[0], exponent_of)
        if node.name == "ln":
            return 0 if inner == 0 else None
        return inner  # abs, ttm, avg2, lag, lag_d, stdev_q, avg_d keep the operand's unit
    raise TypeError(node)


def _metric_share_exponents(definitions: Mapping[str, DerivedMetricDefinition]) -> dict[str, ShareExponent]:
    """Every metric's share exponent derived from its formula operands.

    Family ``per_share`` is a floor: such a metric is never basis-free.
    """
    items = _item_share_exponents()
    memo: dict[str, ShareExponent] = {}

    def metric(code: str) -> ShareExponent:
        if code not in memo:
            definition = definitions[code]
            memo[code] = None  # the validated catalog is acyclic; a cycle stays split-sensitive
            value = share_exponent(
                parse_expression(definition.expression),
                lambda name: metric(name) if name in definition.metric_inputs and name in definitions
                else items.get(name, 0))
            memo[code] = None if definition.family == "per_share" and value == 0 else value
        return memo[code]

    for code in definitions:
        metric(code)
    return memo


_CATALOG_EXPONENTS: dict[int, tuple[Mapping[str, DerivedMetricDefinition], dict[str, ShareExponent]]] = {}
_CATALOG_SLOTS = 8


def _catalog_share_exponents(definitions: Mapping[str, DerivedMetricDefinition]) -> dict[str, ShareExponent]:
    # plan_for runs per security and metric with a few long-lived catalog objects
    # (refresh, research, lineage): cache by identity. Each slot keeps its catalog
    # alive, so an identity cannot be reused while it is cached.
    cached = _CATALOG_EXPONENTS.get(id(definitions))
    if cached is None or cached[0] is not definitions:
        if len(_CATALOG_EXPONENTS) >= _CATALOG_SLOTS:
            _CATALOG_EXPONENTS.pop(next(iter(_CATALOG_EXPONENTS)))
        cached = (definitions, _metric_share_exponents(definitions))
        _CATALOG_EXPONENTS[id(definitions)] = cached
    return cached[1]


def item_share_exponent(code: str) -> int:
    return _item_share_exponents().get(code, 0)


def plan_share_exponents(plan: AnnualPlan) -> Mapping[str, ShareExponent]:
    """Operand units for frame SQL: the plan's, else the shipped catalog's."""
    return _default_ref_exponents() if plan.share_exponents is None else dict(plan.share_exponents)


@lru_cache(maxsize=1)
def _default_ref_exponents() -> dict[str, ShareExponent]:
    metrics = _metric_share_exponents({d.metric_code: d for d in default_derived_definitions()})
    return {**_item_share_exponents(), **metrics}


def share_basis_codes() -> frozenset[str]:
    """Items and shipped metrics whose values move with a split (per share or share counts)."""
    return frozenset(code for code, exponent in _default_ref_exponents().items() if exponent != 0)


def plan_for(definition: DerivedMetricDefinition,
             definitions: dict[str, DerivedMetricDefinition]) -> AnnualPlan:
    """Discover eligible formulas, without a second public metric catalog."""
    def expand(node: Node, owner: DerivedMetricDefinition, seen: frozenset[str]) -> Node | None:
        if isinstance(node, Ref) and node.name in owner.metric_inputs:
            if node.name in seen:
                return None
            dependency = definitions[node.name]
            return expand(parse_expression(dependency.expression), dependency, seen | {node.name})
        if isinstance(node, (Number, Ref)):
            return node
        if isinstance(node, Neg):
            child = expand(node.operand, owner, seen)
            return Neg(child) if child is not None else None
        if isinstance(node, BinOp):
            left, right = expand(node.left, owner, seen), expand(node.right, owner, seen)
            return BinOp(node.op, left, right) if left is not None and right is not None else None
        if isinstance(node, Call) and node.name in SCALAR_FUNCTIONS:
            args = tuple(expand(arg, owner, seen) for arg in node.args)
            return Call(node.name, tuple(arg for arg in args if arg is not None)) \
                if all(arg is not None for arg in args) else None
        return None

    node = parse_expression(definition.expression)
    alternative = None
    if definition.window == "ttm" and isinstance(node, Call) and node.name == "ttm":
        alternative = expand(node.args[0], definition, frozenset({definition.metric_code}))
        # Constant-only formulas have no actual annual source evidence.
        if alternative is not None and not expression_names(alternative):
            alternative = None
    weighted = tuple(sorted(WEIGHTED_SHARES.intersection(definition.item_inputs))) \
        if definition.window == "ttm" else ()
    metrics, items = _catalog_share_exponents(definitions), _item_share_exponents()
    exponents = tuple((name, metrics.get(name, 0) if name in definition.metric_inputs else items.get(name, 0))
                      for name in definition.bare_names)
    return AnnualPlan(alternative, expression_names(alternative) if alternative is not None else (), weighted,
                      exponents)


def prepare_security(con: Any) -> None:
    """All revisions, exact spans, deterministic whole-state selection."""
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _pit_annual_items AS
        WITH events AS (
            SELECT security_id, canonical_code AS code, bucket, period_start, period_end,
                   available_at AS event_at,
                   arg_max(struct_pack(value := value, available_at := available_at,
                       period_start := period_start, period_end := period_end,
                       state_id := standardized_id, source := source, rule_id := rule_id,
                       basis := basis, cik := cik), (available_at, source, rule_id, standardized_id))
                   OVER (PARTITION BY security_id, canonical_code, period_start, period_end
                         ORDER BY available_at RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS state
            FROM _pit_raw WHERE basis = 'annual'
            QUALIFY row_number() OVER (
                PARTITION BY security_id, canonical_code, period_start, period_end, available_at
                ORDER BY source DESC, rule_id DESC, standardized_id DESC) = 1
        )
        SELECT * FROM events
        QUALIFY lag(state) OVER (
            PARTITION BY security_id, code, period_start, period_end ORDER BY event_at)
            IS DISTINCT FROM state
    """)
    # The same visible fiscal span governs every annual input at an endpoint.
    # A later alternative span is a selection event, not a license to mix spans.
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _pit_annual_targets AS
        WITH events AS (
            SELECT security_id, bucket, available_at AS event_at,
                   arg_max(struct_pack(period_start := period_start, period_end := period_end),
                       (period_end, available_at, source, rule_id, standardized_id))
                   OVER (PARTITION BY security_id, bucket ORDER BY available_at
                         RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS span
            FROM _pit_raw WHERE basis = 'annual'
            QUALIFY row_number() OVER (PARTITION BY security_id, bucket, available_at
                ORDER BY period_end DESC, source DESC, rule_id DESC, standardized_id DESC) = 1
        )
        SELECT * FROM events
        QUALIFY lag(span) OVER (PARTITION BY security_id, bucket ORDER BY event_at) IS DISTINCT FROM span
    """)


@dataclass(frozen=True)
class Span:
    """SQL provenance of the selected scalar operands, independent of arithmetic."""

    start: str = "NULL::DATE"
    end: str = "NULL::DATE"
    annual: str = "false"
    coherent: str = "true"
    offset: int | None = None
    #: The operand's value is on a share basis (per share or a share count), so a
    #: split between two quarters changes it. Static, not SQL.
    share_basis: bool = False
    #: SQL availability clock of a share-basis operand (its filing, hence its
    #: split basis); NULL when operands of different clocks were combined.
    clock: str = "NULL::TIMESTAMP"
    #: SQL: every split between the operand's clock and the frame's clock would
    #: have been seen, so rebasing restated it exactly (``_split_epochs``).
    basis: str = "true"


def _case(condition: str, yes: str, no: str) -> str:
    return f"(CASE WHEN {condition} THEN {yes} ELSE {no} END)"


def _years_apart(current: str, prior: str, years: int) -> str:
    return f"(date_diff('day', {prior}, {current}) BETWEEN {330 * years} AND {380 * years})"


#: Largest gap, in days, between a prior quarter's end and the next quarter's start.
ADJACENT_QUARTER_MAX_GAP_DAYS = 7


def _quarter_length(span: Span) -> str:
    return f"(date_diff('day', {span.start}, {span.end}) + 1 BETWEEN 70 AND 120)"


def _one_quarter_apart(newer: Span, older: Span) -> str:
    """Prove that two operands one bucket apart are consecutive fiscal quarters.

    The proof uses only the operands' own fiscal dates (a NULL start marks an
    instant; incoherent flows with a missing start are rejected by their own
    ``coherent`` term):

    - flow after flow: both spans 70-120 days and the newer starts 1 to
      1 + ADJACENT_QUARTER_MAX_GAP_DAYS days after the older ends;
    - flow after a lagged opening balance (``NI_q / lag(equity, 1)``): the flow
      spans 70-120 days and the balance is dated on the day before the flow
      starts or at most ADJACENT_QUARTER_MAX_GAP_DAYS days earlier, never inside
      the flow period;
    - instant after instant: 70-120 days apart. The pair then spans the quarter
      between the balances (see ``_combine``), so an averaged balance meets a
      flow only when the opening balance is dated exactly the day before the
      flow starts.

    An instant after a flow, an overlap, a stub, a fiscal-year-change gap or a
    missing date proves nothing and stays incomparable.
    """
    gap = ADJACENT_QUARTER_MAX_GAP_DAYS
    flow_after_flow = (f"{_quarter_length(newer)} AND {_quarter_length(older)} AND "
                       f"date_diff('day', {older.end}, {newer.start}) BETWEEN 1 AND {1 + gap}")
    flow_after_instant = (f"{_quarter_length(newer)} AND "
                          f"date_diff('day', {older.end}, {newer.start}) BETWEEN 1 AND {1 + gap}")
    instant_after_instant = f"date_diff('day', {older.end}, {newer.end}) BETWEEN 70 AND 120"
    return (f"coalesce(CASE WHEN ({newer.start}) IS NOT NULL AND ({older.start}) IS NOT NULL "
            f"THEN {flow_after_flow} WHEN ({newer.start}) IS NOT NULL THEN {flow_after_instant} "
            f"WHEN ({older.start}) IS NULL THEN {instant_after_instant} ELSE false END, false)")


def _window_basis(inner: Span, frame: str, size: int) -> tuple[str, str, str]:
    """(proof, clock, basis) for a share-basis operand over a ``size``-row frame.

    The window is on one split basis when all its states were filed at one clock,
    or when every state's basis is known and was rebased to the frame's clock.
    """
    same_clock = (f"count({inner.clock}) OVER ({frame}) = {size} "
                  f"AND min({inner.clock}) OVER ({frame}) = max({inner.clock}) OVER ({frame})")
    basis = f"bool_and({inner.basis}) OVER ({frame})"
    return (f"coalesce(({same_clock}) OR ({basis}), false)",
            f"(CASE WHEN {same_clock} THEN max({inner.clock}) OVER ({frame}) END)", basis)


def _consecutive_window(inner: Span, frame: str, size: int) -> str:
    """Prove a ``size``-bucket rolling window is one chain of consecutive quarters.

    Every one of the ``size`` states must be present (end dates), coherent and of
    one kind (all flows or all instants), and every consecutive pair must pass
    the one-bucket proof of :func:`_one_quarter_apart`. A share-basis window
    must also sit on one split basis (:func:`_window_basis`).
    """
    states = f"list(struct_pack(s := {inner.start}, e := {inner.end})) OVER ({frame})"
    pair = _one_quarter_apart(Span("q_pair[2].s", "q_pair[2].e"), Span("q_pair[1].s", "q_pair[1].e"))
    split = f" AND {_window_basis(inner, frame, size)[0]}" if inner.share_basis else ""
    return (f"coalesce(count({inner.end}) OVER ({frame}) = {size} "
            f"AND count({inner.start}) OVER ({frame}) IN (0, {size}) "
            f"AND bool_and({inner.coherent}) OVER ({frame}) "
            f"AND list_bool_and(list_transform(list_zip(({states})[1:-2], ({states})[2:]), "
            f"lambda q_pair: {pair})){split}, false)")


def _combine(left: Span, right: Span) -> Span:
    if left.offset is None:
        return right
    if right.offset is None:
        return left
    newer, older = (left, right) if left.offset <= right.offset else (right, left)
    distance = abs(left.offset - right.offset)
    start = newer.start
    if distance == 0:
        comparable = (
            f"(({left.end}) IS NULL OR ({right.end}) IS NULL OR ({left.end}) = ({right.end})) AND "
            f"(({left.start}) IS NULL OR ({right.start}) IS NULL OR ({left.start}) = ({right.start}))"
        )
        start = f"coalesce({newer.start}, {older.start})"
    elif distance == 1:
        comparable = _one_quarter_apart(newer, older)
        # Two balances one quarter apart span the quarter between them.
        start = (f"(CASE WHEN ({newer.start}) IS NULL AND ({older.start}) IS NULL "
                 f"THEN CAST(({older.end}) + INTERVAL 1 DAY AS DATE) ELSE {newer.start} END)")
    elif distance % 4:
        comparable = "false"
    else:
        comparable = (
            f"{_years_apart(newer.end, older.end, distance // 4)} AND "
            f"(({newer.start}) IS NULL OR ({older.start}) IS NULL OR "
            f"{_years_apart(newer.start, older.start, distance // 4)})"
        )
    if distance and (newer.share_basis or older.share_basis):
        # A filing states every value on the split basis at its own clock. Values
        # from one filing share it; otherwise both must have been rebased with
        # every split between their clocks and the frame's clock (_split_epochs).
        comparable = (f"({comparable}) AND (({newer.clock}) = ({older.clock}) "
                      f"OR (({newer.basis}) AND ({older.basis})))")
    return Span(
        start,
        f"coalesce({newer.end}, {older.end})" if distance == 0 else newer.end,
        f"(({left.annual}) OR ({right.annual}))",
        f"(({left.coherent}) AND ({right.coherent}) AND ({comparable}))",
        newer.offset,
        left.share_basis or right.share_basis,
        f"(CASE WHEN ({left.clock}) = ({right.clock}) THEN ({left.clock}) END)",
        f"(({left.basis}) AND ({right.basis}))",
    )


def lower_span(node: Node, context: LowerContext, refs: dict[str, Span],
               share_exponents: Mapping[str, ShareExponent] | None = None) -> Span:
    """Follow DSL-selected branches and window offsets without changing formulas.

    Coherence is recorded for all states but only gates arithmetic when annual
    evidence participates. This leaves historical quarterly-only rules intact.
    ``share_exponents`` gives each operand's unit under the refreshed catalog
    (``AnnualPlan.share_exponents``); the shipped catalog is the fallback.
    """
    exponents = _default_ref_exponents() if share_exponents is None else share_exponents
    span = _lower_span(node, context, refs, exponents)
    split_sensitive = share_exponent(node, lambda name: exponents.get(name, 0)) != 0
    return span if span.share_basis == split_sensitive else replace(span, share_basis=split_sensitive)


def _lower_span(node: Node, context: LowerContext, refs: dict[str, Span],
                exponents: Mapping[str, ShareExponent]) -> Span:
    if isinstance(node, Number):
        return Span()
    if isinstance(node, Ref):
        return refs[node.name]
    if isinstance(node, Neg):
        return lower_span(node.operand, context, refs, exponents)
    if isinstance(node, BinOp):
        return _combine(lower_span(node.left, context, refs, exponents),
                        lower_span(node.right, context, refs, exponents))
    if not isinstance(node, Call):
        raise TypeError(node)
    children = [lower_span(arg, context, refs, exponents) for arg in node.args]
    inner = children[0]
    if node.name == "coalesce":
        result = Span()
        for arg, child in reversed(list(zip(node.args, children, strict=True))):
            condition = f"({lower(arg, context).value_sql}) IS NOT NULL"
            result = Span(
                start=_case(condition, child.start, result.start),
                end=_case(condition, child.end, result.end),
                annual=_case(condition, child.annual, result.annual),
                coherent=_case(condition, child.coherent, result.coherent),
                offset=child.offset if child.offset is not None else result.offset,
                share_basis=child.share_basis or result.share_basis,
                clock=_case(condition, child.clock, result.clock),
                basis=_case(condition, child.basis, result.basis),
            )
        return result
    if node.name in SCALAR_FUNCTIONS:
        result = inner
        for child in children[1:]:
            result = _combine(result, child)
        return result
    window = f"PARTITION BY {context.partition_sql} ORDER BY {context.order_sql}"
    if node.name == "ttm":
        frame = window + " ROWS BETWEEN 3 PRECEDING AND CURRENT ROW"
        starts = f"list({inner.start}) OVER ({frame})"
        ends = f"list({inner.end}) OVER ({frame})"
        coherent = [f"count({inner.start}) OVER ({frame}) = 4",
                    f"bool_and({inner.coherent}) OVER ({frame})",
                    f"date_diff('day', ({starts})[1], ({ends})[4]) + 1 BETWEEN 330 AND 380"]
        for index in range(1, 5):
            coherent.append(f"date_diff('day', ({starts})[{index}], ({ends})[{index}]) + 1 BETWEEN 70 AND 120")
            if index < 4:
                coherent.append(f"({ends})[{index}] + INTERVAL 1 DAY = ({starts})[{index + 1}]")
        clock, basis = inner.clock, inner.basis
        if inner.share_basis:
            # Quarters filed before a split are rebased to the frame's basis.
            proof, clock, basis = _window_basis(inner, frame, 4)
            coherent.append(proof)
        return Span(f"({starts})[1]", f"({ends})[4]",
                    f"bool_or({inner.annual}) OVER ({frame})", " AND ".join(coherent), 0, inner.share_basis,
                    clock, basis)
    if node.name == "stdev_q":
        period_argument = node.args[1]
        if not isinstance(period_argument, Number):
            raise TypeError(f"{node.name} metadata requires a numeric literal period")
        size = int(period_argument.value)
        frame = window + f" ROWS BETWEEN {size - 1} PRECEDING AND CURRENT ROW"
        clock, basis = _window_basis(inner, frame, size)[1:] if inner.share_basis else (Span.clock, "true")
        return Span(inner.start, inner.end, f"bool_or({inner.annual}) OVER ({frame})",
                    _consecutive_window(inner, frame, size), 0, inner.share_basis, clock, basis)
    periods = {"avg2": 4, "yoy": 4, "qoq": 1}.get(node.name)
    if node.name in ("lag", "cagr"):
        period_argument = node.args[1]
        if not isinstance(period_argument, Number):
            raise TypeError(f"{node.name} metadata requires a numeric literal period")
        periods = int(period_argument.value) * (4 if node.name == "cagr" else 1)
    if periods is None:
        raise ValueError(f"unsupported filing-grid metadata function {node.name}")
    previous = Span(
        start=f"lag({inner.start}, {periods}) OVER ({window})",
        end=f"lag({inner.end}, {periods}) OVER ({window})",
        annual=f"lag({inner.annual}, {periods}) OVER ({window})",
        coherent=f"lag({inner.coherent}, {periods}) OVER ({window})",
        offset=periods,
        share_basis=inner.share_basis,
        clock=f"lag({inner.clock}, {periods}) OVER ({window})",
        basis=f"lag({inner.basis}, {periods}) OVER ({window})",
    )
    if node.name == "lag":
        return previous
    paired = _combine(inner, previous)
    if node.name == "avg2":
        return Span("NULL::DATE", inner.end, paired.annual, paired.coherent, 0, inner.share_basis,
                    paired.clock, paired.basis)
    # yoy/qoq/cagr publish a basis-free relative change over the pair's span:
    # qoq of two balances spans the quarter between them, like ``B - lag(B, 1)``.
    return Span(paired.start, inner.end, paired.annual, paired.coherent, 0)


def frame_annual_columns(plan: AnnualPlan, quote: Any) -> tuple[list[str], list[str], list[str]]:
    """One ASOF state per exact span/code; no annual x historical-frame panel."""
    if not plan.codes:
        return [], [], []
    joins = [
        "ASOF LEFT JOIN _pit_annual_targets ay ON f.security_id=ay.security_id "
        "AND f.bucket=ay.bucket AND f.event_at >= ay.event_at"
    ]
    projections = [
        "CASE WHEN ay.span.period_end=f.period_end THEN ay.span.period_start END AS annual_start",
        "CASE WHEN ay.span.period_end=f.period_end THEN ay.span.period_end END AS annual_end",
    ]
    lineage = ["struct_pack(kind := 'annual_span', code := '', state := to_json(ay.span))"]
    for index, code in enumerate(plan.codes):
        alias = f"a{index}"
        joins.append(
            f"ASOF LEFT JOIN (SELECT * FROM _pit_annual_items WHERE code={quote(code)}) {alias} "
            f"ON f.security_id={alias}.security_id AND ay.span.period_start={alias}.period_start "
            f"AND ay.span.period_end={alias}.period_end AND f.period_end={alias}.period_end "
            f"AND f.event_at >= {alias}.event_at"
        )
        exponent = _item_share_exponents().get(code, 0)
        value = f"{alias}.state.value"
        if exponent:
            clock = f"{alias}.state.available_at"
            value = f"({value} * {_split_epochs.rebase_factor_sql(clock, 'f.event_at', exponent)})"
            projections.append(f'{_split_epochs.basis_known_sql(clock, "f.event_at")} AS "annual_{code}__basis"')
        projections.extend([
            f'{value} AS "annual_{code}"',
            f'{alias}.state.available_at AS "annual_{code}__at"',
            (f"to_json(struct_pack(kind := 'item', code := {quote(code)}, "
             f'bucket := f.bucket, "offset" := f.target_bucket - f.bucket, '
             f"status := CASE WHEN {alias}.state.state_id IS NULL OR {alias}.state.value IS NULL "
             f"THEN 'missing' ELSE 'selected' END, state_id := {alias}.state.state_id, "
             f"available_at := {alias}.state.available_at, cik := {alias}.state.cik, "
             f"basis := {alias}.state.basis, source := {alias}.state.source, "
             f"period_start := {alias}.state.period_start, period_end := {alias}.state.period_end, "
             f"inputs_hash := NULL::VARCHAR, definition_hash := NULL::VARCHAR)) "
             f'AS "annual_{code}__ref"'),
        ])
        lineage.append(f"struct_pack(kind := 'annual', code := {quote(code)}, state := to_json({alias}.state))")
    return joins, projections, lineage
