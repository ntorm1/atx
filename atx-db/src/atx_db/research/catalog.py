"""Research anomaly catalog (R1a): the economic identity of every research metric.

Each row of ``seeds/research_anomaly_catalog.csv`` maps one declarative derived
metric ``(metric_code, window)``, one research-panel native (``panel_native``: a
price/liquidity feature the panel computes from the line's own daily bars, declared
in ``research.panel.NATIVE_FEATURES``), one research-store source feature (``event``:
P3 earnings events, ``factor_exposure``: P4 factor exposures, ``ownership``: P9 13F and
FINRA features; read by the feature store from one pinned sealed source version, declared
in ``research.features.EXTERNAL_FEATURES``) or one formation-time market-scaled
composition to a pre-registered hypothesis: anomaly class, expected sign (``+1`` means a higher
value predicts higher forward returns; ``two_sided`` pre-registers no direction
where the published evidence disagrees) with a rationale and a literature
reference, the hypothesis family its near-duplicates share, scale type, preferred
cross-sectional transform, the hypothesis domain the feature store enforces,
inherited availability clock, minimum history, research admission with
machine-readable caveat codes and the legacy factor ids it supersedes.
Evaluation (R3b/R4) tests these hypotheses; it never chooses a sign from data.

Research metadata (CB2) on every row: ``population`` (:data:`POPULATIONS`: the firms
the hypothesis is defined on, against which coverage is measured), ``evidence_class``
(:data:`EVIDENCE_CLASSES`, derived: ``replication`` for a published anomaly or analogue
with a pre-registered sign, ``discovery`` otherwise), ``publication_year`` (derived: the
earliest year the reference cites, for published rows only), ``jkp_theme``
(:data:`JKP_THEMES` or ``none``) and ``wave`` (:data:`WAVES`: the pre-registration wave
that evaluates the row). One hypothesis is never tested twice: a second construction of
a cataloged hypothesis is ``blocked_duplicate_hypothesis`` and names its primary in
:data:`DUPLICATE_HYPOTHESES`.

Clocks, minimum history and incomparable-by-construction status are *derived* from
the derived-metric seed (for a panel native: from its panel declaration; for a
research-store source: from :data:`EXTERNAL_FEATURE_SHAPES`, declared from each
producer's rules) and must
equal the declared values, so the catalog cannot drift from the metric engine silently. Every seed metric is either a catalog row or
an explicit exclusion in :data:`EXCLUDED_SEED_METRICS`; adding a seed metric without
a catalog decision fails validation.

``domain`` is ``<rule>`` or ``<rule>:<operand>`` with a rule from
:data:`DOMAIN_RULES`; the rule applies to the feature value itself unless it names a
``metric:``/``item:`` operand of the same state. The feature store (R2b) enforces it
before any transform: an out-of-domain value is excluded (or carried as the named
separate indicator), never ranked as ``valid``.
"""

from __future__ import annotations

import csv
import hashlib
import re
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass, replace
from functools import lru_cache
from pathlib import Path
from typing import Any

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
    "BLOCKED_ADMISSIONS",
    "CAVEAT_CODES",
    "CONTROL_CLASSES",
    "DOMAIN_RULES",
    "DUPLICATE_HYPOTHESES",
    "EVIDENCE_CLASSES",
    "EXCLUDED_SEED_METRICS",
    "EXTERNAL_FEATURE_SHAPES",
    "EXTERNAL_SOURCE_KINDS",
    "JKP_THEMES",
    "JKP_THEME_NONE",
    "POPULATIONS",
    "WAVES",
    "AnomalyCatalogEntry",
    "AnomalyCatalogError",
    "MetricShape",
    "anomaly_catalog_sha256",
    "anomaly_class_counts",
    "default_anomaly_catalog",
    "derive_evidence_class",
    "derive_metric_shapes",
    "load_anomaly_catalog",
    "read_anomaly_catalog",
    "reference_publication_year",
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
    "hypothesis_family",
    "scale_type",
    "preferred_transform",
    "domain",
    "availability_clock",
    "min_history_quarters",
    "min_history_sessions",
    "admission",
    "caveat_code",
    "admission_note",
    "supersedes",
    # CB2 research metadata.
    "population",
    "evidence_class",
    "publication_year",
    "jkp_theme",
    "wave",
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
    #: Liquidity premia (CB1): an anomaly class, not a control, so the feature store
    #: generates and tests the size-neutral variant (illiquidity net of size).
    "liquidity",
    #: CB2: institutional ownership, ownership breadth and short interest (P9 13F / FINRA).
    "ownership",
    #: CB2: where a firm stands in its reporting calendar (earnings announcement premium).
    "event_timing",
)
#: Priced characteristics used as controls and benchmarks, not as new anomalies.
CONTROL_CLASSES = ("size", "momentum", "reversal", "volatility")
#: Research-store sources (E1): read by the feature store from one pinned sealed source
#: version -- ``event`` (P3 ``research_event_features``), ``factor_exposure`` (P4
#: ``research_factor_exposures``) and ``ownership`` (P9 ``research_ownership_features``).
EXTERNAL_SOURCE_KINDS = frozenset({"event", "factor_exposure", "ownership"})
SOURCE_KINDS = frozenset({"seed_metric", "panel_native", "composition"}) | EXTERNAL_SOURCE_KINDS
#: Bar-table prefix of a panel native's inputs: bar-only natives keep the bar clock.
_BAR_INPUT_PREFIX = "equity_daily_bars."
SCALE_TYPES = frozenset(
    {"ratio", "yield", "growth_rate", "ratio_change", "score", "days", "dispersion",
     "dollar_level", "return", "volatility"}
)
PREFERRED_TRANSFORMS = frozenset({"winsor_z", "rank_normal", "log_winsor_z"})
#: Strictly positive levels whose cross-section is log-normal-like.
LOG_SCALE_TYPES = frozenset({"dollar_level", "volatility"})
PRIOR_EVIDENCE = frozenset({"published_anomaly", "published_analogue", "economic_conjecture"})
#: Prior evidence that cites a publication documenting the relation (or its close relative).
_PUBLISHED_EVIDENCE = frozenset({"published_anomaly", "published_analogue"})
ADMISSION_DUPLICATE = "blocked_duplicate_hypothesis"
ADMISSIONS = frozenset({"eligible", "eligible_with_caveat", "blocked_incomparable_origin", "blocked_known_bias",
                        ADMISSION_DUPLICATE})
#: Admissions that keep a row out of research: the engine labels every quarterly
#: value incomparable, a known construction bias the span labels cannot see, or a
#: second construction of a cataloged hypothesis (the reason is noted and coded per row).
BLOCKED_ADMISSIONS = frozenset({"blocked_incomparable_origin", "blocked_known_bias", ADMISSION_DUPLICATE})
#: A second construction of a cataloged hypothesis -> its primary row. The primary is the
#: one tested; the duplicate stays cataloged (diagnostics) as ``blocked_duplicate_hypothesis``.
#: ``sue_ni_event`` (P3) reads the very ``sue_ni`` states on a clock never earlier than their
#: filing clock and only where an earnings event is visible, so it adds no timing and at
#: most ``sue_ni``'s coverage (the feature store refuses to build both).
DUPLICATE_HYPOTHESES: Mapping[str, str] = {"sue_ni_event": "sue_ni"}

