"""Definition-as-data factor catalog helpers.

S7 starts by reconciling the legacy ``features.py`` and ``alpha_research.py``
definitions into governed factor rows. The rows are intentionally plain data so
later S7 tasks can build the dependency DAG and engine on top.
"""

from __future__ import annotations

import json
import re
from dataclasses import asdict, dataclass
from typing import Iterable

import pandas as pd

from ..alpha_research import DEFAULT_ALPHA_SPECS, SOURCE_NAME as ALPHA_SOURCE_NAME
from ..features import (
    FEATURE_DEFINITIONS,
    FUNDAMENTAL_FEATURE_DEFINITIONS,
    FUNDAMENTAL_FEATURE_SOURCE_NAME,
    SOURCE_NAME as FEATURE_SOURCE_NAME,
)


DEFAULT_NEUTRALIZATION_SPEC = {"method": "none", "by": []}
DEFAULT_AVAILABLE_AT_POLICY = "Factor is available when all declared same-date inputs are available."
_FEATURE_TOKEN_RE = re.compile(r"\b[a-zA-Z_][a-zA-Z0-9_]*\b")


class CatalogValidationError(ValueError):
    """Raised when factor catalog rows reference undeclared inputs."""


@dataclass(frozen=True)
class FactorDefinition:
    factor_id: str
    factor_name: str
    family: str
    description: str
    expression: str
    input_ids_json: str
    direction: int
    lookback_days: int
    neutralization_spec_json: str
    unit: str
    sign: str
    scale: str
    is_point_in_time_safe: bool
    available_at_policy: str
    declared_in: str
    owner: str
    source: str

    @property
    def input_ids(self) -> tuple[str, ...]:
        values = json.loads(self.input_ids_json)
        if not isinstance(values, list):
            raise CatalogValidationError(f"{self.factor_id} input_ids_json must encode a JSON list")
        return tuple(str(value) for value in values)


