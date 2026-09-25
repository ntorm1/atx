"""Research anomaly catalog (R1a): the economic identity of every research metric.

Each row of ``seeds/research_anomaly_catalog.csv`` maps one declarative derived
metric ``(metric_code, window)`` or one formation-time market-scaled composition to
a pre-registered hypothesis: anomaly class, expected sign (``+1`` means a higher
value predicts higher forward returns) with a rationale and a literature reference,
scale type, preferred cross-sectional transform, inherited availability clock,
minimum history, research admission and the legacy factor ids it supersedes.
Evaluation (R3b/R4) tests these hypotheses; it never chooses a sign from data.

Clocks, minimum history and incomparable-by-construction status are *derived* from
the derived-metric seed and must equal the declared values, so the catalog cannot
drift from the metric engine silently. Every seed metric is either a catalog row or
an explicit exclusion in :data:`EXCLUDED_SEED_METRICS`; adding a seed metric without
a catalog decision fails validation.
"""

from __future__ import annotations

import csv
import hashlib
import re
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass, replace
from functools import lru_cache
from pathlib import Path

from .._derived_annual import SHARE_COUNT_CODES, ShareExponent, share_exponent
from .._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY
from ..derived_dsl import SCALAR_FUNCTIONS, BinOp, Call, Neg, Node, Number, Ref, parse_expression
from ..derived_registry import (
    QUARTER_GRID_WINDOWS,
    DerivedMetricDefinition,
    default_derived_definitions,
    topological_order,
)
from ..item_registry import read_fundamental_item_seed

__all__ = [
    "ADMISSIONS",
    "ANOMALY_CATALOG_COLUMNS",
    "ANOMALY_CATALOG_PATH",
    "ANOMALY_CLASSES",
    "AVAILABILITY_CLOCKS",
    "CONTROL_CLASSES",
    "EXCLUDED_SEED_METRICS",
    "AnomalyCatalogEntry",
    "AnomalyCatalogError",
    "MetricShape",
    "anomaly_catalog_sha256",
    "anomaly_class_counts",
    "default_anomaly_catalog",
    "derive_metric_shapes",
    "load_anomaly_catalog",
    "read_anomaly_catalog",
    "render_anomaly_catalog_markdown",
    "validate_anomaly_catalog",
]

_SEEDS = Path(__file__).resolve().parents[1] / "seeds"
ANOMALY_CATALOG_PATH = _SEEDS / "research_anomaly_catalog.csv"
_FACTOR_SEEDS = (_SEEDS / "factor_definitions.csv", _SEEDS / "derived_factor_projections.csv")
_PACKAGE = Path(__file__).resolve().parents[1]

ANOMALY_CATALOG_COLUMNS = (
    "feature_id",
    "source_kind",
    "metric_code",
    "metric_window",
    "numerator",
    "denominator",
    "anomaly_class",
    "economic_definition",
    "expected_sign",
    "sign_rationale",
    "reference",
    "prior_evidence",
    "scale_type",
    "preferred_transform",
    "availability_clock",
    "min_history_quarters",
    "min_history_sessions",
    "admission",
    "admission_note",
    "supersedes",
)

ANOMALY_CLASSES = (
    "value",
    "profitability",
    "quality",
    "growth",
    "investment",
    "accruals",
    "leverage",
    "payout_issuance",
    "efficiency",
    "earnings_stability",
)
#: Priced characteristics used as controls and benchmarks, not as new anomalies.
CONTROL_CLASSES = ("size", "momentum", "reversal", "volatility")
SOURCE_KINDS = frozenset({"seed_metric", "composition"})
SCALE_TYPES = frozenset(
    {"ratio", "yield", "growth_rate", "ratio_change", "score", "days", "dispersion",
     "dollar_level", "return", "volatility"}
)
PREFERRED_TRANSFORMS = frozenset({"winsor_z", "rank_normal", "log_winsor_z"})
#: Strictly positive levels whose cross-section is log-normal-like.
LOG_SCALE_TYPES = frozenset({"dollar_level", "volatility"})
PRIOR_EVIDENCE = frozenset({"published_anomaly", "published_analogue", "economic_conjecture"})
ADMISSIONS = frozenset({"eligible", "eligible_with_caveat", "blocked_incomparable_origin"})

CLOCK_FILING = "conservative_filing_46h"
CLOCK_BAR = "modeled_trade_date_22h"
CLOCK_MAX = "max_filing_46h_trade_date_22h"
AVAILABILITY_CLOCKS: Mapping[str, str] = {
    CLOCK_FILING: f"SEC filing date + 46h ({FUNDAMENTAL_CLOCK_POLICY}); modeled, not measured delivery",
    CLOCK_BAR: "bar trade_date + 22h; modeled end-of-day availability of the daily bar",
    CLOCK_MAX: "latest of the filing clock of every fundamental input and the bar clock",
}
#: Daily market columns that carry only the bar clock. ``dei_shares`` and the
#: ``shares_outstanding`` coalesce are SEC-filing share counts (filing-clocked).
_BAR_ONLY_MARKET_COLUMNS = frozenset({"close", "adj_close", "log_return", "volume", "archive_shares"})