#: CB2 ``population``: the firms a hypothesis is defined on (its coverage denominator). A
#: conditional feature names the optional activity whose absence leaves it without a value.
POPULATIONS: Mapping[str, str] = {
    "all": "every firm of the research universe",
    "rd_reporters": "firms that report research and development expense (a missing R&D tag is never read as zero)",
    "dividend_payers": "firms that pay common dividends (a non-payer that never tags a dividend has no value)",
    "inventory_holders": "firms that report inventory (no presence-rule zero is read)",
    "advertising_reporters": "firms that report advertising expense",
    "interest_payers": "firms that report interest expense (a firm without debt has no coverage ratio)",
}
#: CB2 ``evidence_class`` (derived, :func:`derive_evidence_class`): the gate a row is graded under.
EVIDENCE_CLASSES: Mapping[str, str] = {
    "replication": "a published anomaly or published analogue with a pre-registered sign (one-sided test)",
    "discovery": "an economic conjecture, or a two-sided hypothesis with no pre-registered sign",
}
#: CB2 ``jkp_theme``: the 13 theme clusters of Jensen, Kelly and Pedersen (2023, Journal of
#: Finance). A row takes the cluster of the JKP characteristic measuring the same construct,
#: else the theme its construct belongs to, else :data:`JKP_THEME_NONE`.
JKP_THEMES = ("accruals", "debt_issuance", "investment", "low_leverage", "low_risk", "momentum", "profit_growth",
              "profitability", "quality", "seasonality", "size", "short_term_reversal", "value")
JKP_THEME_NONE = "none"
#: CB2 ``wave``: the pre-registration wave whose frozen catalog digest evaluates the row.
WAVES: Mapping[str, str] = {
    "w0_existing": "the catalog rows that existed before the tier-1 v2 waves (R1a-R1b, CB1)",
    "w1_price": "price and friction natives computed from the retained daily bars",
    "w2_fund_a": "accounting characteristics, fundamental batch A",
    "w3_compositions": "market-scaled compositions and market-dependent scores",
    "w4_events": "event features and pinned research-store sources (P3 earnings events, P4 factor exposures, "
                 "EDGAR filing events)",
    "w5_ownership": "13F institutional ownership and FINRA short interest",
}
#: ``expected_sign`` text -> value; ``two_sided`` (0) pre-registers no direction.
_SIGNS: Mapping[str, int] = {"+1": 1, "-1": -1, "two_sided": 0}

#: Hypothesis domain rules the feature store (R2b) enforces before any transform.
DOMAIN_RULES: Mapping[str, str] = {
    "unrestricted": "every finite value is in the hypothesis domain",
    "guarded_in_definition": "the definition itself yields no value outside the domain (a non-positive "
                             "opening balance or endpoint has no value); nothing further to enforce",
    "positive_value_required": "values at or below zero are out of the domain (log-scaled levels)",
    "positive_denominator_required": "out of the domain when the named denominator is at or below zero or "
                                     "missing: a non-positive denominator flips the ratio's meaning",
    "negative_book_excluded": "out of the domain when book equity (the named operand, else the value) is at or "
                              "below zero, the Fama-French convention",
    "loss_firms_separate": "a value whose earnings or cash-flow numerator (the named operand, else the value) is "
                           "at or below zero leaves the ranked domain and is carried as a separate loss "
                           "indicator (Fama-French 1992 E(+)/P)",
    "zero_payer_separate": "a zero value (a non-payer) leaves the ranked domain and is carried as a separate "
                           "indicator; the zero-dividend puzzle makes the relation U-shaped",
}
_OPERAND_DOMAINS = frozenset({"positive_denominator_required", "negative_book_excluded", "loss_firms_separate"})
_OPERAND_REQUIRED_DOMAINS = frozenset({"positive_denominator_required"})

#: Machine-readable construction hazards; every non-eligible admission names one.
CAVEAT_CODES: Mapping[str, str] = {
    "sign_flip": "a non-positive denominator or book value inverts the ratio's meaning",
    "fiscal_seasonality": "a single-quarter level or dispersion carries fiscal seasonality",
    "sequential_quarter": "adjacent fiscal quarters differ in season and length",
    "split_basis": "a per-share or share-count comparison or window: comparable only on one filing's clock or "
                   "a split basis proven from daily bars (R1d), so the research gate admits it row by row",
    "trailing_span_overlap": "trailing-twelve-month spans never form a single-quarter chain",
    "non_monotone": "the published relation is U-shaped or holds only within a subgroup",
    "mixed_evidence": "published evidence disagrees on the sign; the hypothesis is two-sided",
    "coverage_bias": "an input is missing for a non-random group of filers",
    "presence_rule": "a value can rest on a zero imputed from a balance's absence under a presence guard: debt or "
                     "inventory only when the issuer has never tagged a mapped alias up to that quarter; a switch "
                     "to an unmapped alias has no value (flows are never imputed: an absent discrete quarter "
                     "cannot prove absence from a year-to-date or annual fact)",
    "unguarded_zero": "a missing optional balance (preferred stock, minority interest, goodwill, other "
                      "intangibles) is read as zero with no presence guard",
    "construct_deviation": "the definition deviates from the published construction (see the note)",
    "filing_clock_lag": "the filing clock trails the market's first information (the earnings release)",
    "io_above_one": "13F shares above the verified share count (double counting, lending, stale counts) are kept, "
                    "never clipped, and counted per formation",
    "duplicate_hypothesis": "a second construction of a cataloged hypothesis (DUPLICATE_HYPOTHESES names the primary "
                            "row); cataloged for diagnostics, never tested beside the primary",
}
#: A sign-flipping ratio names the operand to test; a non-monotone relation names
#: the subgroup the feature store carries separately.
_CAVEAT_DOMAINS: Mapping[str, frozenset[str]] = {
    "sign_flip": frozenset({"positive_denominator_required", "negative_book_excluded"}),
    "non_monotone": frozenset({"loss_firms_separate", "zero_payer_separate"}),
}

