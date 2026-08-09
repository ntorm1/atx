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


SOURCE_NAME = "atx-db fundamental factor families"
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
    stage: str

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
            owner="atx-db",
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


def load_factor_seed_specs(
    seed_path: str | Path = SEED_PATH,
    *,
    stages: Iterable[str] | None = None,
) -> tuple[FundamentalFactorSpec, ...]:
    """Load S8 factor-definition seed rows as typed specs."""

    frame = pd.read_csv(seed_path, keep_default_na=False)
    stage_filter = {str(stage) for stage in (stages or ())}
    if stage_filter and "stage" in frame.columns:
        frame = frame[frame["stage"].astype(str).isin(stage_filter)]
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
        "stage",
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
                stage=str(row["stage"]).strip(),
            )
        )
    factor_ids = [spec.factor_id for spec in specs]
    if len(set(factor_ids)) != len(factor_ids):
        raise ValueError("factor definition seed has duplicate factor_id values")
    return tuple(sorted(specs, key=lambda spec: spec.factor_id))


def factor_seed_definitions(
    seed_path: str | Path = SEED_PATH,
    *,
    stages: Iterable[str] | None = None,
) -> tuple[FactorDefinition, ...]:
    return tuple(spec.to_factor_definition() for spec in load_factor_seed_specs(seed_path, stages=stages))


def factor_seed_frame(
    seed_path: str | Path = SEED_PATH,
    *,
    stages: Iterable[str] | None = None,
) -> pd.DataFrame:
    specs = load_factor_seed_specs(seed_path, stages=stages)
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
                "owner": "atx-db",
                "source": SOURCE_NAME,
                "standardization_spec_json": json_dumps({"method": spec.standardization}),
                "valid_from": spec.valid_from,
                "valid_to": spec.valid_to,
                "stage": spec.stage,
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
    if method == "piotroski_f":
        assets = values.get("assets")
        if assets is None or assets == 0:
            return None
        roa = values.get("net_income", 0.0) / assets
        cfo = values.get("operating_cash_flow")
        net_income = values.get("net_income")
        if cfo is None or net_income is None:
            return None
        signals = (
            roa > 0,
            cfo > 0,
            values.get("roa_yoy_change", 0.0) > 0,
            cfo > net_income,
            values.get("debt_to_assets_change", 0.0) < 0,
            values.get("current_ratio_change", 0.0) > 0,
            values.get("shares_outstanding_growth", 0.0) <= 0,
            values.get("gross_margin_change", 0.0) > 0,
            values.get("asset_turnover_change", 0.0) > 0,
        )
        return float(sum(1 for signal in signals if signal))
    if method == "altman_z":
        assets = values.get("assets")
        total_debt = values.get("total_debt")
        if assets is None or total_debt is None or assets == 0 or total_debt == 0:
            return None
        return (
            1.2 * (values.get("working_capital", 0.0) / assets)
            + 1.4 * (values.get("retained_earnings", 0.0) / assets)
            + 3.3 * (values.get("ebit", 0.0) / assets)
            + 0.6 * (values.get("market_cap", 0.0) / total_debt)
            + 1.0 * (values.get("revenue", 0.0) / assets)
        )
    if method == "sloan_accruals":
        average_assets = values.get("average_assets")
        if average_assets is None or average_assets == 0:
            return None
        accruals = (
            values.get("delta_current_assets", 0.0)
            - values.get("delta_cash_and_equivalents", 0.0)
            - values.get("delta_current_liabilities", 0.0)
            + values.get("delta_short_term_debt", 0.0)
            - values.get("depreciation_expense", 0.0)
        )
        return accruals / average_assets
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