_FEATURE_ID = re.compile(r"^[a-z][a-z0-9_]{1,63}$")
_OPERAND = re.compile(r"^(metric|item):([a-z][a-z0-9_]{0,95})$")
_SUPERSEDES = re.compile(r"^factor:([a-z][a-z0-9_]{1,95})$")

#: Seed metrics that are not research features, each with a reviewed reason:
#: ``dollar_level_input`` (a currency level or flow; not comparable across firms,
#: consumed by cataloged ratios), ``per_share_level`` (depends on share count and
#: price level; its price-scaled form is cataloged), ``inverse_cataloged:<id>``
#: (a price multiple whose sign-changing denominator breaks monotonicity; the
#: monotone inverse ``<id>`` is cataloged) and ``component_of:<id>`` (a term of the
#: cataloged composite ``<id>``).
EXCLUDED_SEED_METRICS: Mapping[str, str] = {
    **{code: "dollar_level_input" for code in (
        "gross_profit_q", "ebitda_q", "cash_st_investments_q", "common_equity_q", "total_debt_q",
        "net_debt", "capex_q", "fcf_q", "invested_capital_q", "invested_capital_ex_goodwill_q",
        "revenue_ttm", "cost_of_revenue_ttm", "gross_profit_ttm", "sga_expense_ttm", "rd_expense_ttm",
        "depreciation_ttm", "operating_income_ttm", "ebitda_ttm", "interest_expense_ttm",
        "pretax_income_ttm", "income_tax_ttm", "net_income_ttm", "net_income_common_ttm", "cfo_ttm",
        "capex_ttm", "fcf_ttm", "dividends_paid_ttm", "common_dividends_ttm", "share_repurchase_ttm",
        "share_issuance_ttm", "debt_issuance_ttm", "debt_reduction_ttm", "stock_compensation_ttm",
        "acquisitions_ttm", "total_payout_ttm", "total_assets_avg2", "stockholders_equity_avg2",
        "common_equity_avg2", "inventory_avg2", "receivables_avg2", "payables_avg2", "total_debt_avg2",
        "invested_capital_avg2", "invested_capital_ex_goodwill_avg2", "nopat_ttm",
        "change_in_receivables_yoy", "change_in_inventory_yoy", "change_in_payables_yoy", "noa",
        "operating_working_capital_q", "enterprise_value",
        # R1b: seasonal changes and their eight-quarter scales, consumed by sue_ni /
        # sue_revenue and the earnings_surprise_to_market composition.
        "ni_q_change_yoy", "ni_q_change_yoy_sd8", "revenue_q_change_yoy", "revenue_q_change_yoy_sd8",
    )},
    **{code: "per_share_level" for code in (
        "eps_diluted_ttm", "eps_basic_ttm", "eps_ttm", "sales_per_share", "book_per_share",
        "tangible_book_value_per_share", "cfo_per_share", "fcf_per_share", "dividends_per_share_ttm",
        "cash_per_share",
    )},
    "pe_ttm": "inverse_cataloged:earnings_yield",
    "pb": "inverse_cataloged:book_to_market",
    "ps_ttm": "inverse_cataloged:sales_to_price",
    "pcf_ttm": "inverse_cataloged:cfo_to_price",
    "ev_ebitda": "inverse_cataloged:ebitda_to_ev",
    "ev_sales": "inverse_cataloged:sales_to_ev",
    **{code: "component_of:beneish_m" for code in (
        "beneish_dsri", "beneish_gmi", "beneish_aqi", "beneish_sgi", "beneish_depi", "beneish_sgai",
        "beneish_tata", "beneish_lvgi",
    )},
    **{code: "component_of:ohlson_o" for code in (
        "ohlson_tlta", "ohlson_wcta", "ohlson_clca", "ohlson_oeneg", "ohlson_nita", "ohlson_futl",
        "ohlson_intwo", "ohlson_chin",
    )},
    "effective_tax_rate_ttm": "component_of:roic",
    "no_equity_issuance_ttm": "component_of:piotroski_f_cash_issuance",
}


class AnomalyCatalogError(ValueError):
    """A malformed, unknown or engine-inconsistent anomaly catalog."""