CLOCK_FILING = "conservative_filing_46h"
CLOCK_BAR = "modeled_trade_date_22h"
CLOCK_MAX = "max_filing_46h_trade_date_22h"
CLOCK_FINRA = "finra_publication_modeled"
AVAILABILITY_CLOCKS: Mapping[str, str] = {
    CLOCK_FILING: f"SEC filing date + 46h ({FUNDAMENTAL_CLOCK_POLICY}); modeled, not measured delivery",
    CLOCK_BAR: "bar trade_date + 22h; modeled end-of-day availability of the daily bar",
    CLOCK_MAX: "latest of the filing clock of every fundamental input and the bar clock",
    CLOCK_FINRA: "FINRA settlement + 8 business days at 22:00 UTC (never before the loader's settlement + 10 days "
                 "+ 22h, nor before the clock of a verified share count it reads); modeled dissemination, not "
                 "measured",
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
#: monotone inverse ``<id>`` is cataloged), ``component_of:<id>`` (a term of the
#: cataloged composite ``<id>``), ``duplicate_of:<id>`` (equal to the cataloged
#: ``<id>`` for every value in its domain), ``negation_of:<id>`` (exactly minus the
#: cataloged ``<id>``), ``conflicting_prior:<id>`` (a conjecture nearly identical
#: to the cataloged ``<id>`` but argued with the opposite sign; one family member
#: would be chosen by data, so it is dropped) and ``presence_indicator`` (a 0/1
#: building block of an ever-reported missing-is-not-zero rule).
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
    # Ever-reported presence indicators of the missing-is-not-zero rules (debt, inventory).
    **dict.fromkeys((
        "debt_alias_seen_q", "debt_alias_seen_4q", "debt_alias_seen_20q", "debt_alias_ever_q",
        "inventory_alias_seen_q", "inventory_alias_seen_4q", "inventory_alias_seen_20q", "inventory_alias_ever_q",
    ), "presence_indicator"),
    "effective_tax_rate_ttm": "component_of:roic",
    "no_equity_issuance_ttm": "component_of:piotroski_f_cash_issuance",
    # cagr(x, 1) = x / x_-4 - 1 = yoy(x) whenever both endpoints are positive.
    "revenue_cagr_1y": "duplicate_of:revenue_growth_yoy",
    # (repurchases - issuance) / avg assets = -net_equity_issuance.
    "buyback_ratio": "negation_of:net_equity_issuance",
    # ROE x retention ~ book-equity growth for non-issuers, argued +1 against -1.
    "sustainable_growth": "conflicting_prior:book_value_growth_yoy",
}
_TARGETED_EXCLUSIONS = frozenset(
    {"inverse_cataloged", "component_of", "duplicate_of", "negation_of", "conflicting_prior"})


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
    #: +1 or -1; 0 when the hypothesis is pre-registered ``two_sided``.
    expected_sign: int
    sign_rationale: str
    reference: str
    prior_evidence: str
    hypothesis_family: str
    scale_type: str
    preferred_transform: str
    domain: str
    availability_clock: str
    min_history_quarters: int
    min_history_sessions: int
    admission: str
    caveat_codes: tuple[str, ...]
    admission_note: str
    supersedes: tuple[str, ...]
    #: CB2 research metadata (see the module docstring).
    population: str
    evidence_class: str
    #: The earliest year the reference cites; None for an economic conjecture.
    publication_year: int | None
    jkp_theme: str
    wave: str

    @property
    def is_control(self) -> bool:
        return self.anomaly_class in CONTROL_CLASSES

    @property
    def is_two_sided(self) -> bool:
        return self.expected_sign == 0

    @property
    def is_research_eligible(self) -> bool:
        """False for a blocked admission: engine-incomparable or a known construction bias."""
        return self.admission not in BLOCKED_ADMISSIONS

    @property
    def domain_rule(self) -> str:
        return self.domain.partition(":")[0]

    @property
    def domain_operand(self) -> str | None:
        """The ``metric:``/``item:`` operand the domain rule tests; None tests the value itself."""
        return self.domain.partition(":")[2] or None

    @property
    def operands(self) -> tuple[str, ...]:
        """``metric:``/``item:`` seed inputs: the seed metric itself or the composition legs.

        A panel native or a research-store source row has none: it is computed by the
        panel or by its source module, not read as a seed metric.
        """
        if self.source_kind == "seed_metric":
            return (f"metric:{self.metric_code}",)
        return tuple(value for value in (self.numerator, self.denominator) if value)


# --------------------------------------------------------------------------- reading


def _blank(value: str | None) -> str | None:
    text = (value or "").strip()
    return text or None


def _split(value: str | None) -> tuple[str, ...]:
    """A ``|``-separated list cell (empty means none)."""
    return tuple(part.strip() for part in (value or "").split("|") if part.strip())


_YEAR = re.compile(r"\b(1[89]\d{2}|20\d{2})\b")


def reference_publication_year(reference: str) -> int | None:
    """The earliest four-digit year the reference cites (None when it cites none).

    The catalog's ``publication_year`` of a published row: the first publication of the
    relation among the cited works (the pre/post-publication split keys on it).
    """
    years = [int(year) for year in _YEAR.findall(reference or "")]
    return min(years) if years else None


def derive_evidence_class(prior_evidence: str, expected_sign: int) -> str:
    """``replication`` for a published anomaly or analogue with a pre-registered sign, else ``discovery``.

    A two-sided row pre-registers no direction, so it cannot be a one-sided replication
    even when its relation is published; an economic conjecture is always a discovery.
    """
    return "replication" if prior_evidence in _PUBLISHED_EVIDENCE and expected_sign != 0 else "discovery"


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
            if sign_text not in _SIGNS:
                raise AnomalyCatalogError(
                    f"{where}: expected_sign must be '+1', '-1' or 'two_sided', got {sign_text!r}")
            try:
                quarters = int((raw["min_history_quarters"] or "").strip())
                sessions = int((raw["min_history_sessions"] or "").strip())
            except ValueError as error:
                raise AnomalyCatalogError(f"{where}: min_history_* must be integers") from error
            year_text = (raw["publication_year"] or "").strip()
            try:
                year = int(year_text) if year_text else None
            except ValueError as error:
                raise AnomalyCatalogError(f"{where}: publication_year must be a year or empty") from error
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
                    expected_sign=_SIGNS[sign_text],
                    sign_rationale=(raw["sign_rationale"] or "").strip(),
                    reference=(raw["reference"] or "").strip(),
                    prior_evidence=(raw["prior_evidence"] or "").strip(),
                    hypothesis_family=(raw["hypothesis_family"] or "").strip(),
                    scale_type=(raw["scale_type"] or "").strip(),
                    preferred_transform=(raw["preferred_transform"] or "").strip(),
                    domain=(raw["domain"] or "").strip(),
                    availability_clock=(raw["availability_clock"] or "").strip(),
                    min_history_quarters=quarters,
                    min_history_sessions=sessions,
                    admission=(raw["admission"] or "").strip(),
                    caveat_codes=_split(raw["caveat_code"]),
                    admission_note=(raw["admission_note"] or "").strip(),
                    supersedes=_split(raw["supersedes"]),
                    population=(raw["population"] or "").strip(),
                    evidence_class=(raw["evidence_class"] or "").strip(),
                    publication_year=year,
                    jkp_theme=(raw["jkp_theme"] or "").strip(),
                    wave=(raw["wave"] or "").strip(),
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
    #: A share-basis comparison or window (R1d): each value is comparable only on
    #: one filing's clock or a split basis proven from daily bars, so the engine
    #: labels it per row and research gates each row on ``value_origin``.
    split_gated: bool = False
    #: The value (or an input) applies a presence rule: a zero imputed from a
    #: concept's absence (the DSL idiom ``x * 0``), never a reported zero.
    reads_absence: bool = False
    #: The value (or an input) reads a missing input as a literal constant with no
    #: presence guard (``coalesce(<input>, 0)``).
    reads_unguarded_zero: bool = False