def _standardize_rows(frame: pd.DataFrame) -> pd.DataFrame:
    if frame.empty:
        return frame
    standardized = zscore(
        frame.rename(columns={"raw_value": "value"}),
        value_column="value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    frame = frame.copy()
    frame["value"] = standardized["value"]
    return frame[FUNDAMENTAL_FACTOR_COLUMNS].sort_values(
        ["family", "factor_id", "as_of_date", "security_id"], kind="mergesort"
    ).reset_index(drop=True)


def _native_factor_id(factor_id: str, security_id: str, as_of_date: object, available_at: object, run_id: str | None) -> str:
    return _hash_id("signal_native_factor", factor_id, security_id, as_of_date, available_at, run_id)


def _native_lineage(records: list[dict[str, Any]]) -> str:
    return json_dumps(records)


def compute_signal_native_factor_rows(
    *,
    revisions: pd.DataFrame | None = None,
    standardization_deltas: pd.DataFrame | None = None,
    segments: pd.DataFrame | None = None,
    footnotes: pd.DataFrame | None = None,
    run_id: str | None = None,
    source: str = SOURCE_NAME,
) -> pd.DataFrame:
    """Compute signal-native factors from PIT/vintage warehouse fixtures."""

    rows: list[dict[str, object]] = []

    if revisions is not None and not revisions.empty:
        rev = revisions.copy()
        rev["as_of_date"] = pd.to_datetime(rev["as_of_date"], errors="coerce").dt.date
        rev["available_at"] = pd.to_datetime(rev["available_at"], errors="coerce")
        rev["value"] = pd.to_numeric(rev["value"], errors="coerce")
        rev = rev.dropna(subset=["security_id", "metric_code", "as_of_date", "available_at", "value"])
        rev = rev[rev["available_at"].dt.date <= rev["as_of_date"]]
        for (security_id, as_of_date), group in rev.groupby(["security_id", "as_of_date"], sort=True):
            group = group.sort_values(["available_at", "metric_code"], kind="mergesort")
            if len(group) < 2:
                continue
            first = float(group.iloc[0]["value"])
            last = float(group.iloc[-1]["value"])
            if first == 0:
                continue
            available_at = pd.Timestamp(group["available_at"].max())
            symbol = group["symbol"].dropna().iloc[0] if "symbol" in group.columns and not group["symbol"].dropna().empty else pd.NA
            lineage = [
                {
                    "metric_code": str(row.metric_code),
                    "available_at": pd.Timestamp(row.available_at).isoformat(),
                    "value": float(row.value),
                    "vintage_class": getattr(row, "vintage_class", None),
                }
                for row in group.itertuples(index=False)
            ]
            rows.append(
                {
                    "factor_value_id": _native_factor_id("signal_revision_momentum", security_id, as_of_date, available_at, run_id),
                    "factor_id": "signal_revision_momentum",
                    "factor_name": "PIT revisions momentum",
                    "family": "signal_native",
                    "security_id": str(security_id),
                    "symbol": symbol,
                    "as_of_date": as_of_date,
                    "raw_value": (last - first) / abs(first),
                    "available_at": available_at,
                    "input_ids_json": json_dumps(["metric:revision_vintages"]),
                    "input_lineage_json": _native_lineage(lineage),
                    "is_latest_revision": True,
                    "run_id": run_id,
                    "source": source,
                }
            )

    if standardization_deltas is not None and not standardization_deltas.empty:
        std = standardization_deltas.copy()
        std["as_of_date"] = pd.to_datetime(std["as_of_date"], errors="coerce").dt.date
        std["available_at"] = pd.to_datetime(std["available_at"], errors="coerce")
        std["raw_value"] = pd.to_numeric(std["raw_value"], errors="coerce")
        std["standardized_value"] = pd.to_numeric(std["standardized_value"], errors="coerce")
        std = std.dropna(subset=["security_id", "as_of_date", "available_at", "raw_value", "standardized_value"])
        for row in std.itertuples(index=False):
            if float(row.raw_value) == 0:
                continue
            raw_value = (float(row.standardized_value) - float(row.raw_value)) / abs(float(row.raw_value))
            lineage = [
                {
                    "raw_value": float(row.raw_value),
                    "standardized_value": float(row.standardized_value),
                    "standardization_rule_id": getattr(row, "standardization_rule_id", None),
                    "available_at": pd.Timestamp(row.available_at).isoformat(),
                }
            ]
            rows.append(
                {
                    "factor_value_id": _native_factor_id("signal_standardization_delta", row.security_id, row.as_of_date, row.available_at, run_id),
                    "factor_id": "signal_standardization_delta",
                    "factor_name": "Standardization-delta anomaly",
                    "family": "signal_native",
                    "security_id": str(row.security_id),
                    "symbol": getattr(row, "symbol", pd.NA),
                    "as_of_date": row.as_of_date,
                    "raw_value": raw_value,
                    "available_at": pd.Timestamp(row.available_at),
                    "input_ids_json": json_dumps(["metric:standardization_delta"]),
                    "input_lineage_json": _native_lineage(lineage),
                    "is_latest_revision": True,
                    "run_id": run_id,
                    "source": source,
                }
            )

    if segments is not None and not segments.empty:
        seg = segments.copy()
        seg["as_of_date"] = pd.to_datetime(seg["as_of_date"], errors="coerce").dt.date
        seg["available_at"] = pd.to_datetime(seg["available_at"], errors="coerce")
        seg["segment_revenue"] = pd.to_numeric(seg["segment_revenue"], errors="coerce")
        seg = seg.dropna(subset=["security_id", "as_of_date", "available_at", "segment_revenue"])
        for (security_id, as_of_date), group in seg.groupby(["security_id", "as_of_date"], sort=True):
            total = float(group["segment_revenue"].sum())
            if total <= 0:
                continue
            shares = group["segment_revenue"] / total
            available_at = pd.Timestamp(group["available_at"].max())
            symbol = group["symbol"].dropna().iloc[0] if "symbol" in group.columns and not group["symbol"].dropna().empty else pd.NA
            lineage = [
                {
                    "segment": getattr(row, "segment", None),
                    "segment_revenue": float(row.segment_revenue),
                    "available_at": pd.Timestamp(row.available_at).isoformat(),
                }
                for row in group.itertuples(index=False)
            ]
            rows.append(
                {
                    "factor_value_id": _native_factor_id("signal_segment_revenue_concentration", security_id, as_of_date, available_at, run_id),
                    "factor_id": "signal_segment_revenue_concentration",
                    "factor_name": "Segment revenue concentration",
                    "family": "signal_native",
                    "security_id": str(security_id),
                    "symbol": symbol,
                    "as_of_date": as_of_date,
                    "raw_value": float((shares**2).sum()),
                    "available_at": available_at,
                    "input_ids_json": json_dumps(["metric:segment_revenue"]),
                    "input_lineage_json": _native_lineage(lineage),
                    "is_latest_revision": True,
                    "run_id": run_id,
                    "source": source,
                }
            )

    if footnotes is not None and not footnotes.empty:
        foot = footnotes.copy()
        foot["as_of_date"] = pd.to_datetime(foot["as_of_date"], errors="coerce").dt.date
        foot["available_at"] = pd.to_datetime(foot["available_at"], errors="coerce")
        foot["footnote_count"] = pd.to_numeric(foot["footnote_count"], errors="coerce")
        foot["prior_footnote_count"] = pd.to_numeric(foot["prior_footnote_count"], errors="coerce")
        foot = foot.dropna(subset=["security_id", "as_of_date", "available_at", "footnote_count", "prior_footnote_count"])
        for row in foot.itertuples(index=False):
            if float(row.prior_footnote_count) == 0:
                continue
            raw_value = (float(row.footnote_count) - float(row.prior_footnote_count)) / abs(float(row.prior_footnote_count))
            lineage = [
                {
                    "footnote_count": float(row.footnote_count),
                    "prior_footnote_count": float(row.prior_footnote_count),
                    "available_at": pd.Timestamp(row.available_at).isoformat(),
                }
            ]
            rows.append(
                {
                    "factor_value_id": _native_factor_id("signal_footnote_disclosure_change", row.security_id, row.as_of_date, row.available_at, run_id),
                    "factor_id": "signal_footnote_disclosure_change",
                    "factor_name": "Footnote disclosure change",
                    "family": "signal_native",
                    "security_id": str(row.security_id),
                    "symbol": getattr(row, "symbol", pd.NA),
                    "as_of_date": row.as_of_date,
                    "raw_value": raw_value,
                    "available_at": pd.Timestamp(row.available_at),
                    "input_ids_json": json_dumps(["metric:footnote_disclosure_count"]),
                    "input_lineage_json": _native_lineage(lineage),
                    "is_latest_revision": True,
                    "run_id": run_id,
                    "source": source,
                }
            )

    if not rows:
        return pd.DataFrame(columns=FUNDAMENTAL_FACTOR_COLUMNS)
    return _standardize_rows(pd.DataFrame(rows))


def fundamental_factor_lineage_completeness(frame: pd.DataFrame) -> dict[str, object]:
    """Return a gate-ready lineage completeness report for emitted factor rows."""

    if frame is None or frame.empty:
        return {"status": "warning", "total_count": 0, "incomplete_count": 0, "examples": []}
    incomplete: list[dict[str, object]] = []
    for row in frame.itertuples(index=False):
        payload = getattr(row, "input_lineage_json", None)
        ok = bool(payload)
        if ok:
            try:
                lineage = json.loads(payload)
                ok = isinstance(lineage, list) and bool(lineage)
            except (TypeError, json.JSONDecodeError):
                ok = False
        if not ok:
            incomplete.append(
                {
                    "factor_id": getattr(row, "factor_id", None),
                    "security_id": getattr(row, "security_id", None),
                    "as_of_date": str(getattr(row, "as_of_date", None)),
                }
            )
    return {
        "status": "passed" if not incomplete else "failed",
        "total_count": int(len(frame)),
        "incomplete_count": int(len(incomplete)),
        "examples": incomplete[:20],
    }


def fundamental_factor_family_coverage(
    frame: pd.DataFrame,
    *,
    expected_families: Iterable[str] | None = None,
) -> dict[str, object]:
    """Return coverage status for expected fundamental factor families."""

    expected = set(
        expected_families
        or {
            "fundamental_value",
            "fundamental_quality",
            "fundamental_profitability",
            "fundamental_growth",
            "fundamental_investment",
            "fundamental_leverage",
            "fundamental_distress",
            "fundamental_accruals",
            "signal_native",
        }
    )
    present = set() if frame is None or frame.empty else {str(value) for value in frame["family"].dropna().unique()}
    missing = sorted(expected - present)
    counts = (
        {}
        if frame is None or frame.empty
        else {str(key): int(value) for key, value in frame.groupby("family")["factor_id"].nunique().items()}
    )
    return {
        "status": "passed" if not missing else "failed",
        "expected_families": sorted(expected),
        "missing_families": missing,
        "family_factor_counts": counts,
    }