@dataclass(frozen=True)
class AnomalyCatalogEntry:
    feature_id: str
    source_kind: str
    metric_code: str | None
    metric_window: str | None
    numerator: str | None
    denominator: str | None
    anomaly_class: str
    economic_definition: str
    expected_sign: int
    sign_rationale: str
    reference: str
    prior_evidence: str
    scale_type: str
    preferred_transform: str
    availability_clock: str
    min_history_quarters: int
    min_history_sessions: int
    admission: str
    admission_note: str
    supersedes: tuple[str, ...]

    @property
    def is_control(self) -> bool:
        return self.anomaly_class in CONTROL_CLASSES

    @property
    def is_research_eligible(self) -> bool:
        """False only when the engine labels every quarterly value ``incomparable``."""
        return self.admission != "blocked_incomparable_origin"

    @property
    def operands(self) -> tuple[str, ...]:
        """``metric:``/``item:`` inputs: the seed metric itself or the composition legs."""
        if self.source_kind == "seed_metric":
            return (f"metric:{self.metric_code}",)
        return tuple(value for value in (self.numerator, self.denominator) if value)


# --------------------------------------------------------------------------- reading


def _blank(value: str | None) -> str | None:
    text = (value or "").strip()
    return text or None


def read_anomaly_catalog(path: Path | str = ANOMALY_CATALOG_PATH) -> tuple[AnomalyCatalogEntry, ...]:
    """Parse the catalog CSV strictly (exact header, typed fields); no semantic checks."""
    catalog_path = Path(path)
    entries: list[AnomalyCatalogEntry] = []
    with catalog_path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if tuple(reader.fieldnames or ()) != ANOMALY_CATALOG_COLUMNS:
            raise AnomalyCatalogError(
                f"{catalog_path} header must be {ANOMALY_CATALOG_COLUMNS}, got {reader.fieldnames}"
            )
        for row_number, raw in enumerate(reader, start=2):
            where = f"{catalog_path.name}:{row_number}"
            if None in raw:
                raise AnomalyCatalogError(f"{where}: more fields than the header")
            sign_text = (raw["expected_sign"] or "").strip()
            if sign_text not in ("+1", "-1"):
                raise AnomalyCatalogError(f"{where}: expected_sign must be '+1' or '-1', got {sign_text!r}")
            try:
                quarters = int((raw["min_history_quarters"] or "").strip())
                sessions = int((raw["min_history_sessions"] or "").strip())
            except ValueError as error:
                raise AnomalyCatalogError(f"{where}: min_history_* must be integers") from error
            entries.append(
                AnomalyCatalogEntry(
                    feature_id=(raw["feature_id"] or "").strip(),
                    source_kind=(raw["source_kind"] or "").strip(),
                    metric_code=_blank(raw["metric_code"]),
                    metric_window=_blank(raw["metric_window"]),
                    numerator=_blank(raw["numerator"]),
                    denominator=_blank(raw["denominator"]),
                    anomaly_class=(raw["anomaly_class"] or "").strip(),
                    economic_definition=(raw["economic_definition"] or "").strip(),
                    expected_sign=1 if sign_text == "+1" else -1,
                    sign_rationale=(raw["sign_rationale"] or "").strip(),
                    reference=(raw["reference"] or "").strip(),
                    prior_evidence=(raw["prior_evidence"] or "").strip(),
                    scale_type=(raw["scale_type"] or "").strip(),
                    preferred_transform=(raw["preferred_transform"] or "").strip(),
                    availability_clock=(raw["availability_clock"] or "").strip(),
                    min_history_quarters=quarters,
                    min_history_sessions=sessions,
                    admission=(raw["admission"] or "").strip(),
                    admission_note=(raw["admission_note"] or "").strip(),
                    supersedes=tuple(part.strip() for part in (raw["supersedes"] or "").split("|")
                                     if part.strip()),
                )
            )
    return tuple(entries)


# ------------------------------------------------------------- engine-derived shape


@dataclass(frozen=True)
class MetricShape:
    """What the engine implies for one seed metric, independent of the catalog."""

    min_history_quarters: int
    min_history_sessions: int
    availability_clock: str
    #: Set when every quarterly value is labeled ``value_origin='incomparable'``.
    incomparable_reason: str | None


@dataclass(frozen=True)
class _Span:
    """Static mirror of ``_derived_annual.Span``: what can be proven for every row.

    ``kind`` is the fiscal span of the selected operand: ``quarter`` (a duration
    fact of one quarter), ``instant`` (no period start), ``long`` (a trailing
    multi-quarter span) or ``mixed``. ``never`` names why comparability is false
    for every row, i.e. why the engine's origin label is always ``incomparable``.
    """

    kind: str | None = None
    offset: int | None = None
    share_basis: bool = False
    never: str | None = None


_QUARTER_POSSIBLE = frozenset({"quarter", "mixed"})