#: Clock and minimum history of every research-store source feature, declared from its
#: producer's rules (the source modules carry no machine-readable history). Sessions follow
#: the panel convention: a window of k daily returns needs k + 1 bars.
#:
#: * P3 ``research/events.py``: every event value is visible from the later of the evidence
#:   clock (acceptance, SEC filed date + 46 h) and the 22:00 UTC close of event session E + 1,
#:   and no earlier than its bars (max clock). ``ear_m1p1`` sums the abnormal returns of
#:   E-1..E+1 (3 returns, 4 bars), ``runup_m21_m2`` those of E-21..E-2 (20 returns, 21 bars),
#:   ``days_since_announcement`` reads the event session only, ``sue_ni_event`` re-clocks the
#:   ``sue_ni`` states (its history is ``sue_ni``'s: :data:`_EXTERNAL_SEED_STATES`).
#: * P4 ``research/factor_returns.py``: 252 daily excess returns (253 bars, at least 200
#:   observed) on the daily VW market weighted by verified DEI caps (filing-clocked): max clock.
#: * P9 ``research/ownership_features.py``: a 13F value is visible from the later of its report
#:   quarter end + 45 days + 46 h (the filing deadline) and the clocks of the filings it reads;
#:   IO and its change also read the DEI share count at the quarter end (max clock), breadth
#:   reads filings only (filing clock); one report quarter for IO, two for a change. Short
#:   interest is visible from the modeled FINRA publication (and the share count it reads).
EXTERNAL_FEATURE_SHAPES: Mapping[tuple[str, str], MetricShape] = {
    ("event", "ear_m1p1"): MetricShape(0, 4, CLOCK_MAX, None),
    ("event", "runup_m21_m2"): MetricShape(0, 21, CLOCK_MAX, None),
    ("event", "days_since_announcement"): MetricShape(0, 0, CLOCK_MAX, None),
    ("event", "sue_ni_event"): MetricShape(0, 0, CLOCK_MAX, None),
    ("factor_exposure", "beta_mkt_252d"): MetricShape(0, 253, CLOCK_MAX, None),
    ("factor_exposure", "ivol_252d"): MetricShape(0, 253, CLOCK_MAX, None),
    ("ownership", "io_ratio_13f"): MetricShape(1, 0, CLOCK_MAX, None),
    ("ownership", "io_change_13f"): MetricShape(2, 0, CLOCK_MAX, None),
    ("ownership", "breadth_change_13f"): MetricShape(2, 0, CLOCK_FILING, None),
    ("ownership", "short_interest_ratio"): MetricShape(0, 0, CLOCK_FINRA, None),
    ("ownership", "days_to_cover_si"): MetricShape(0, 0, CLOCK_FINRA, None),
}
#: A source feature that re-clocks a seed metric's states inherits that metric's engine
#: shape (history, comparability, split basis, presence rules) under its own clock.
_EXTERNAL_SEED_STATES: Mapping[tuple[str, str], str] = {("event", "sue_ni_event"): "sue_ni"}


@dataclass(frozen=True)
class _Span:
    """Static mirror of ``_derived_annual.Span``: what can be proven for every row.

    ``kind`` is the fiscal span of the selected operand: ``quarter`` (a duration
    fact of one quarter), ``instant`` (no period start), ``long`` (a trailing
    multi-quarter span) or ``mixed``. ``never`` names why comparability is false
    for every row, i.e. why the engine's origin label is always ``incomparable``.
    ``split_gated`` marks a share-basis comparison or window anywhere below.
    """

    kind: str | None = None
    offset: int | None = None
    share_basis: bool = False
    never: str | None = None
    split_gated: bool = False


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
    gated = left.split_gated or right.split_gated
    if distance == 0:
        # Both starts present and unequal (one quarter vs a trailing span) fails.
        kind = right.kind if left.kind == "instant" else left.kind
        if {left.kind, right.kind} == {"quarter", "long"}:
            never = "quarter_vs_trailing_span_same_bucket"
    else:
        # A proven one-quarter pair spans that quarter: two balances one bucket
        # apart get the older end + 1 day as their start in _combine.
        kind = "quarter" if distance == 1 else newer.kind
        # R1d: a split-sensitive comparison across periods is comparable when both
        # values share one filing clock or both split bases are proven from daily
        # bars (_split_epochs); that is decided per row, never statically.
        gated = gated or newer.share_basis or older.share_basis
        if distance == 1:
            # _one_quarter_apart proves flow after flow, a flow after its opening
            # balance and instant after instant. A trailing span or an instant
            # after a flow never passes.
            if "long" in (newer.kind, older.kind) or (newer.kind, older.kind) == ("instant", "quarter"):
                never = "one_quarter_pair_without_provable_quarter_spans"
        elif distance % 4:
            never = "lag_distance_off_the_annual_grid"
    return _Span(kind, newer.offset, left.share_basis or right.share_basis,
                 left.never or right.never or never, gated)


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
        # A constant branch (the zero of a presence rule) selects no fiscal dates,
        # so the selected span's kind is not known statically.
        constant = len(spans) < len(children)
        return _Span(
            kinds.pop() if len(kinds) == 1 and not constant else "mixed",
            spans[0].offset if spans else None,
            any(child.share_basis for child in children),
            reasons[0] if all(reasons) else None,
            any(child.split_gated for child in children),
        )
    if node.name in SCALAR_FUNCTIONS:
        result = inner
        for child in children[1:]:
            result = _pair(result, child)
        return result
    # R1d _window_basis: a trailing sum or a stdev_q window over share-basis
    # values is comparable only on one filing clock or proven split bases.
    window_gated = inner.split_gated or inner.share_basis
    if node.name == "ttm":
        never = inner.never or (None if inner.kind in _QUARTER_POSSIBLE else "ttm_over_non_quarter_spans")
        return _Span("long", 0, inner.share_basis, never, window_gated)
    if node.name == "stdev_q":
        # _derived_annual._consecutive_window (R1c): coherent only for a chain of
        # coherent single-quarter flows or quarter-end instants. Consecutive
        # trailing (365-day) spans never pass the quarter proof.
        never = inner.never
        if never is None and inner.kind == "long":
            never = "stdev_q_over_trailing_spans"
        return _Span(inner.kind, 0, inner.share_basis, never, window_gated)
    periods = {"avg2": 4, "yoy": 4, "qoq": 1}.get(node.name)
    if node.name == "lag":
        periods = _numeric(node.args[1])
    elif node.name == "cagr":
        periods = 4 * _numeric(node.args[1])
    if periods is None:
        raise AnomalyCatalogError(f"no quarter-grid span rule for {node.name!r}")
    previous = _Span(inner.kind, periods, inner.share_basis, inner.never, inner.split_gated)
    if node.name == "lag":
        return previous
    paired = _pair(inner, previous)
    if node.name == "avg2":
        return _Span("instant", 0, inner.share_basis, paired.never, paired.split_gated)
    # yoy/qoq/cagr publish a basis-free relative change over the pair's span
    # (qoq of two balances spans the quarter between them, like B - lag(B, 1)).
    return _Span(paired.kind, 0, False, paired.never, paired.split_gated)