def _json(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _feature_family(feature_id: str) -> str:
    if feature_id.startswith("ret_"):
        return "returns"
    if feature_id.startswith("mom_"):
        return "momentum"
    if feature_id.startswith("vol_"):
        return "volatility"
    if feature_id in {"adv_21d", "dollar_volume"}:
        return "liquidity"
    return "price"


def _fundamental_family(feature_id: str) -> str:
    if "growth" in feature_id:
        return "fundamental_growth"
    if "yield" in feature_id or "price_to" in feature_id or "book_to_market" in feature_id:
        return "fundamental_value"
    if "margin" in feature_id or "roa" in feature_id or "roe" in feature_id:
        return "fundamental_profitability"
    if "liabilities" in feature_id or "debt" in feature_id or "assets" in feature_id:
        return "fundamental_leverage"
    return "fundamental"


def _unit_for(feature_id: str, expression: str) -> str:
    lowered = f"{feature_id} {expression}".lower()
    if feature_id in {"adv_21d", "dollar_volume"}:
        return "currency"
    ratio_terms = ("_to_", "price_to", "book_to_market", "ratio", "yield", "margin", "growth", "return", "ret_", "mom_", "vol_", "roa", "roe")
    if any(token in lowered for token in ratio_terms):
        return "ratio"
    if "shares" in lowered:
        return "shares"
    currency_terms = (
        "revenue",
        "income",
        "cash_flow",
        "capex",
        "capital_expenditures",
        "dividends",
        "repurchases",
        "assets",
        "equity",
        "liabilities",
        "market_cap",
        "dollar_volume",
    )
    if any(token in lowered for token in currency_terms):
        return "currency"
    return "score"


def _sign_for(unit: str, feature_id: str) -> str:
    if unit in {"currency", "shares"}:
        return "nonnegative"
    if "rank" in feature_id or unit == "score":
        return "bounded"
    return "signed"


def _price_inputs(feature_id: str) -> tuple[str, ...]:
    if feature_id == "vol_21d":
        return ("factor:ret_1d",)
    return ("source:equity_daily_bars",)


def _fundamental_inputs(feature_id: str, expression: str) -> tuple[str, ...]:
    tokens = {
        token
        for token in _FEATURE_TOKEN_RE.findall(expression)
        if token.startswith("fund_") and token != feature_id
    }
    if tokens:
        return tuple(f"factor:{token}" for token in sorted(tokens))
    if "fundamental_ttm_points" in expression:
        return ("source:fundamental_ttm_points",)
    if "equity_daily_bars" in expression:
        return ("source:equity_daily_bars",)
    return ("source:fundamental_points",)


def _legacy_feature_rows() -> list[FactorDefinition]:
    rows: list[FactorDefinition] = []
    for feature_id, definition in sorted(FEATURE_DEFINITIONS.items()):
        expression = str(definition.get("expression_sql") or "")
        unit = _unit_for(feature_id, expression)
        rows.append(
            FactorDefinition(
                factor_id=feature_id,
                factor_name=feature_id,
                family=_feature_family(feature_id),
                description=str(definition.get("description") or feature_id),
                expression=expression,
                input_ids_json=_json(list(_price_inputs(feature_id))),
                direction=-1 if feature_id.startswith("vol_") else 1,
                lookback_days=int(definition.get("lookback_days") or 0),
                neutralization_spec_json=_json(DEFAULT_NEUTRALIZATION_SPEC),
                unit=unit,
                sign=_sign_for(unit, feature_id),
                scale="1",
                is_point_in_time_safe=True,
                available_at_policy=DEFAULT_AVAILABLE_AT_POLICY,
                declared_in="features.FEATURE_DEFINITIONS",
                owner="atx-impl",
                source=FEATURE_SOURCE_NAME,
            )
        )
    return rows


def _legacy_fundamental_rows() -> list[FactorDefinition]:
    rows: list[FactorDefinition] = []
    for feature_id, definition in sorted(FUNDAMENTAL_FEATURE_DEFINITIONS.items()):
        expression = str(definition.get("expression_sql") or "")
        unit = _unit_for(feature_id, expression)
        rows.append(
            FactorDefinition(
                factor_id=feature_id,
                factor_name=feature_id,
                family=_fundamental_family(feature_id),
                description=str(definition.get("description") or feature_id),
                expression=expression,
                input_ids_json=_json(list(_fundamental_inputs(feature_id, expression))),
                direction=-1 if any(token in feature_id for token in ("debt", "liabilities", "price_to")) else 1,
                lookback_days=int(definition.get("lookback_days") or 0),
                neutralization_spec_json=_json(DEFAULT_NEUTRALIZATION_SPEC),
                unit=unit,
                sign=_sign_for(unit, feature_id),
                scale="1",
                is_point_in_time_safe=True,
                available_at_policy=DEFAULT_AVAILABLE_AT_POLICY,
                declared_in="features.FUNDAMENTAL_FEATURE_DEFINITIONS",
                owner="atx-impl",
                source=FUNDAMENTAL_FEATURE_SOURCE_NAME,
            )
        )
    return rows


def _legacy_alpha_rows() -> list[FactorDefinition]:
    rows: list[FactorDefinition] = []
    for spec in sorted(DEFAULT_ALPHA_SPECS, key=lambda item: item.alpha_id):
        rows.append(
            FactorDefinition(
                factor_id=spec.alpha_id,
                factor_name=spec.alpha_name,
                family="alpha_research",
                description=spec.description,
                expression=spec.expression_sql,
                input_ids_json=_json([f"factor:{feature}" for feature in spec.input_features]),
                direction=int(spec.direction),
                lookback_days=0,
                neutralization_spec_json=_json({"method": "none", "by": [], "rank_method": "cross_section_percent_rank"}),
                unit="score",
                sign="signed",
                scale="zscore",
                is_point_in_time_safe=True,
                available_at_policy="Alpha is available when all same-date feature inputs are available.",
                declared_in="alpha_research.DEFAULT_ALPHA_SPECS",
                owner="atx-impl",
                source=ALPHA_SOURCE_NAME,
            )
        )
    return rows


def legacy_factor_definitions() -> tuple[FactorDefinition, ...]:
    """Return deterministic S7-0 factor rows derived from existing surfaces."""

    rows = [*_legacy_feature_rows(), *_legacy_fundamental_rows(), *_legacy_alpha_rows()]
    validate_catalog(rows)
    return tuple(rows)


def validate_catalog(
    rows: Iterable[FactorDefinition],
    *,
    known_metric_ids: Iterable[str] = (),
    known_source_ids: Iterable[str] = (
        "equity_daily_bars",
        "fundamental_points",
        "fundamental_ttm_points",
        "feature_values",
        "alpha_signal_values",
    ),
) -> None:
    """Validate that declared factor inputs resolve to factors, metrics, ratios, or sources."""

    materialized = tuple(rows)
    factor_ids = {row.factor_id for row in materialized}
    if len(factor_ids) != len(materialized):
        raise CatalogValidationError("factor_id values must be unique")
    metric_ids = set(known_metric_ids)
    source_ids = set(known_source_ids)
    errors: list[str] = []

    for row in materialized:
        if not row.family or not row.direction or not row.neutralization_spec_json:
            errors.append(f"{row.factor_id}: family, direction, and neutralization_spec_json are required")
        if not row.unit or not row.sign or not row.scale:
            errors.append(f"{row.factor_id}: unit, sign, and scale are required")
        for input_id in row.input_ids:
            kind, _, value = input_id.partition(":")
            if not value:
                if input_id not in factor_ids and input_id not in metric_ids:
                    errors.append(f"{row.factor_id}: unresolved input {input_id!r}")
                continue
            if kind == "factor" and value not in factor_ids:
                errors.append(f"{row.factor_id}: unresolved factor input {value!r}")
            elif kind in {"metric", "ratio"} and metric_ids and value not in metric_ids:
                errors.append(f"{row.factor_id}: unresolved {kind} input {value!r}")
            elif kind == "source" and value not in source_ids:
                errors.append(f"{row.factor_id}: unresolved source input {value!r}")
            elif kind not in {"factor", "metric", "ratio", "source"}:
                errors.append(f"{row.factor_id}: unsupported input namespace {kind!r}")

    if errors:
        raise CatalogValidationError("; ".join(errors))


def factor_definitions_frame(rows: Iterable[FactorDefinition] | None = None) -> pd.DataFrame:
    """Convert factor definitions to a stable DataFrame for migration seeds/tests."""

    materialized = tuple(legacy_factor_definitions() if rows is None else rows)
    return pd.DataFrame([asdict(row) for row in materialized]).sort_values("factor_id").reset_index(drop=True)