def _pair(left: _Span, right: _Span) -> _Span:
    """Mirror ``_derived_annual._combine`` for the cases that are false by construction."""
    if left.offset is None:
        return right
    if right.offset is None:
        return left
    newer, older = (left, right) if left.offset <= right.offset else (right, left)
    distance = abs(left.offset - right.offset)
    never: str | None = None
    if distance == 0:
        # Both starts present and unequal (one quarter vs a trailing span) fails.
        kind = right.kind if left.kind == "instant" else left.kind
        if {left.kind, right.kind} == {"quarter", "long"}:
            never = "quarter_vs_trailing_span_same_bucket"
    else:
        kind = newer.kind
        # _derived_annual._combine: until a split guard exists, a split-sensitive
        # comparison across periods is proven only between flows exactly four
        # quarters apart (the prior-year comparative a filing restates for splits).
        sensitive = newer.share_basis or older.share_basis
        if distance == 1:
            # _one_quarter_apart proves flow after flow, a flow after its opening
            # balance and instant after instant. A trailing span, an instant after a
            # flow and any split-sensitive pair never pass.
            if sensitive:
                never = "one_quarter_pair_per_share_without_split_guard"
            elif "long" in (newer.kind, older.kind) or (newer.kind, older.kind) == ("instant", "quarter"):
                never = "one_quarter_pair_without_provable_quarter_spans"
        elif distance % 4:
            never = "lag_distance_off_the_annual_grid"
        elif sensitive and distance != 4:
            never = "multi_year_share_basis_pair_without_split_guard"
        elif sensitive and "instant" in (newer.kind, older.kind):
            never = "share_basis_balance_pair_without_split_guard"
    return _Span(kind, newer.offset, left.share_basis or right.share_basis,
                 left.never or right.never or never)


def _numeric(node: Node) -> int:
    if not isinstance(node, Number):
        raise AnomalyCatalogError("window length arguments must be numeric literals")
    return int(node.value)


def _span(node: Node, refs: Mapping[str, _Span], exponent_of: Callable[[str], ShareExponent]) -> _Span:
    """Mirror ``_derived_annual.lower_span``, including its per-node split sensitivity.

    Like the engine, every sub-expression's share basis is its formula-derived
    share exponent (``_derived_annual.share_exponent``): nonzero or mixed means a
    split changes the value.
    """
    span = _span_rule(node, refs, exponent_of)
    sensitive = share_exponent(node, exponent_of) != 0
    return span if span.share_basis == sensitive else replace(span, share_basis=sensitive)


def _span_rule(node: Node, refs: Mapping[str, _Span], exponent_of: Callable[[str], ShareExponent]) -> _Span:
    if isinstance(node, Number):
        return _Span()
    if isinstance(node, Ref):
        return refs[node.name]
    if isinstance(node, Neg):
        return _span(node.operand, refs, exponent_of)
    if isinstance(node, BinOp):
        return _pair(_span(node.left, refs, exponent_of), _span(node.right, refs, exponent_of))
    if not isinstance(node, Call):
        raise TypeError(node)
    children = [_span(argument, refs, exponent_of) for argument in node.args]
    inner = children[0]
    if node.name == "coalesce":
        spans = [child for child in children if child.offset is not None]
        kinds = {child.kind for child in spans}
        reasons = [child.never for child in children]
        return _Span(
            kinds.pop() if len(kinds) == 1 else "mixed",
            spans[0].offset if spans else None,
            any(child.share_basis for child in children),
            reasons[0] if all(reasons) else None,
        )
    if node.name in SCALAR_FUNCTIONS:
        result = inner
        for child in children[1:]:
            result = _pair(result, child)
        return result
    if node.name == "ttm":
        never = inner.never or (None if inner.kind in _QUARTER_POSSIBLE else "ttm_over_non_quarter_spans")
        return _Span("long", 0, inner.share_basis, never)
    if node.name == "stdev_q":
        # _derived_annual._consecutive_window (R1c): coherent only for a chain of
        # coherent single-quarter flows or quarter-end instants, never on a share
        # basis. Consecutive trailing (365-day) spans never pass the quarter proof.
        never = inner.never
        if never is None and inner.share_basis:
            never = "stdev_q_over_share_basis_without_split_guard"
        elif never is None and inner.kind == "long":
            never = "stdev_q_over_trailing_spans"
        return _Span(inner.kind, 0, inner.share_basis, never)
    periods = {"avg2": 4, "yoy": 4, "qoq": 1}.get(node.name)
    if node.name == "lag":
        periods = _numeric(node.args[1])
    elif node.name == "cagr":
        periods = 4 * _numeric(node.args[1])
    if periods is None:
        raise AnomalyCatalogError(f"no quarter-grid span rule for {node.name!r}")
    previous = _Span(inner.kind, periods, inner.share_basis, inner.never)
    if node.name == "lag":
        return previous
    paired = _pair(inner, previous)
    if node.name == "avg2":
        return _Span("instant", 0, inner.share_basis, paired.never)
    # yoy/qoq/cagr publish a relative change: basis-free, keeps the newer span.
    return _Span(inner.kind, 0, False, paired.never)