def _reads_absence(node: Node) -> bool:
    """True for an expression holding ``x * 0``: the seed's presence-rule idiom.

    Presence rules turn a concept's absence into a zero (``coalesce(x * 0, y * 0 + 1)``
    is 1 exactly where ``x`` is untagged beside a tagged ``y``); no other seed
    definition multiplies by a literal zero.
    """
    if isinstance(node, BinOp):
        if node.op == "*" and any(isinstance(side, Number) and side.value == 0 for side in (node.left, node.right)):
            return True
        return _reads_absence(node.left) or _reads_absence(node.right)
    if isinstance(node, Neg):
        return _reads_absence(node.operand)
    if isinstance(node, Call):
        return any(_reads_absence(argument) for argument in node.args)
    return False


def _reads_unguarded_zero(node: Node) -> bool:
    """True for ``coalesce(<input>, <number>)``: a missing input read as a constant, unguarded."""
    if isinstance(node, Call):
        if node.name == "coalesce" and isinstance(node.args[0], Ref) and any(
                isinstance(argument, Number) for argument in node.args[1:]):
            return True
        return any(_reads_unguarded_zero(argument) for argument in node.args)
    if isinstance(node, BinOp):
        return _reads_unguarded_zero(node.left) or _reads_unguarded_zero(node.right)
    if isinstance(node, Neg):
        return _reads_unguarded_zero(node.operand)
    return False


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
    are false for every row: a one-quarter pair involving a trailing span or an
    instant after a flow; ``stdev_q`` over trailing spans; off-grid lags; a quarter
    and a trailing span of the same bucket. Split sensitivity is the engine's own
    formula-derived share exponent (per-share and share-count operands); since
    R1d a split-sensitive pair or window is never false by construction but
    ``split_gated``: comparable per row on one filing clock or proven split bases.
    Daily-window metrics are not span-labeled. ``reads_absence`` marks a metric
    whose expression or any input applies a presence rule (:func:`_reads_absence`).
    ``tests/test_research_metric_economics.py`` checks this mirror against the
    real engine's ``value_origin`` on a fixture of consecutive quarters, with and
    without the daily bars that prove the split basis.
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
            # An incomparable input makes the output incomparable (_derived_pit).
            refs[name] = _Span(span.kind, 0, metric_exponents[name] != 0,
                               f"incomparable_input:{name}" if shape.incomparable_reason else None,
                               shape.split_gated)
        for name in definition.market_inputs:
            quarters[name], sessions[name] = 0, 2 if name == "log_return" else 1
        history = _history(node, quarters, sessions)
        filing = bool(definition.item_inputs) or any(
            name in filing_clocked for name in definition.metric_inputs
        ) or any(name not in _BAR_ONLY_MARKET_COLUMNS for name in definition.market_inputs)
        if filing:
            filing_clocked.add(code)
        absence = _reads_absence(node) or any(shapes[name].reads_absence for name in definition.metric_inputs)
        unguarded = _reads_unguarded_zero(node) or any(
            shapes[name].reads_unguarded_zero for name in definition.metric_inputs)
        reason: str | None = None
        gated = False
        if definition.window in QUARTER_GRID_WINDOWS:
            # A value lives on a fiscal-quarter bucket even when a constant
            # fallback (e.g. ``coalesce(debt, 0)``) needs no input history.
            history = (max(history[0], 1), history[1])
            span = _span(node, refs, exponent_of)
            metric_spans[code] = span
            reason, gated = span.never, span.split_gated
            clock = CLOCK_FILING
        else:
            clock = CLOCK_MAX if filing else CLOCK_BAR
        shapes[code] = MetricShape(history[0], history[1], clock, reason, gated, absence, unguarded)
    return shapes


# ----------------------------------------------------------------------- validation


#: A legacy factor module's declaration: ``FACTOR_ID = "<id>"`` or a
#: ``FACTOR_IDS = ("<id>", ...)`` tuple at the start of a line.
_LEGACY_FACTOR_DECLARATION = re.compile(r'^FACTOR_IDS?\b[^=\n]*=\s*(\([^)]*\)|"[^"\n]*")', re.MULTILINE)


@lru_cache(maxsize=1)
def _legacy_module_factor_ids() -> frozenset[str]:
    """Factor ids declared by legacy factor modules (``FACTOR_ID``/``FACTOR_IDS``)."""
    paths = sorted(_PACKAGE.glob("*.py")) + sorted((_PACKAGE / "factors").glob("*.py"))
    ids: set[str] = set()
    for path in paths:
        for declaration in _LEGACY_FACTOR_DECLARATION.finditer(path.read_text(encoding="utf-8")):
            ids.update(re.findall(r'"([a-z][a-z0-9_]*)"', declaration.group(1)))
    return frozenset(ids)


@lru_cache(maxsize=1)
def _seed_factor_ids() -> frozenset[str]:
    ids: set[str] = set()
    for path in _FACTOR_SEEDS:
        with path.open("r", encoding="utf-8", newline="") as handle:
            ids.update((row["factor_id"] or "").strip() for row in csv.DictReader(handle))
    return frozenset(ids)


def _known_legacy_factor(factor_id: str) -> bool:
    """A factor id from the factor seeds or declared by a legacy factor module.

    Orientation is not restated here: the legacy seeds and modules carry each
    factor's own direction, which a parity check pairs with ``expected_sign``.
    """
    return factor_id in _seed_factor_ids() or factor_id in _legacy_module_factor_ids()


def _domain_errors(where: str, domain: str, metrics: Mapping[str, object], items: frozenset[str]) -> list[str]:
    """``<rule>`` or ``<rule>:<metric|item>:<code>`` with a known rule and a resolvable operand."""
    rule, _, operand = domain.partition(":")
    if rule not in DOMAIN_RULES:
        return [f"{where}: unknown domain rule {rule!r}"]
    if operand and rule not in _OPERAND_DOMAINS:
        return [f"{where}: domain rule {rule!r} takes no operand"]
    if not operand:
        return [f"{where}: domain rule {rule!r} requires an operand"] if rule in _OPERAND_REQUIRED_DOMAINS else []
    match = _OPERAND.fullmatch(operand)
    if match is None or (match.group(1) == "metric" and match.group(2) not in metrics) or (
            match.group(1) == "item" and match.group(2) not in items):
        return [f"{where}: domain operand {operand!r} is not a seed metric or fundamental item"]
    return []


def _external_shape(
    entry: AnomalyCatalogEntry, externals: Mapping[str, Mapping[str, str]], shapes: Mapping[str, MetricShape],
) -> tuple[MetricShape | None, list[str]]:
    """The inherited shape of a research-store source row, or the reasons it has none.

    The source feature must be declared by the feature store (``EXTERNAL_FEATURES``) with the
    row's window, and its clock and history in :data:`EXTERNAL_FEATURE_SHAPES`; a feature that
    re-clocks seed-metric states also inherits that metric's engine shape.
    """
    kind, code = entry.source_kind, entry.metric_code or ""
    window = externals.get(kind, {}).get(code)
    if window is None:
        return None, [f"unknown {kind} source feature {code!r} (research.features.EXTERNAL_FEATURES)"]
    problems: list[str] = []
    if entry.metric_window != window:
        problems.append(f"metric_window {entry.metric_window!r} but the {kind} window of {code!r} is {window!r}")
    if entry.feature_id != code:
        problems.append(f"a {kind} feature_id must equal its metric_code")
    declared = EXTERNAL_FEATURE_SHAPES.get((kind, code))
    if declared is None:
        return None, [*problems, f"no declared clock and history for the {kind} feature {code!r}"]
    state = _EXTERNAL_SEED_STATES.get((kind, code))
    if state is None:
        return declared, problems
    inner = shapes.get(state)
    if inner is None:
        return None, [*problems, f"re-clocks the seed metric {state!r}, which is not in the seed"]
    return MetricShape(max(declared.min_history_quarters, inner.min_history_quarters),
                       max(declared.min_history_sessions, inner.min_history_sessions), declared.availability_clock,
                       inner.incomparable_reason, inner.split_gated, inner.reads_absence,
                       inner.reads_unguarded_zero), problems


