"""Fundamental factor family definitions and pure compute helpers."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

import pandas as pd

from ..warehouse import json_dumps
from .catalog import FactorDefinition
from .cross_section import zscore


SOURCE_NAME = "atx-impl fundamental factor families"
SEED_PATH = Path(__file__).resolve().parents[1] / "seeds" / "factor_definitions.csv"
FUNDAMENTAL_FACTOR_COLUMNS = [
    "factor_value_id",
    "factor_id",
    "factor_name",
    "family",
    "security_id",
    "symbol",
    "as_of_date",
    "raw_value",
    "value",
    "available_at",
    "input_ids_json",
    "input_lineage_json",
    "is_latest_revision",
    "run_id",
    "source",
]


@dataclass(frozen=True)
class FundamentalFactorSpec:
    factor_id: str
    factor_name: str
    family: str
    description: str
    expression: str
    input_metric_codes: tuple[str, ...]
    standardization: str
    neutralization_spec_json: str
    direction: int
    lookback_days: int
    unit: str
    sign: str
    scale: str
    valid_from: dt.date
    valid_to: dt.date | None

    @property
    def input_ids_json(self) -> str:
        return json_dumps([f"metric:{metric}" for metric in self.input_metric_codes])

    def to_factor_definition(self) -> FactorDefinition:
        return FactorDefinition(
            factor_id=self.factor_id,
            factor_name=self.factor_name,
            family=self.family,
            description=self.description,
            expression=self.expression,
            input_ids_json=self.input_ids_json,
            direction=self.direction,
            lookback_days=self.lookback_days,
            neutralization_spec_json=self.neutralization_spec_json,
            unit=self.unit,
            sign=self.sign,
            scale=self.scale,
            is_point_in_time_safe=True,
            available_at_policy="Factor is available when all source metric inputs are visible as-of.",
            declared_in="db/seeds/factor_definitions.csv",
            owner="atx-impl",
            source=SOURCE_NAME,
        )


def _as_date(value: object) -> dt.date | None:
    if value is None or value == "":
        return None
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    return pd.Timestamp(value).date()


def _parse_json_list(value: str) -> tuple[str, ...]:
    loaded = json.loads(value)
    if not isinstance(loaded, list):
        raise ValueError("input_metric_codes must encode a JSON list")
    return tuple(str(item) for item in loaded)


def load_factor_seed_specs(seed_path: str | Path = SEED_PATH) -> tuple[FundamentalFactorSpec, ...]:
    """Load S8 factor-definition seed rows as typed specs."""

    frame = pd.read_csv(seed_path, keep_default_na=False)
    specs: list[FundamentalFactorSpec] = []
    required = {
        "factor_id",
        "factor_name",
        "family",
        "description",
        "expression",
        "input_metric_codes",
        "standardization",
        "neutralization",
        "direction",
        "lookback_days",
        "unit",
        "sign",
        "scale",
        "valid_from",
        "valid_to",
    }
    missing = sorted(required - set(frame.columns))
    if missing:
        raise ValueError(f"factor definition seed missing columns: {missing}")
    for row in frame.to_dict(orient="records"):
        specs.append(
            FundamentalFactorSpec(
                factor_id=str(row["factor_id"]).strip(),
                factor_name=str(row["factor_name"]).strip(),
                family=str(row["family"]).strip(),
                description=str(row["description"]).strip(),
                expression=str(row["expression"]).strip(),
                input_metric_codes=_parse_json_list(str(row["input_metric_codes"])),
                standardization=str(row["standardization"]).strip(),
                neutralization_spec_json=str(row["neutralization"]).strip(),
                direction=int(row["direction"]),
                lookback_days=int(row["lookback_days"]),
                unit=str(row["unit"]).strip(),
                sign=str(row["sign"]).strip(),
                scale=str(row["scale"]).strip(),
                valid_from=_as_date(row["valid_from"]) or dt.date(1900, 1, 1),
                valid_to=_as_date(row["valid_to"]),
            )
        )
    factor_ids = [spec.factor_id for spec in specs]
    if len(set(factor_ids)) != len(factor_ids):
        raise ValueError("factor definition seed has duplicate factor_id values")
    return tuple(sorted(specs, key=lambda spec: spec.factor_id))


def factor_seed_definitions(seed_path: str | Path = SEED_PATH) -> tuple[FactorDefinition, ...]:
    return tuple(spec.to_factor_definition() for spec in load_factor_seed_specs(seed_path))


def factor_seed_frame(seed_path: str | Path = SEED_PATH) -> pd.DataFrame:
    specs = load_factor_seed_specs(seed_path)
    return pd.DataFrame(
        [
            {
                "factor_id": spec.factor_id,
                "factor_name": spec.factor_name,
                "family": spec.family,
                "description": spec.description,
                "expression": spec.expression,
                "input_ids_json": spec.input_ids_json,
                "direction": spec.direction,
                "lookback_days": spec.lookback_days,
                "neutralization_spec_json": spec.neutralization_spec_json,
                "unit": spec.unit,
                "sign": spec.sign,
                "scale": spec.scale,
                "is_point_in_time_safe": True,
                "available_at_policy": "Factor is available when all source metric inputs are visible as-of.",
                "declared_in": "db/seeds/factor_definitions.csv",
                "owner": "atx-impl",
                "source": SOURCE_NAME,
                "standardization_spec_json": json_dumps({"method": spec.standardization}),
                "valid_from": spec.valid_from,
                "valid_to": spec.valid_to,
            }
            for spec in specs
        ]
    )


def _parse_expression(expression: str) -> dict[str, str]:
    parsed: dict[str, str] = {}
    for part in expression.split("|"):
        if ":" not in part:
            continue
        key, value = part.split(":", 1)
        parsed[key.strip()] = value.strip()
    return parsed


def _hash_id(prefix: str, *parts: object) -> str:
    payload = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(f"{prefix}|{payload}".encode("utf-8")).hexdigest()


def _normalize_metric_frame(metrics: pd.DataFrame) -> pd.DataFrame:
    if metrics is None or metrics.empty:
        return pd.DataFrame()
    out = metrics.copy()
    defaults: dict[str, object] = {
        "symbol": pd.NA,
        "metric_row_id": pd.NA,
        "formula_code": pd.NA,
        "standardization_rule_id": pd.NA,
        "vintage_class": "most_recently_restated",
        "source_table": "metric_fixture",
        "input_lineage_json": pd.NA,
    }
    for column, value in defaults.items():
        if column not in out.columns:
            out[column] = value
    out["metric_code"] = out["metric_code"].astype("string")
    out["security_id"] = out["security_id"].astype("string")
    out["as_of_date"] = pd.to_datetime(out["as_of_date"], errors="coerce").dt.date
    out["available_at"] = pd.to_datetime(out["available_at"], errors="coerce")
    out["value"] = pd.to_numeric(out["value"], errors="coerce")
    out = out.dropna(subset=["metric_code", "security_id", "as_of_date", "available_at", "value"])
    return out.reset_index(drop=True)


def _leg(row: pd.Series) -> dict[str, Any]:
    return {
        "metric_code": str(row["metric_code"]),
        "metric_row_id": None if pd.isna(row.get("metric_row_id")) else str(row.get("metric_row_id")),
        "source_table": None if pd.isna(row.get("source_table")) else str(row.get("source_table")),
        "formula_code": None if pd.isna(row.get("formula_code")) else str(row.get("formula_code")),
        "standardization_rule_id": None
        if pd.isna(row.get("standardization_rule_id"))
        else str(row.get("standardization_rule_id")),
        "vintage_class": None if pd.isna(row.get("vintage_class")) else str(row.get("vintage_class")),
        "as_of_date": row["as_of_date"].isoformat(),
        "available_at": pd.Timestamp(row["available_at"]).isoformat(),
        "value": float(row["value"]),
    }


def _raw_value(expression: str, values: dict[str, float]) -> float | None:
    parsed = _parse_expression(expression)
    method = parsed.get("method")
    if method == "identity":
        return values.get(parsed["metric"])
    if method == "divide":
        denominator = values.get(parsed["denominator"])
        numerator = values.get(parsed["numerator"])
        if denominator is None or numerator is None or denominator == 0:
            return None
        return numerator / denominator
    if method == "spread_scaled":
        scale = values.get(parsed["scale"])
        left = values.get(parsed["left"])
        right = values.get(parsed["right"])
        if scale is None or left is None or right is None or scale == 0:
            return None
        return (left - right) / scale
    raise ValueError(f"Unsupported factor expression: {expression!r}")


def compute_fundamental_factor_rows(
    metrics: pd.DataFrame,
    *,
    specs: Iterable[FundamentalFactorSpec] | None = None,
    families: Iterable[str] | None = None,
    run_id: str | None = None,
    source: str = SOURCE_NAME,
) -> pd.DataFrame:
    """Compute S8 fundamental family rows from PIT metric observations."""

    metric_frame = _normalize_metric_frame(metrics)
    if metric_frame.empty:
        return pd.DataFrame(columns=FUNDAMENTAL_FACTOR_COLUMNS)
    selected_specs = tuple(load_factor_seed_specs() if specs is None else specs)
    family_filter = set(families or ())
    if family_filter:
        selected_specs = tuple(spec for spec in selected_specs if spec.family in family_filter)

    rows: list[dict[str, object]] = []
    grouped = metric_frame.groupby(["security_id", "as_of_date"], sort=True, dropna=False)
    for (security_id, as_of_date), group in grouped:
        latest = (
            group.sort_values(["metric_code", "available_at", "metric_row_id"], kind="mergesort")
            .groupby("metric_code", as_index=False)
            .tail(1)
        )
        by_metric = {str(row.metric_code): row for row in latest.itertuples(index=False)}
        for spec in selected_specs:
            if any(metric not in by_metric for metric in spec.input_metric_codes):
                continue
            input_rows = [by_metric[metric] for metric in spec.input_metric_codes]
            values = {metric: float(by_metric[metric].value) for metric in spec.input_metric_codes}
            raw = _raw_value(spec.expression, values)
            if raw is None or pd.isna(raw):
                continue
            available_at = max(pd.Timestamp(row.available_at) for row in input_rows)
            symbol = next((row.symbol for row in input_rows if not pd.isna(row.symbol)), pd.NA)
            lineage = [_leg(pd.Series(row._asdict())) for row in input_rows]
            rows.append(
                {
                    "factor_value_id": _hash_id("fundamental_factor", spec.factor_id, security_id, as_of_date, available_at, run_id),
                    "factor_id": spec.factor_id,
                    "factor_name": spec.factor_name,
                    "family": spec.family,
                    "security_id": str(security_id),
                    "symbol": symbol,
                    "as_of_date": as_of_date,
                    "raw_value": float(raw),
                    "available_at": available_at,
                    "input_ids_json": spec.input_ids_json,
                    "input_lineage_json": json_dumps(lineage),
                    "is_latest_revision": True,
                    "run_id": run_id,
                    "source": source,
                }
            )
    if not rows:
        return pd.DataFrame(columns=FUNDAMENTAL_FACTOR_COLUMNS)
    frame = pd.DataFrame(rows)
    standardized = zscore(
        frame.rename(columns={"raw_value": "value"}),
        value_column="value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    frame["value"] = standardized["value"]
    return frame[FUNDAMENTAL_FACTOR_COLUMNS].sort_values(
        ["family", "factor_id", "as_of_date", "security_id"], kind="mergesort"
    ).reset_index(drop=True)