def _history(node: Node, quarters: Mapping[str, int], sessions: Mapping[str, int]) -> tuple[int, int]:
    """(fiscal quarters, daily bars) a value needs; ``coalesce`` takes its cheapest branch."""
    if isinstance(node, Number):
        return 0, 0
    if isinstance(node, Ref):
        return quarters[node.name], sessions[node.name]
    if isinstance(node, Neg):
        return _history(node.operand, quarters, sessions)
    if isinstance(node, BinOp):
        left = _history(node.left, quarters, sessions)
        right = _history(node.right, quarters, sessions)
        return max(left[0], right[0]), max(left[1], right[1])
    if not isinstance(node, Call):
        raise TypeError(node)
    parts = [_history(argument, quarters, sessions) for argument in node.args]
    if node.name == "coalesce":
        return min(part[0] for part in parts), min(part[1] for part in parts)
    if node.name in SCALAR_FUNCTIONS:
        return max(part[0] for part in parts), max(part[1] for part in parts)
    q, s = parts[0]
    if node.name == "ttm":
        return q + 3, s
    if node.name in ("avg2", "yoy"):
        return q + 4, s
    if node.name == "qoq":
        return q + 1, s
    if node.name == "lag":
        return q + _numeric(node.args[1]), s
    if node.name == "cagr":
        return q + 4 * _numeric(node.args[1]), s
    if node.name == "stdev_q":
        return q + _numeric(node.args[1]) - 1, s
    if node.name in ("tret", "lag_d"):
        return q, s + _numeric(node.args[1])
    if node.name in ("avg_d", "rvol"):
        return q, s + _numeric(node.args[1]) - 1
    raise AnomalyCatalogError(f"no history rule for {node.name!r}")


def derive_metric_shapes(
    definitions: Iterable[DerivedMetricDefinition] | None = None,
) -> dict[str, MetricShape]:
    """Engine-implied history, clock and incomparability for every seed metric.

    Incomparability mirrors ``_derived_annual`` (span rules) and ``_derived_pit``
    (an ``incomparable`` input makes the output incomparable) for the cases that
    are false for every row. Split sensitivity is the formula-derived share
    exponent (per-share and share-count operands; ruling: no split guard yet): a
    split-sensitive pair is never proven one quarter apart, across balances or
    beyond four quarters, and a split-sensitive ``stdev_q`` window never is. Also
    false: a one-quarter pair involving a trailing span or an instant after a
    flow; ``stdev_q`` over trailing spans; off-grid lags; a quarter and a trailing
    span of the same bucket. Daily-window metrics are not span-labeled.
    ``tests/test_research_metric_economics.py`` checks this mirror against the
    real engine's ``value_origin`` on a fixture of consecutive quarters.
    """
    rows = default_derived_definitions() if definitions is None else tuple(definitions)
    item_exponents: dict[str, int] = {
        item.canonical_code: -1 for item in read_fundamental_item_seed() if item.unit_type == "per_share"}
    item_exponents.update(dict.fromkeys(SHARE_COUNT_CODES, 1))
    items: dict[str, _Span] = {
        item.canonical_code: _Span("instant" if item.data_type == "instant" else "quarter", 0,
                                   item_exponents.get(item.canonical_code, 0) != 0, None)
        for item in read_fundamental_item_seed()
    }
    shapes: dict[str, MetricShape] = {}
    metric_spans: dict[str, _Span] = {}
    metric_exponents: dict[str, ShareExponent] = {}
    filing_clocked: set[str] = set()
    for definition in topological_order(rows):
        code = definition.metric_code
        node = parse_expression(definition.expression)

        def exponent_of(name: str, owner: DerivedMetricDefinition = definition) -> ShareExponent:
            # Mirrors _derived_annual._metric_share_exponents / AnnualPlan.share_exponents.
            return metric_exponents[name] if name in owner.metric_inputs else item_exponents.get(name, 0)

        exponent = share_exponent(node, exponent_of)
        # Family per_share is never basis-free (engine floor).
        metric_exponents[code] = None if definition.family == "per_share" and exponent == 0 else exponent
        quarters: dict[str, int] = {}
        sessions: dict[str, int] = {}
        refs: dict[str, _Span] = {}
        for name in definition.item_inputs:
            quarters[name], sessions[name] = 1, 0
            refs[name] = items.get(name, _Span("mixed", 0))
        for name in definition.metric_inputs:
            shape = shapes[name]
            quarters[name], sessions[name] = shape.min_history_quarters, shape.min_history_sessions
            span = metric_spans.get(name, _Span("mixed", 0))
            refs[name] = _Span(span.kind, 0, metric_exponents[name] != 0,
                               f"incomparable_input:{name}" if shape.incomparable_reason else None)
        for name in definition.market_inputs:
            quarters[name], sessions[name] = 0, 2 if name == "log_return" else 1
        history = _history(node, quarters, sessions)
        filing = bool(definition.item_inputs) or any(
            name in filing_clocked for name in definition.metric_inputs
        ) or any(name not in _BAR_ONLY_MARKET_COLUMNS for name in definition.market_inputs)
        if filing:
            filing_clocked.add(code)
        reason: str | None = None
        if definition.window in QUARTER_GRID_WINDOWS:
            # A value lives on a fiscal-quarter bucket even when a constant
            # fallback (e.g. ``coalesce(debt, 0)``) needs no input history.
            history = (max(history[0], 1), history[1])
            span = _span(node, refs, exponent_of)
            metric_spans[code] = span
            reason = span.never
            clock = CLOCK_FILING
        else:
            clock = CLOCK_MAX if filing else CLOCK_BAR
        shapes[code] = MetricShape(history[0], history[1], clock, reason)
    return shapes