def _metadata_errors(entry: AnomalyCatalogEntry) -> list[str]:
    """CB2 research metadata: vocabularies, and the fields derived from evidence and reference."""
    problems: list[str] = []
    if entry.population not in POPULATIONS:
        problems.append(f"population must be one of {sorted(POPULATIONS)}, got {entry.population!r}")
    derived = derive_evidence_class(entry.prior_evidence, entry.expected_sign)
    if entry.evidence_class not in EVIDENCE_CLASSES:
        problems.append(f"evidence_class must be one of {sorted(EVIDENCE_CLASSES)}, got {entry.evidence_class!r}")
    elif entry.evidence_class != derived:
        problems.append(f"evidence_class {entry.evidence_class!r} but a {entry.prior_evidence} row with "
                        f"{_SIGN_LABELS.get(entry.expected_sign, entry.expected_sign)} sign is {derived!r}")
    if entry.prior_evidence in _PUBLISHED_EVIDENCE:
        year = reference_publication_year(entry.reference)
        if year is None:
            problems.append("a published row's reference must cite a publication year")
        elif entry.publication_year != year:
            problems.append(f"publication_year {entry.publication_year} but the earliest year the reference cites "
                            f"is {year}")
    elif entry.publication_year is not None:
        problems.append(f"publication_year must be empty for a {entry.prior_evidence} row")
    if entry.jkp_theme not in (*JKP_THEMES, JKP_THEME_NONE):
        problems.append(f"jkp_theme must be one of {[*JKP_THEMES, JKP_THEME_NONE]}, got {entry.jkp_theme!r}")
    if entry.wave not in WAVES:
        problems.append(f"wave must be one of {sorted(WAVES)}, got {entry.wave!r}")
    return problems