# ----------------------------------------------------------------------- validation


@lru_cache(maxsize=1)
def _legacy_source_text() -> str:
    paths = sorted(_PACKAGE.glob("*.py")) + sorted((_PACKAGE / "factors").glob("*.py"))
    return "\n".join(path.read_text(encoding="utf-8") for path in paths)


@lru_cache(maxsize=1)
def _seed_factor_ids() -> frozenset[str]:
    ids: set[str] = set()
    for path in _FACTOR_SEEDS:
        with path.open("r", encoding="utf-8", newline="") as handle:
            ids.update((row["factor_id"] or "").strip() for row in csv.DictReader(handle))
    return frozenset(ids)


def _known_legacy_factor(factor_id: str) -> bool:
    """A factor id from the factor seeds or a quoted id in a legacy factor module."""
    return factor_id in _seed_factor_ids() or f'"{factor_id}"' in _legacy_source_text()


def validate_anomaly_catalog(
    entries: Iterable[AnomalyCatalogEntry],
    *,
    definitions: Iterable[DerivedMetricDefinition] | None = None,
    exclusions: Mapping[str, str] | None = None,
) -> None:
    """Reject unknown codes, bad enumerations and any disagreement with the engine.

    All problems are collected and raised together as one :class:`AnomalyCatalogError`.
    """
    rows = tuple(entries)
    seed = default_derived_definitions() if definitions is None else tuple(definitions)
    excluded = EXCLUDED_SEED_METRICS if exclusions is None else exclusions
    by_code = {definition.metric_code: definition for definition in seed}
    shapes = derive_metric_shapes(seed)
    item_codes = frozenset(item.canonical_code for item in read_fundamental_item_seed())
    errors: list[str] = []
    seen: set[str] = set()
    cataloged: set[str] = set()
    superseded: dict[str, str] = {}
    if not rows:
        errors.append("the catalog is empty")
    for entry in rows:
        where = f"feature {entry.feature_id!r}"
        if not _FEATURE_ID.fullmatch(entry.feature_id):
            errors.append(f"{where}: feature_id must match {_FEATURE_ID.pattern}")
        if entry.feature_id in seen:
            errors.append(f"{where}: duplicate feature_id")
        seen.add(entry.feature_id)
        shape: MetricShape | None = None
        if entry.source_kind == "seed_metric":
            definition = by_code.get(entry.metric_code or "")
            if definition is None:
                errors.append(f"{where}: unknown seed metric_code {entry.metric_code!r}")
            else:
                cataloged.add(definition.metric_code)
                shape = shapes[definition.metric_code]
                if entry.metric_window != definition.window:
                    errors.append(f"{where}: metric_window {entry.metric_window!r} but the seed "
                                  f"window of {definition.metric_code!r} is {definition.window!r}")
                if entry.feature_id != definition.metric_code:
                    errors.append(f"{where}: a seed_metric feature_id must equal its metric_code")
            if entry.numerator or entry.denominator:
                errors.append(f"{where}: a seed_metric row has no numerator/denominator")
        elif entry.source_kind == "composition":
            if entry.metric_code or entry.metric_window:
                errors.append(f"{where}: a composition has no metric_code/metric_window")
            if entry.feature_id in by_code or entry.feature_id in item_codes:
                errors.append(f"{where}: composition id collides with a seed metric or item code")
            legs: list[tuple[str, str]] = []
            for operand in (entry.numerator, entry.denominator):
                match = _OPERAND.fullmatch(operand or "")
                if match is None:
                    errors.append(f"{where}: operand {operand!r} must be 'metric:<code>' or 'item:<code>'")
                elif match.group(1) == "metric" and match.group(2) not in by_code:
                    errors.append(f"{where}: unknown seed metric operand {operand!r}")
                elif match.group(1) == "item" and match.group(2) not in item_codes:
                    errors.append(f"{where}: unknown fundamental item operand {operand!r}")
                else:
                    legs.append((match.group(1), match.group(2)))
            if len(legs) == 2:
                if legs[0] == legs[1]:
                    errors.append(f"{where}: numerator and denominator are the same operand")
                daily = [by_code[code].window == "daily" for kind, code in legs if kind == "metric"]
                if not any(daily):
                    errors.append(f"{where}: a composition must be market-scaled (a daily operand); "
                                  "fundamental-only ratios belong in the derived seed")
                leg_shapes = [shapes[code] for kind, code in legs if kind == "metric"]
                quarters = max([1 if kind == "item" else 0 for kind, _ in legs]
                               + [leg.min_history_quarters for leg in leg_shapes])
                sessions = max((leg.min_history_sessions for leg in leg_shapes), default=0)
                reasons = [leg.incomparable_reason for leg in leg_shapes if leg.incomparable_reason]
                shape = MetricShape(quarters, sessions, CLOCK_MAX,
                                    f"incomparable_input:{reasons[0]}" if reasons else None)
        else:
            errors.append(f"{where}: source_kind must be one of {sorted(SOURCE_KINDS)}")
        if entry.anomaly_class not in ANOMALY_CLASSES + CONTROL_CLASSES:
            errors.append(f"{where}: unknown anomaly_class {entry.anomaly_class!r}")
        for field in ("economic_definition", "sign_rationale", "reference"):
            if not getattr(entry, field):
                errors.append(f"{where}: {field} must be non-empty")
        if entry.expected_sign not in (-1, 1):
            errors.append(f"{where}: expected_sign must be -1 or +1")
        if entry.prior_evidence not in PRIOR_EVIDENCE:
            errors.append(f"{where}: prior_evidence must be one of {sorted(PRIOR_EVIDENCE)}")
        if entry.scale_type not in SCALE_TYPES:
            errors.append(f"{where}: unknown scale_type {entry.scale_type!r}")
        if entry.preferred_transform not in PREFERRED_TRANSFORMS:
            errors.append(f"{where}: unknown preferred_transform {entry.preferred_transform!r}")
        elif (entry.preferred_transform == "log_winsor_z") != (entry.scale_type in LOG_SCALE_TYPES):
            errors.append(f"{where}: log_winsor_z is required for, and only for, scale types "
                          f"{sorted(LOG_SCALE_TYPES)}")
        if entry.admission not in ADMISSIONS:
            errors.append(f"{where}: admission must be one of {sorted(ADMISSIONS)}")
        elif (entry.admission == "eligible") == bool(entry.admission_note):
            errors.append(f"{where}: admission_note is required unless admission is 'eligible'")
        for token in entry.supersedes:
            match = _SUPERSEDES.fullmatch(token)
            if match is None or not _known_legacy_factor(match.group(1)):
                errors.append(f"{where}: supersedes {token!r} is not a known legacy factor id")
            elif token in superseded:
                errors.append(f"{where}: {token!r} is already superseded by {superseded[token]!r}")
            else:
                superseded[token] = entry.feature_id
        if shape is None:
            continue
        if entry.availability_clock != shape.availability_clock:
            errors.append(f"{where}: availability_clock {entry.availability_clock!r} but the "
                          f"inherited clock is {shape.availability_clock!r}")
        if (entry.min_history_quarters, entry.min_history_sessions) != (
                shape.min_history_quarters, shape.min_history_sessions):
            errors.append(f"{where}: min history ({entry.min_history_quarters}q, "
                          f"{entry.min_history_sessions}s) but the definition needs "
                          f"({shape.min_history_quarters}q, {shape.min_history_sessions}s)")
        blocked = entry.admission == "blocked_incomparable_origin"
        if shape.incomparable_reason and not blocked:
            errors.append(f"{where}: every quarterly value is labeled incomparable "
                          f"({shape.incomparable_reason}); admission must be blocked_incomparable_origin")
        if blocked and not shape.incomparable_reason:
            errors.append(f"{where}: blocked_incomparable_origin but the engine can label it comparable")
    feature_ids = {entry.feature_id for entry in rows}
    for code, reason in sorted(excluded.items()):
        if code not in by_code:
            errors.append(f"exclusion {code!r} is not a seed metric")
        if code in cataloged:
            errors.append(f"seed metric {code!r} is both cataloged and excluded")
        kind, _, target = reason.partition(":")
        if (kind in ("dollar_level_input", "per_share_level") and not target) or (
                kind in ("inverse_cataloged", "component_of") and target in feature_ids):
            continue
        errors.append(f"exclusion {code!r} has an invalid reason {reason!r}")
    for code in sorted(set(by_code) - cataloged - set(excluded)):
        errors.append(f"seed metric {code!r} has no catalog row and no exclusion")
    if errors:
        raise AnomalyCatalogError("anomaly catalog invalid:\n  " + "\n  ".join(errors))


def load_anomaly_catalog(path: Path | str = ANOMALY_CATALOG_PATH) -> tuple[AnomalyCatalogEntry, ...]:
    entries = read_anomaly_catalog(path)
    validate_anomaly_catalog(entries)
    return entries


@lru_cache(maxsize=1)
def default_anomaly_catalog() -> tuple[AnomalyCatalogEntry, ...]:
    return load_anomaly_catalog()


def anomaly_catalog_sha256(path: Path | str = ANOMALY_CATALOG_PATH) -> str:
    """Content digest for feature/evaluation manifests (R2b ``feature_version`` input)."""
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def anomaly_class_counts(
    entries: Iterable[AnomalyCatalogEntry], *, eligible_only: bool = False,
) -> dict[str, int]:
    """Rows per class in catalog class order (zero-count classes included)."""
    counts = dict.fromkeys(ANOMALY_CLASSES + CONTROL_CLASSES, 0)
    for entry in entries:
        if not eligible_only or entry.is_research_eligible:
            counts[entry.anomaly_class] = counts.get(entry.anomaly_class, 0) + 1
    return counts


# -------------------------------------------------------------------------- docs


def _cell(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def render_anomaly_catalog_markdown(entries: Iterable[AnomalyCatalogEntry] | None = None) -> str:
    """The generated body of ``docs/research/ANOMALY_CATALOG.md``."""
    rows = default_anomaly_catalog() if entries is None else tuple(entries)
    total = anomaly_class_counts(rows)
    eligible = anomaly_class_counts(rows, eligible_only=True)
    lines = [
        "# Research anomaly catalog",
        "",
        "Generated from `atx-db/src/atx_db/seeds/research_anomaly_catalog.csv` by",
        "`atx_db.research.catalog.render_anomaly_catalog_markdown()`; "
        "`tests/test_research_catalog.py` fails when this file is stale.",
        "Edit the CSV (and `EXCLUDED_SEED_METRICS`), never this file by hand.",
        "",
        "Every row is a pre-registered hypothesis: `expected_sign` +1 means a higher value "
        "predicts higher 1-12 month forward returns. Evaluation tests the sign; it never "
        "chooses it. `prior_evidence`: `published_anomaly` (the metric, or its standard "
        "construction, is a published anomaly with this sign), `published_analogue` (a close "
        "published relative; the sign is carried over), `economic_conjecture` (the sign is "
        "argued, not published). Qualification should treat conjectures as exploratory.",
        "",
        "Admission: `eligible`; `eligible_with_caveat` (a known construction hazard, "
        "noted per row); `blocked_incomparable_origin` (the derived engine labels every "
        "quarterly value `value_origin='incomparable'`, which the research gate rejects; "
        "not testable until the engine can prove comparability).",
        "",
        "Clocks are inherited, never declared freely: "
        + "; ".join(f"`{key}` = {value}" for key, value in AVAILABILITY_CLOCKS.items())
        + ". Minimum history counts fiscal quarters (`q`) and daily bars (`s`) needed for "
        "one value, derived from the metric's expression and its dependencies.",
        "",
        "Compositions are market-scaled ratios declared here and computed at formation by "
        "the feature store (R2b), clock = latest input clock.",
        "",
        "## Class counts",
        "",
        "| class | role | rows | research-eligible |",
        "|---|---|---:|---:|",
    ]
    for name in ANOMALY_CLASSES + CONTROL_CLASSES:
        role = "control" if name in CONTROL_CLASSES else "anomaly"
        lines.append(f"| {name} | {role} | {total[name]} | {eligible[name]} |")
    lines.append(f"| **all** | | **{sum(total.values())}** | **{sum(eligible.values())}** |")
    for name in ANOMALY_CLASSES + CONTROL_CLASSES:
        members = [entry for entry in rows if entry.anomaly_class == name]
        if not members:
            continue
        lines += [
            "",
            f"## {name}",
            "",
            "| feature | source | sign | definition | reference | evidence | transform | clock "
            "| history | admission |",
            "|---|---|:-:|---|---|---|---|---|---|---|",
        ]
        for entry in members:
            if entry.source_kind == "seed_metric":
                source = f"`{entry.metric_code}` ({entry.metric_window})"
            else:
                source = f"`{entry.numerator}` / `{entry.denominator}`"
            admission = entry.admission
            if entry.admission_note:
                admission += f": {entry.admission_note}"
            lines.append(
                f"| `{entry.feature_id}` | {source} | {'+1' if entry.expected_sign > 0 else '-1'} "
                f"| {_cell(entry.economic_definition)} | {_cell(entry.reference)} "
                f"| {entry.prior_evidence} | {entry.preferred_transform} | {entry.availability_clock} "
                f"| {entry.min_history_quarters}q/{entry.min_history_sessions}s | {_cell(admission)} |"
            )
    lines += [
        "",
        "## Seed metrics that are not research features",
        "",
        "| metric | reason |",
        "|---|---|",
    ]
    lines += [f"| `{code}` | {reason} |" for code, reason in sorted(EXCLUDED_SEED_METRICS.items())]
    return "\n".join(lines) + "\n"