def validate_anomaly_catalog(
    entries: Iterable[AnomalyCatalogEntry],
    *,
    definitions: Iterable[DerivedMetricDefinition] | None = None,
    exclusions: Mapping[str, str] | None = None,
    natives: Mapping[str, Mapping[str, Any]] | None = None,
    externals: Mapping[str, Mapping[str, str]] | None = None,
) -> None:
    """Reject unknown codes, bad enumerations and any disagreement with the engine.

    ``natives`` defaults to ``research.panel.NATIVE_FEATURES`` and ``externals`` (research-
    store source kind -> source feature -> window) to ``research.features.EXTERNAL_FEATURES``
    (both imported here: the panel and the feature store import this module). All problems
    are collected and raised together as one :class:`AnomalyCatalogError`.
    """
    rows = tuple(entries)
    seed = default_derived_definitions() if definitions is None else tuple(definitions)
    excluded = EXCLUDED_SEED_METRICS if exclusions is None else exclusions
    if natives is None:
        from .panel import NATIVE_FEATURES

        natives = NATIVE_FEATURES
    if externals is None:
        from .features import EXTERNAL_FEATURES

        externals = EXTERNAL_FEATURES
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
        elif entry.source_kind == "panel_native":
            spec = natives.get(entry.metric_code or "")
            if spec is None:
                errors.append(f"{where}: unknown panel_native metric_code {entry.metric_code!r}")
            else:
                if entry.metric_window != spec["metric_window"]:
                    errors.append(f"{where}: metric_window {entry.metric_window!r} but the panel "
                                  f"window of {entry.metric_code!r} is {spec['metric_window']!r}")
                if entry.feature_id != entry.metric_code:
                    errors.append(f"{where}: a panel_native feature_id must equal its metric_code")
                sessions = spec.get("min_history_sessions")
                if not isinstance(sessions, int):
                    errors.append(f"{where}: panel native {entry.metric_code!r} declares no min_history_sessions")
                else:
                    # A native computed from bars alone keeps the bar clock; a share
                    # count from filings (DEI) makes it the latest of both clocks.
                    inputs = [str(name) for name in spec.get("inputs", ())]
                    bar_only = bool(inputs) and all(name.startswith(_BAR_INPUT_PREFIX) for name in inputs)
                    shape = MetricShape(0, sessions, CLOCK_BAR if bar_only else CLOCK_MAX, None)
            if entry.feature_id in by_code or entry.feature_id in item_codes:
                errors.append(f"{where}: panel_native id collides with a seed metric or item code")
            if entry.numerator or entry.denominator:
                errors.append(f"{where}: a panel_native row has no numerator/denominator")
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
                # The latest input clock: bar-only legs keep the bar clock.
                clocks = [CLOCK_FILING if kind == "item" else shapes[code].availability_clock
                          for kind, code in legs]
                clock = CLOCK_BAR if all(leg_clock == CLOCK_BAR for leg_clock in clocks) else CLOCK_MAX
                shape = MetricShape(quarters, sessions, clock,
                                    f"incomparable_input:{reasons[0]}" if reasons else None,
                                    any(leg.split_gated for leg in leg_shapes),
                                    any(leg.reads_absence for leg in leg_shapes),
                                    any(leg.reads_unguarded_zero for leg in leg_shapes))
        elif entry.source_kind in EXTERNAL_SOURCE_KINDS:
            shape, problems = _external_shape(entry, externals, shapes)
            errors.extend(f"{where}: {problem}" for problem in problems)
            if entry.feature_id in by_code or entry.feature_id in item_codes or entry.feature_id in natives:
                errors.append(f"{where}: {entry.source_kind} id collides with a seed metric, item or panel native code")
            if entry.numerator or entry.denominator:
                errors.append(f"{where}: a {entry.source_kind} row has no numerator/denominator")
            if entry.domain_operand:
                errors.append(f"{where}: a {entry.source_kind} row takes no domain operand")
        else:
            errors.append(f"{where}: source_kind must be one of {sorted(SOURCE_KINDS)}")
        if entry.anomaly_class not in ANOMALY_CLASSES + CONTROL_CLASSES:
            errors.append(f"{where}: unknown anomaly_class {entry.anomaly_class!r}")
        for field in ("economic_definition", "sign_rationale", "reference"):
            if not getattr(entry, field):
                errors.append(f"{where}: {field} must be non-empty")
        if entry.expected_sign not in (-1, 0, 1):
            errors.append(f"{where}: expected_sign must be -1, +1 or two_sided")
        if entry.prior_evidence not in PRIOR_EVIDENCE:
            errors.append(f"{where}: prior_evidence must be one of {sorted(PRIOR_EVIDENCE)}")
        if entry.is_two_sided and entry.prior_evidence == "published_anomaly":
            errors.append(f"{where}: a two_sided hypothesis cannot claim published_anomaly evidence")
        if entry.is_two_sided != ("mixed_evidence" in entry.caveat_codes):
            errors.append(f"{where}: expected_sign two_sided goes with caveat mixed_evidence and only with it")
        if not _FEATURE_ID.fullmatch(entry.hypothesis_family):
            errors.append(f"{where}: hypothesis_family must match {_FEATURE_ID.pattern}")
        errors.extend(_domain_errors(where, entry.domain, by_code, item_codes))
        # Domain rules and the caveats they answer come together.
        for caveat, rules in _CAVEAT_DOMAINS.items():
            if (caveat in entry.caveat_codes) != (entry.domain_rule in rules):
                errors.append(f"{where}: caveat {caveat} goes with a domain rule in {sorted(rules)} and only with one")
        if (entry.preferred_transform == "log_winsor_z") != (entry.domain_rule == "positive_value_required"):
            errors.append(f"{where}: log_winsor_z goes with domain positive_value_required and only with it")
        for code in entry.caveat_codes:
            if code not in CAVEAT_CODES:
                errors.append(f"{where}: unknown caveat_code {code!r}")
        if len(set(entry.caveat_codes)) != len(entry.caveat_codes):
            errors.append(f"{where}: duplicate caveat_code")
        if entry.scale_type not in SCALE_TYPES:
            errors.append(f"{where}: unknown scale_type {entry.scale_type!r}")
        if entry.preferred_transform not in PREFERRED_TRANSFORMS:
            errors.append(f"{where}: unknown preferred_transform {entry.preferred_transform!r}")
        elif (entry.preferred_transform == "log_winsor_z") != (entry.scale_type in LOG_SCALE_TYPES):
            errors.append(f"{where}: log_winsor_z is required for, and only for, scale types "
                          f"{sorted(LOG_SCALE_TYPES)}")
        if entry.admission not in ADMISSIONS:
            errors.append(f"{where}: admission must be one of {sorted(ADMISSIONS)}")
        else:
            if (entry.admission == "eligible") == bool(entry.admission_note):
                errors.append(f"{where}: admission_note is required unless admission is 'eligible'")
            if (entry.admission == "eligible") == bool(entry.caveat_codes):
                errors.append(f"{where}: caveat_code is required unless admission is 'eligible' "
                              "(and an eligible row has none)")
        for token in entry.supersedes:
            match = _SUPERSEDES.fullmatch(token)
            if match is None or not _known_legacy_factor(match.group(1)):
                errors.append(f"{where}: supersedes {token!r} is not a known legacy factor id")
            elif token in superseded:
                errors.append(f"{where}: {token!r} is already superseded by {superseded[token]!r}")
            else:
                superseded[token] = entry.feature_id
        if entry.availability_clock not in AVAILABILITY_CLOCKS:
            errors.append(f"{where}: unknown availability_clock {entry.availability_clock!r}")
        errors.extend(f"{where}: {problem}" for problem in _metadata_errors(entry))
        # One hypothesis is tested once: a duplicate construction names its primary.
        if (entry.admission == ADMISSION_DUPLICATE) != (entry.feature_id in DUPLICATE_HYPOTHESES):
            errors.append(f"{where}: admission {ADMISSION_DUPLICATE} goes with a DUPLICATE_HYPOTHESES row "
                          "and only with one")
        if ("duplicate_hypothesis" in entry.caveat_codes) != (entry.admission == ADMISSION_DUPLICATE):
            errors.append(f"{where}: caveat duplicate_hypothesis goes with admission {ADMISSION_DUPLICATE} "
                          "and only with it")
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
        if ("split_basis" in entry.caveat_codes) != shape.split_gated:
            errors.append(f"{where}: caveat split_basis goes with a share-basis comparison or window "
                          "(labeled per row since R1d) and only with one")
        if ("presence_rule" in entry.caveat_codes) != shape.reads_absence:
            errors.append(f"{where}: caveat presence_rule goes with a definition that reads a zero from a "
                          "concept's absence (itself or through an input) and only with one")
        if ("unguarded_zero" in entry.caveat_codes) != shape.reads_unguarded_zero:
            errors.append(f"{where}: caveat unguarded_zero goes with a definition that reads a missing input as "
                          "a constant without a presence guard (itself or through an input) and only with one")
        blocked = entry.admission == "blocked_incomparable_origin"
        if shape.incomparable_reason and not blocked:
            errors.append(f"{where}: every quarterly value is labeled incomparable "
                          f"({shape.incomparable_reason}); admission must be blocked_incomparable_origin")
        if blocked and not shape.incomparable_reason:
            errors.append(f"{where}: blocked_incomparable_origin but the engine can label it comparable")
    feature_ids = {entry.feature_id for entry in rows}
    by_id = {entry.feature_id: entry for entry in rows}
    for duplicate, primary in sorted(DUPLICATE_HYPOTHESES.items()):
        if duplicate not in by_id:
            continue
        target = by_id.get(primary)
        if target is None or not target.is_research_eligible:
            errors.append(f"duplicate hypothesis {duplicate!r}: its primary {primary!r} must be a research-eligible "
                          "catalog row")
        elif (target.hypothesis_family, target.expected_sign) != (
                by_id[duplicate].hypothesis_family, by_id[duplicate].expected_sign):
            errors.append(f"duplicate hypothesis {duplicate!r}: family and sign must equal its primary {primary!r}'s")
    for code, reason in sorted(excluded.items()):
        if code not in by_code:
            errors.append(f"exclusion {code!r} is not a seed metric")
        if code in cataloged:
            errors.append(f"seed metric {code!r} is both cataloged and excluded")
        kind, _, target = reason.partition(":")
        if (kind in ("dollar_level_input", "per_share_level", "presence_indicator") and not target) or (
                kind in _TARGETED_EXCLUSIONS and target in feature_ids):
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
    """Content digest for feature/evaluation manifests (R2b ``feature_version`` input).

    Line endings are normalized to LF first, so a CRLF checkout (``core.autocrlf``)
    or ``git archive`` export hashes the same as the committed blob.
    """
    content = Path(path).read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    return hashlib.sha256(content).hexdigest()


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
        "predicts higher 1-12 month forward returns; `two-sided` pre-registers no direction "
        "where published evidence disagrees (always with caveat `mixed_evidence` and never "
        "graded `published_anomaly`). Evaluation tests the sign; it never chooses it. "
        "`prior_evidence`: `published_anomaly` (the metric, or its standard "
        "construction, is a published anomaly with this sign), `published_analogue` (a close "
        "published relative; the sign is carried over), `economic_conjecture` (the sign is "
        "argued, not published). Qualification should treat conjectures as exploratory.",
        "",
        "`hypothesis_family` groups near-duplicates and same-construct variants (one "
        "economic hypothesis); multiple-testing and deduplication work over families, not rows.",
        "",
        "Admission: `eligible`; `eligible_with_caveat` (a known construction hazard, "
        "coded and noted per row); `blocked_incomparable_origin` (the derived engine labels every "
        "quarterly value `value_origin='incomparable'`, which the research gate rejects; "
        "not testable until the engine can prove comparability); `blocked_known_bias` (the "
        "engine can label values comparable but a known construction bias is noted per row); "
        "`blocked_duplicate_hypothesis` (a second construction of a cataloged hypothesis, cataloged for "
        "diagnostics: "
        + "; ".join(f"`{duplicate}` duplicates the tested `{primary}`"
                    for duplicate, primary in sorted(DUPLICATE_HYPOTHESES.items()))
        + ").",
        "",
        "`domain` is enforced by the feature store before any transform: an out-of-domain "
        "value is excluded or carried as the named separate indicator, never ranked as valid. "
        "A rule without an operand tests the feature value itself.",
        "",
        "Clocks are inherited, never declared freely: "
        + "; ".join(f"`{key}` = {value}" for key, value in AVAILABILITY_CLOCKS.items())
        + ". Minimum history counts fiscal quarters (`q`) and daily bars (`s`) needed for "
        "one value, derived from the metric's expression and its dependencies.",
        "",
        "Compositions are market-scaled ratios declared here and computed at formation by "
        "the feature store (R2b), clock = latest input clock.",
        "",
        "Panel natives (source `(daily, panel)`) are price/liquidity features the research panel "
        "computes from the line's own daily bars on XNYS session windows "
        "(`research.panel.NATIVE_FEATURES`); their minimum history is the panel's declared one "
        "and their clock is the bar clock, or the latest input clock when a filed share count is read.",
        "",
        "Research-store sources (source `(<window>, <kind>)`: `event` = P3 earnings events, "
        "`factor_exposure` = P4 factor exposures, `ownership` = P9 13F and FINRA features) are read by "
        "the feature store from one pinned sealed source version (`research.features.EXTERNAL_FEATURES`); "
        "their clock and minimum history are declared from each producer's rules "
        "(`EXTERNAL_FEATURE_SHAPES`).",
        "",
        "Research metadata (see the section below): `population`, `evidence_class`, "
        "`publication_year`, `jkp_theme` and `wave` on every row.",
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
            "| feature | source | sign | definition | reference | evidence | family | transform | domain "
            "| clock | history | admission |",
            "|---|---|:-:|---|---|---|---|---|---|---|---|---|",
        ]
        for entry in members:
            if entry.source_kind == "seed_metric":
                source = f"`{entry.metric_code}` ({entry.metric_window})"
            elif entry.source_kind == "panel_native":
                source = f"`{entry.metric_code}` ({entry.metric_window}, panel)"
            elif entry.source_kind in EXTERNAL_SOURCE_KINDS:
                source = f"`{entry.metric_code}` ({entry.metric_window}, {entry.source_kind})"
            else:
                source = f"`{entry.numerator}` / `{entry.denominator}`"
            admission = entry.admission
            if entry.caveat_codes:
                admission += f" [{', '.join(entry.caveat_codes)}]"
            if entry.admission_note:
                admission += f": {entry.admission_note}"
            lines.append(
                f"| `{entry.feature_id}` | {source} | {_SIGN_LABELS[entry.expected_sign]} "
                f"| {_cell(entry.economic_definition)} | {_cell(entry.reference)} "
                f"| {entry.prior_evidence} | {entry.hypothesis_family} | {entry.preferred_transform} "
                f"| {entry.domain} | {entry.availability_clock} "
                f"| {entry.min_history_quarters}q/{entry.min_history_sessions}s | {_cell(admission)} |"
            )
    families: dict[str, list[str]] = {}
    for entry in rows:
        families.setdefault(entry.hypothesis_family, []).append(entry.feature_id)
    lines += [
        "",
        "## Hypothesis families with more than one member",
        "",
        "| family | members |",
        "|---|---|",
    ]
    lines += [f"| {family} | {', '.join(f'`{member}`' for member in members)} |"
              for family, members in sorted(families.items()) if len(members) > 1]
    lines += ["", "## Domain rules", "", "| rule | meaning |", "|---|---|"]
    lines += [f"| `{rule}` | {meaning} |" for rule, meaning in DOMAIN_RULES.items()]
    lines += ["", "## Caveat codes", "", "| code | meaning |", "|---|---|"]
    lines += [f"| `{code}` | {meaning} |" for code, meaning in CAVEAT_CODES.items()]
    lines += _render_metadata(rows)
    lines += [
        "",
        "## Seed metrics that are not research features",
        "",
        "| metric | reason |",
        "|---|---|",
    ]
    lines += [f"| `{code}` | {reason} |" for code, reason in sorted(EXCLUDED_SEED_METRICS.items())]
    return "\n".join(lines) + "\n"


def _render_metadata(rows: Sequence[AnomalyCatalogEntry]) -> list[str]:
    """The research-metadata section: vocabularies, theme and wave counts, one line per row."""

    def counts(values: Iterable[str], key: Callable[[AnomalyCatalogEntry], str]) -> list[str]:
        return [f"| `{value}` | {sum(1 for e in rows if key(e) == value)} "
                f"| {sum(1 for e in rows if key(e) == value and e.is_research_eligible)} |" for value in values]

    lines = [
        "",
        "## Research metadata",
        "",
        "`population` names the firms a hypothesis is defined on (coverage is measured against it); "
        "`evidence_class` is derived: `replication` for a published anomaly or analogue with a "
        "pre-registered sign, `discovery` for an economic conjecture or a two-sided hypothesis; "
        "`publication_year` is the earliest year the reference cites (published rows only); "
        "`jkp_theme` is the Jensen-Kelly-Pedersen (2023) theme cluster of the JKP characteristic "
        "measuring the same construct, else the theme the construct belongs to, else `none`; "
        "`wave` is the pre-registration wave whose frozen catalog digest evaluates the row.",
        "",
        "| population | meaning |",
        "|---|---|",
    ]
    lines += [f"| `{name}` | {meaning} |" for name, meaning in POPULATIONS.items()]
    lines += ["", "| evidence class | meaning |", "|---|---|"]
    lines += [f"| `{name}` | {meaning} |" for name, meaning in EVIDENCE_CLASSES.items()]
    lines += ["", "| wave | meaning |", "|---|---|"]
    lines += [f"| `{name}` | {meaning} |" for name, meaning in WAVES.items()]
    lines += ["", "| JKP theme | rows | research-eligible |", "|---|---:|---:|"]
    lines += counts((*JKP_THEMES, JKP_THEME_NONE), lambda e: e.jkp_theme)
    lines += ["", "| wave | rows | research-eligible |", "|---|---:|---:|"]
    lines += counts(WAVES, lambda e: e.wave)
    lines += ["", "| feature | population | evidence class | publication year | JKP theme | wave |",
              "|---|---|---|---:|---|---|"]
    lines += [f"| `{e.feature_id}` | {e.population} | {e.evidence_class} | "
              f"{'' if e.publication_year is None else e.publication_year} | {e.jkp_theme} | {e.wave} |"
              for e in rows]
    return lines


_SIGN_LABELS: Mapping[int, str] = {1: "+1", -1: "-1", 0: "two-sided"}
