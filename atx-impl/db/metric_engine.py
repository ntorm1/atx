"""PF3-S6 S6-1: first-class PIT fundamental growth metrics.

The ratio engine still emits its legacy YoY ``growth`` rows for compatibility.
This module materializes the broader growth surface requested for PF3-S6:
YoY, QoQ, multi-year CAGR, and trailing growth stability/consistency over
fundamental period history.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Any

import pandas as pd

from .connection import DEFAULT_DB_PATH, DuckDBStore, connect
from .dataset import Dataset, DatasetLoadResult
from .formula_library import (
    REQUIRE_POSITIVE_DENOMINATOR_RULE,
    SEED_PATH,
    FormulaRegistrySeedRow,
    load_ratio_formula_rows,
)
from .warehouse import insert_frame, json_dumps, quality_check


SOURCE_NAME = "Derived point-in-time fundamental growth"
DEFAULT_SOURCE = "derived_fundamental_growth_v1"

FUNDAMENTAL_GROWTH_COLUMNS = [
    "growth_id",
    "source",
    "security_id",
    "symbol",
    "cik",
    "formula_code",
    "family",
    "kind",
    "unit",
    "basis",
    "growth_method",
    "horizon_years",
    "elapsed_years",
    "period_start",
    "period_end",
    "base_period_start",
    "base_period_end",
    "fiscal_year",
    "fiscal_period",
    "value",
    "current_code",
    "current_value",
    "base_code",
    "base_value",
    "is_meaningful",
    "is_latest_revision",
    "vintage_class",
    "current_available_at",
    "base_available_at",
    "available_at",
    "as_of_date",
    "current_source_accession",
    "base_source_accession",
    "current_filed_date",
    "base_filed_date",
    "input_codes_json",
    "input_lineage_json",
    "run_id",
]

TTM_GROWTH_METRICS = {
    "revenue",
    "net_income",
    "operating_income",
    "operating_cash_flow",
    "capital_expenditures",
}
BALANCE_GROWTH_METRICS = {"assets", "stockholders_equity"}
DERIVED_TTM_GROWTH_METRICS = {"free_cash_flow"}
GROWTH_METRIC_BASIS = {
    **{metric: "ttm" for metric in TTM_GROWTH_METRICS | DERIVED_TTM_GROWTH_METRICS},
    **{metric: "instant" for metric in BALANCE_GROWTH_METRICS},
}


@dataclass(frozen=True)
class GrowthFormulaSpec:
    formula_code: str
    family: str
    kind: str
    unit: str
    metric_code: str
    basis: str
    growth_method: str
    horizon_years: float
    transform: str
    require_positive_base: bool = False
    min_observations: int = 2


@dataclass(frozen=True)
class FundamentalGrowthOptions:
    source: str = DEFAULT_SOURCE
    symbols: tuple[str, ...] | None = None
    metrics: tuple[str, ...] | None = None
    run_id: str | None = None


def _present(value: Any) -> bool:
    try:
        return not pd.isna(value)
    except (TypeError, ValueError):
        return value is not None


def _safe_float(value: Any) -> float | None:
    if not _present(value):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _as_date(value: Any) -> dt.date | None:
    if not _present(value):
        return None
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    ts = pd.Timestamp(value)
    return None if pd.isna(ts) else ts.date()


def _as_timestamp(value: Any) -> pd.Timestamp | None:
    if not _present(value):
        return None
    ts = pd.Timestamp(value)
    return None if pd.isna(ts) else ts


def _iso(value: Any) -> str | None:
    if not _present(value):
        return None
    if isinstance(value, pd.Timestamp):
        return None if pd.isna(value) else value.isoformat()
    if isinstance(value, (dt.datetime, dt.date)):
        return value.isoformat()
    return str(value)


def _growth_id(
    source: str,
    security_id: str,
    formula_code: str,
    basis: str,
    period_end: Any,
    base_period_end: Any,
    available_at: Any,
) -> str:
    payload = "|".join(
        str(part)
        for part in (source, security_id, formula_code, basis, period_end, base_period_end, available_at)
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _parse_expression(expression: str | None) -> dict[str, str]:
    if not expression:
        return {}
    parsed: dict[str, str] = {}
    for part in expression.split("|"):
        if ":" not in part:
            continue
        key, value = part.split(":", 1)
        parsed[key.strip()] = value.strip()
    return parsed


def _infer_method(formula_code: str, transform: str, expression: dict[str, str]) -> str:
    if "mode" in expression:
        return expression["mode"]
    if formula_code.endswith("_growth_yoy"):
        return "yoy"
    if formula_code.endswith("_growth_qoq"):
        return "qoq"
    if "_cagr_" in formula_code or transform == "cagr":
        return "cagr"
    if "_growth_stability_" in formula_code or transform == "stability":
        return "stability"
    if "_growth_consistency_" in formula_code or transform == "consistency":
        return "consistency"
    raise ValueError(f"Cannot infer growth method for formula_code={formula_code!r}")


def _infer_horizon_years(formula_code: str, method: str, expression: dict[str, str]) -> float:
    for key in ("horizon_years", "years"):
        if key in expression:
            return float(expression[key])
    if method == "qoq":
        return 0.25
    if method == "yoy":
        return 1.0
    marker = formula_code.rsplit("_", 1)[-1]
    if marker.endswith("y"):
        return float(marker[:-1])
    raise ValueError(f"Cannot infer horizon years for formula_code={formula_code!r}")


def _metric_code(row: FormulaRegistrySeedRow, expression: dict[str, str]) -> str:
    metric = expression.get("metric") or row.numerator_code
    if not metric:
        raise ValueError(f"Growth formula {row.formula_code!r} does not name a metric")
    return metric


def _spec_from_row(row: FormulaRegistrySeedRow) -> GrowthFormulaSpec | None:
    if row.family == "growth":
        if not row.formula_code.endswith("_growth_yoy"):
            return None
    elif row.family != "growth_cagr":
        return None

    expression = _parse_expression(row.expression)
    metric_code = _metric_code(row, expression)
    method = _infer_method(row.formula_code, row.transform, expression)
    horizon_years = _infer_horizon_years(row.formula_code, method, expression)
    basis = expression.get("basis") or GROWTH_METRIC_BASIS.get(metric_code)
    if basis is None:
        raise ValueError(f"Growth formula {row.formula_code!r} has unknown basis for {metric_code!r}")
    min_observations = int(expression.get("min_observations", 2))
    return GrowthFormulaSpec(
        formula_code=row.formula_code,
        family=row.family,
        kind=row.kind,
        unit=row.unit,
        metric_code=metric_code,
        basis=basis,
        growth_method=method,
        horizon_years=horizon_years,
        transform=row.transform,
        require_positive_base=row.is_meaningful_rule == REQUIRE_POSITIVE_DENOMINATOR_RULE,
        min_observations=min_observations,
    )


@lru_cache(maxsize=None)
def load_growth_formula_specs(
    seed_path: str | None = None,
) -> tuple[GrowthFormulaSpec, ...]:
    """Load growth-engine specs from the committed formula registry seed."""

    rows = load_ratio_formula_rows(SEED_PATH if seed_path is None else seed_path)
    specs = [_spec_from_row(row) for row in rows]
    return tuple(spec for spec in specs if spec is not None)


def _normalize_metric_history(metric_history: pd.DataFrame) -> pd.DataFrame:
    if metric_history is None or metric_history.empty:
        return pd.DataFrame()
    out = metric_history.copy()
    defaults = {
        "input_table": "fundamental_metric_history",
        "upstream_source": pd.NA,
        "symbol": pd.NA,
        "cik": pd.NA,
        "period_start": pd.NaT,
        "fiscal_year": pd.NA,
        "fiscal_period": pd.NA,
        "source_metric_id": pd.NA,
        "source_accession": pd.NA,
        "filed_date": pd.NaT,
        "vintage_class": "most_recently_restated",
        "is_latest_revision": True,
    }
    for column, value in defaults.items():
        if column not in out.columns:
            out[column] = value

    out["metric_code"] = out["metric_code"].astype("string")
    out["basis"] = out["basis"].astype("string")
    out["period_end"] = pd.to_datetime(out["period_end"], errors="coerce").dt.date
    out["period_start"] = pd.to_datetime(out["period_start"], errors="coerce").dt.date
    out["as_of_date"] = pd.to_datetime(out.get("as_of_date", out["period_end"]), errors="coerce").dt.date
    out["available_at"] = pd.to_datetime(out["available_at"], errors="coerce")
    out["filed_date"] = pd.to_datetime(out["filed_date"], errors="coerce").dt.date
    out["value"] = pd.to_numeric(out["value"], errors="coerce")
    out = out.dropna(subset=["security_id", "metric_code", "basis", "period_end", "value", "available_at"])
    if out.empty:
        return out
    out["period_end_ts"] = pd.to_datetime(out["period_end"], errors="coerce")
    return out.sort_values(
        ["security_id", "metric_code", "basis", "period_end_ts", "available_at", "source_metric_id"],
        kind="mergesort",
    ).reset_index(drop=True)


def _expected_gap_days(spec: GrowthFormulaSpec) -> float:
    if spec.growth_method == "qoq":
        return 91.0
    return 365.25 * spec.horizon_years


def _gap_tolerance_days(spec: GrowthFormulaSpec) -> float:
    if spec.growth_method == "qoq":
        return 25.0
    if spec.growth_method == "yoy":
        return 35.0
    return max(45.0, 18.0 * spec.horizon_years)


def _base_row(group: pd.DataFrame, current: pd.Series, spec: GrowthFormulaSpec) -> pd.Series | None:
    cur_end = pd.Timestamp(current["period_end_ts"])
    expected = _expected_gap_days(spec)
    tolerance = _gap_tolerance_days(spec)
    candidates = group[group["period_end_ts"] < cur_end].copy()
    if candidates.empty:
        return None
    candidates["gap_days"] = (cur_end - candidates["period_end_ts"]).dt.days.astype(float)
    candidates["gap_distance"] = (candidates["gap_days"] - expected).abs()
    candidates = candidates[candidates["gap_distance"] <= tolerance]
    if candidates.empty:
        return None
    candidates = candidates.sort_values(
        ["gap_distance", "period_end_ts", "available_at", "source_metric_id"],
        ascending=[True, False, False, False],
        kind="mergesort",
    )
    return candidates.iloc[0]


def _elapsed_years(current: pd.Series, base: pd.Series) -> float:
    cur = _as_date(current.get("period_end"))
    prev = _as_date(base.get("period_end"))
    if cur is None or prev is None:
        return 0.0
    return max((cur - prev).days / 365.25, 0.0)


def _input_code(row: pd.Series) -> str:
    return f"{row.get('input_table')}.{row.get('metric_code')}"


def _lineage_leg(row: pd.Series) -> dict[str, Any]:
    return {
        "table": row.get("input_table"),
        "source": row.get("upstream_source"),
        "source_metric_id": row.get("source_metric_id"),
        "security_id": row.get("security_id"),
        "metric_code": row.get("metric_code"),
        "basis": row.get("basis"),
        "period_start": _iso(row.get("period_start")),
        "period_end": _iso(row.get("period_end")),
        "available_at": _iso(row.get("available_at")),
        "source_accession": row.get("source_accession"),
        "filed_date": _iso(row.get("filed_date")),
        "value": _safe_float(row.get("value")),
    }


def _pair_record(
    spec: GrowthFormulaSpec,
    current: pd.Series,
    base: pd.Series,
    *,
    source: str,
    run_id: str | None,
    value: float,
    is_meaningful: bool,
    elapsed_years: float,
    lineage: dict[str, Any] | None = None,
) -> dict[str, Any]:
    current_av = _as_timestamp(current.get("available_at"))
    base_av = _as_timestamp(base.get("available_at"))
    if current_av is None or base_av is None:
        raise ValueError("Growth rows require current and base availability timestamps")
    available_at = max(current_av, base_av)
    lineage_payload = lineage or {
        "formula": {
            "formula_code": spec.formula_code,
            "method": spec.growth_method,
            "horizon_years": spec.horizon_years,
        },
        "current": _lineage_leg(current),
        "base": _lineage_leg(base),
    }
    return {
        "growth_id": _growth_id(
            source,
            str(current.get("security_id")),
            spec.formula_code,
            spec.basis,
            current.get("period_end"),
            base.get("period_end"),
            available_at,
        ),
        "source": source,
        "security_id": current.get("security_id"),
        "symbol": current.get("symbol"),
        "cik": current.get("cik"),
        "formula_code": spec.formula_code,
        "family": spec.family,
        "kind": spec.kind,
        "unit": spec.unit,
        "basis": spec.basis,
        "growth_method": spec.growth_method,
        "horizon_years": float(spec.horizon_years),
        "elapsed_years": float(elapsed_years),
        "period_start": current.get("period_start"),
        "period_end": current.get("period_end"),
        "base_period_start": base.get("period_start"),
        "base_period_end": base.get("period_end"),
        "fiscal_year": current.get("fiscal_year"),
        "fiscal_period": current.get("fiscal_period"),
        "value": float(value),
        "current_code": spec.metric_code,
        "current_value": float(current.get("value")),
        "base_code": spec.metric_code,
        "base_value": float(base.get("value")),
        "is_meaningful": bool(is_meaningful),
        "is_latest_revision": True,
        "vintage_class": current.get("vintage_class") or "most_recently_restated",
        "current_available_at": current_av,
        "base_available_at": base_av,
        "available_at": available_at,
        "as_of_date": current.get("period_end"),
        "current_source_accession": current.get("source_accession"),
        "base_source_accession": base.get("source_accession"),
        "current_filed_date": current.get("filed_date"),
        "base_filed_date": base.get("filed_date"),
        "input_codes_json": json_dumps({"current": _input_code(current), "base": _input_code(base)}),
        "input_lineage_json": json_dumps(lineage_payload),
        "run_id": run_id,
    }


def _pct_change_value(current_value: float, base_value: float) -> float | None:
    if base_value == 0:
        return None
    return (current_value - base_value) / abs(base_value)


def _cagr_value(current_value: float, base_value: float, years: float) -> float | None:
    if current_value <= 0 or base_value <= 0 or years <= 0:
        return None
    return (current_value / base_value) ** (1.0 / years) - 1.0


def _window_start(current: pd.Series, spec: GrowthFormulaSpec) -> pd.Timestamp:
    cur_end = pd.Timestamp(current["period_end_ts"])
    return cur_end - pd.Timedelta(days=int(round(spec.horizon_years * 365.25 + _gap_tolerance_days(spec))))


def _stability_observations(
    group: pd.DataFrame,
    current: pd.Series,
    spec: GrowthFormulaSpec,
) -> list[tuple[pd.Series, pd.Series, float]]:
    window_start = _window_start(current, spec)
    candidates = group[
        (group["period_end_ts"] <= current["period_end_ts"])
        & (group["period_end_ts"] >= window_start)
    ].copy()
    observations: list[tuple[pd.Series, pd.Series, float]] = []
    yoy_spec = GrowthFormulaSpec(
        formula_code=f"{spec.metric_code}_growth_yoy_window",
        family=spec.family,
        kind=spec.kind,
        unit=spec.unit,
        metric_code=spec.metric_code,
        basis=spec.basis,
        growth_method="yoy",
        horizon_years=1.0,
        transform="pct_change",
        require_positive_base=False,
    )
    for _, candidate in candidates.sort_values("period_end_ts", ascending=False).iterrows():
        base = _base_row(group, candidate, yoy_spec)
        if base is None:
            continue
        cur_value = _safe_float(candidate.get("value"))
        base_value = _safe_float(base.get("value"))
        if cur_value is None or base_value is None:
            continue
        growth = _pct_change_value(cur_value, base_value)
        if growth is None:
            continue
        observations.append((candidate, base, growth))
    observations.reverse()
    return observations


def _stability_record(
    spec: GrowthFormulaSpec,
    group: pd.DataFrame,
    current: pd.Series,
    *,
    source: str,
    run_id: str | None,
) -> dict[str, Any] | None:
    observations = _stability_observations(group, current, spec)
    if len(observations) < spec.min_observations:
        return None

    growth_values = [obs[2] for obs in observations]
    if spec.growth_method == "stability":
        dispersion = float(pd.Series(growth_values, dtype="float64").std(ddof=0))
        value = 1.0 / (1.0 + dispersion)
    elif spec.growth_method == "consistency":
        value = sum(1 for growth in growth_values if growth > 0) / len(growth_values)
    else:
        raise ValueError(f"Unsupported stability method {spec.growth_method!r}")

    base = observations[0][1]
    current_av = _as_timestamp(current.get("available_at"))
    base_avs = [_as_timestamp(obs_base.get("available_at")) for _, obs_base, _ in observations]
    current_avs = [_as_timestamp(obs_current.get("available_at")) for obs_current, _, _ in observations]
    availability = [ts for ts in (*base_avs, *current_avs, current_av) if ts is not None]
    if not availability:
        return None
    max_prior_av = max(ts for ts in (*base_avs, *current_avs[:-1]) if ts is not None)
    lineage = {
        "formula": {
            "formula_code": spec.formula_code,
            "method": spec.growth_method,
            "horizon_years": spec.horizon_years,
        },
        "observations": [
            {
                "growth": growth,
                "current": _lineage_leg(obs_current),
                "base": _lineage_leg(obs_base),
            }
            for obs_current, obs_base, growth in observations
        ],
    }
    record = _pair_record(
        spec,
        current,
        base,
        source=source,
        run_id=run_id,
        value=value,
        is_meaningful=all(float(obs_base.get("value")) > 0 for _, obs_base, _ in observations),
        elapsed_years=_elapsed_years(current, base),
        lineage=lineage,
    )
    record["base_available_at"] = max_prior_av
    record["available_at"] = max(availability)
    record["growth_id"] = _growth_id(
        source,
        str(current.get("security_id")),
        spec.formula_code,
        spec.basis,
        current.get("period_end"),
        base.get("period_end"),
        record["available_at"],
    )
    return record


def compute_growth_rows(
    metric_history: pd.DataFrame,
    *,
    specs: tuple[GrowthFormulaSpec, ...] | None = None,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: normalized metric history -> ``fundamental_growth`` rows."""

    history = _normalize_metric_history(metric_history)
    if history.empty:
        return pd.DataFrame(columns=FUNDAMENTAL_GROWTH_COLUMNS)
    resolved_specs = specs or load_growth_formula_specs()
    specs_by_key: dict[tuple[str, str], list[GrowthFormulaSpec]] = {}
    for spec in resolved_specs:
        specs_by_key.setdefault((spec.metric_code, spec.basis), []).append(spec)

    records: list[dict[str, Any]] = []
    for key, group in history.groupby(["metric_code", "basis", "security_id"], sort=False):
        metric_code, basis, _security_id = key
        group_specs = specs_by_key.get((str(metric_code), str(basis)), [])
        if not group_specs:
            continue
        group = group.sort_values(["period_end_ts", "available_at", "source_metric_id"], kind="mergesort")
        for _, current in group.iterrows():
            current_value = _safe_float(current.get("value"))
            if current_value is None:
                continue
            for spec in group_specs:
                if spec.growth_method in {"stability", "consistency"}:
                    record = _stability_record(spec, group, current, source=source, run_id=run_id)
                    if record is not None:
                        records.append(record)
                    continue
                base = _base_row(group, current, spec)
                if base is None:
                    continue
                base_value = _safe_float(base.get("value"))
                if base_value is None:
                    continue
                years = _elapsed_years(current, base)
                if spec.growth_method in {"yoy", "qoq"}:
                    value = _pct_change_value(current_value, base_value)
                    if value is None:
                        continue
                    is_meaningful = (not spec.require_positive_base) or base_value > 0
                elif spec.growth_method == "cagr":
                    value = _cagr_value(current_value, base_value, years)
                    if value is None:
                        continue
                    is_meaningful = True
                else:
                    raise ValueError(f"Unsupported growth method {spec.growth_method!r}")
                records.append(
                    _pair_record(
                        spec,
                        current,
                        base,
                        source=source,
                        run_id=run_id,
                        value=value,
                        is_meaningful=is_meaningful,
                        elapsed_years=years,
                    )
                )

    if not records:
        return pd.DataFrame(columns=FUNDAMENTAL_GROWTH_COLUMNS)
    return pd.DataFrame(records, columns=FUNDAMENTAL_GROWTH_COLUMNS)


def _normalized_symbols(symbols: tuple[str, ...] | None) -> tuple[str, ...]:
    return tuple(sorted({str(symbol).strip().upper() for symbol in symbols or () if str(symbol).strip()}))


def _normalized_metrics(metrics: tuple[str, ...] | None) -> tuple[str, ...]:
    return tuple(sorted({str(metric).strip() for metric in metrics or () if str(metric).strip()}))


def _end_of_day_asof_ts(as_of_date: dt.date) -> dt.datetime:
    return dt.datetime.combine(as_of_date, dt.time.max).replace(tzinfo=None)


def _register_filter(store: DuckDBStore, relation: str, column: str, values: tuple[str, ...]) -> bool:
    if not values:
        return False
    store.con.register(relation, pd.DataFrame({column: list(values)}))
    return True


def metric_lineage_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: DuckDBStore | None = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    metric_codes: tuple[str, ...] | list[str] | None = None,
    source_tables: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible metric lineage rows as of a point in time."""

    resolved_as_of_ts = (as_of_ts or _end_of_day_asof_ts(as_of_date)).replace(tzinfo=None)
    symbol_values = _normalized_symbols(tuple(symbols or ()))
    metric_values = _normalized_metrics(tuple(metric_codes or ()))
    source_table_values = _normalized_metrics(tuple(source_tables or ()))

    def _run(active: DuckDBStore) -> pd.DataFrame:
        registered: list[str] = []
        joins: list[str] = []
        try:
            if _register_filter(active, "metric_lineage_symbol_filter", "symbol", symbol_values):
                registered.append("metric_lineage_symbol_filter")
                joins.append("JOIN metric_lineage_symbol_filter sf ON sf.symbol = ml.symbol")
            if _register_filter(active, "metric_lineage_code_filter", "metric_code", metric_values):
                registered.append("metric_lineage_code_filter")
                joins.append("JOIN metric_lineage_code_filter cf ON cf.metric_code = ml.metric_code")
            if _register_filter(active, "metric_lineage_source_filter", "metric_source_table", source_table_values):
                registered.append("metric_lineage_source_filter")
                joins.append("JOIN metric_lineage_source_filter stf ON stf.metric_source_table = ml.metric_source_table")
            join_sql = "\n        ".join(joins)
            return active.con.execute(
                f"""
                SELECT ml.*
                FROM v_metric_lineage ml
                {join_sql}
                WHERE ml.metric_as_of_date <= ?
                  AND ml.metric_available_at <= ?
                  AND (
                      ml.formula_valid_from IS NULL
                      OR ml.formula_valid_from <= ?
                  )
                  AND (
                      ml.formula_valid_to IS NULL
                      OR ml.formula_valid_to > ?
                  )
                ORDER BY ml.metric_source_table, ml.security_id, ml.metric_code, ml.metric_as_of_date
                """,
                [as_of_date, resolved_as_of_ts, as_of_date, as_of_date],
            ).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)


def _derive_free_cash_flow_rows(history: pd.DataFrame) -> pd.DataFrame:
    if history.empty:
        return history
    flows = history[history["metric_code"].isin(["operating_cash_flow", "capital_expenditures"])].copy()
    if flows.empty:
        return history
    key_columns = [
        "security_id",
        "basis",
        "period_end",
        "symbol",
        "cik",
        "period_start",
        "fiscal_year",
        "fiscal_period",
    ]
    left = flows[flows["metric_code"] == "operating_cash_flow"].rename(
        columns={
            "value": "ocf_value",
            "available_at": "ocf_available_at",
            "source_metric_id": "ocf_source_metric_id",
            "source_accession": "ocf_source_accession",
            "filed_date": "ocf_filed_date",
            "upstream_source": "ocf_upstream_source",
        }
    )
    right = flows[flows["metric_code"] == "capital_expenditures"].rename(
        columns={
            "value": "capex_value",
            "available_at": "capex_available_at",
            "source_metric_id": "capex_source_metric_id",
            "source_accession": "capex_source_accession",
            "filed_date": "capex_filed_date",
            "upstream_source": "capex_upstream_source",
        }
    )
    merged = left.merge(right, on=key_columns, how="inner", suffixes=("", "_capex"))
    if merged.empty:
        return history
    records: list[dict[str, Any]] = []
    for row in merged.to_dict("records"):
        ocf_av = _as_timestamp(row.get("ocf_available_at"))
        capex_av = _as_timestamp(row.get("capex_available_at"))
        if ocf_av is None or capex_av is None:
            continue
        lineage_key = "|".join(str(part) for part in (row.get("ocf_source_metric_id"), row.get("capex_source_metric_id")))
        source_metric_id = hashlib.sha256(f"free_cash_flow|{lineage_key}".encode("utf-8")).hexdigest()
        records.append(
            {
                "source_metric_id": source_metric_id,
                "input_table": "fundamental_ttm_points",
                "upstream_source": row.get("ocf_upstream_source") or row.get("capex_upstream_source"),
                "security_id": row.get("security_id"),
                "symbol": row.get("symbol"),
                "cik": row.get("cik"),
                "metric_code": "free_cash_flow",
                "basis": "ttm",
                "period_start": row.get("period_start"),
                "period_end": row.get("period_end"),
                "fiscal_year": row.get("fiscal_year"),
                "fiscal_period": row.get("fiscal_period"),
                "value": float(row["ocf_value"]) + float(row["capex_value"]),
                "as_of_date": row.get("period_end"),
                "available_at": max(ocf_av, capex_av),
                "source_accession": row.get("ocf_source_accession") or row.get("capex_source_accession"),
                "filed_date": row.get("ocf_filed_date") or row.get("capex_filed_date"),
                "vintage_class": "most_recently_restated",
                "is_latest_revision": True,
            }
        )
    if not records:
        return history
    return pd.concat([history, pd.DataFrame(records)], ignore_index=True)


def load_growth_inputs(store: DuckDBStore, options: FundamentalGrowthOptions) -> pd.DataFrame:
    symbols = _normalized_symbols(options.symbols)
    metrics = _normalized_metrics(options.metrics)
    registered: list[str] = []
    ttm_join = ""
    bal_join = ""
    params: list[object] = []
    if symbols:
        store.con.register("fundamental_growth_symbol_filter", pd.DataFrame({"symbol": symbols}))
        registered.append("fundamental_growth_symbol_filter")
        ttm_join = "JOIN fundamental_growth_symbol_filter gsf ON gsf.symbol = t.symbol"
        bal_join = "JOIN fundamental_growth_symbol_filter gsf ON gsf.symbol = s.symbol"

    metric_filter_sql = ""
    ttm_metrics = tuple(sorted(TTM_GROWTH_METRICS))
    balance_metrics = tuple(sorted(BALANCE_GROWTH_METRICS))
    if metrics:
        selected_base_metrics = set(metrics)
        if "free_cash_flow" in selected_base_metrics:
            selected_base_metrics.update({"operating_cash_flow", "capital_expenditures"})
        ttm_metrics = tuple(metric for metric in ttm_metrics if metric in selected_base_metrics)
        balance_metrics = tuple(metric for metric in balance_metrics if metric in selected_base_metrics)
        if not ttm_metrics and not balance_metrics:
            return pd.DataFrame()

    try:
        frames: list[pd.DataFrame] = []
        if ttm_metrics:
            metric_placeholders = ", ".join("?" for _ in ttm_metrics)
            params = list(ttm_metrics)
            ttm_sql = f"""
                WITH ranked AS (
                    SELECT
                        t.*,
                        row_number() OVER (
                            PARTITION BY t.security_id, t.canonical_metric, t.ttm_end_date
                            ORDER BY
                                t.available_at DESC NULLS LAST,
                                t.revision_sequence DESC,
                                t.source DESC,
                                t.ttm_point_id DESC
                        ) AS rn
                    FROM fundamental_ttm_points t
                    {ttm_join}
                    WHERE t.is_latest_revision
                      AND t.canonical_metric IN ({metric_placeholders})
                      AND t.ttm_value IS NOT NULL
                      AND t.available_at IS NOT NULL
                )
                SELECT
                    ttm_point_id AS source_metric_id,
                    'fundamental_ttm_points' AS input_table,
                    source AS upstream_source,
                    security_id,
                    symbol,
                    cik,
                    canonical_metric AS metric_code,
                    'ttm' AS basis,
                    ttm_start_date AS period_start,
                    ttm_end_date AS period_end,
                    fiscal_year,
                    fiscal_period,
                    ttm_value AS value,
                    as_of_date,
                    available_at,
                    accession_number AS source_accession,
                    as_of_date AS filed_date,
                    CASE
                        WHEN revision_count = 1 THEN 'as_first_reported'
                        WHEN rn = 1 THEN 'most_recently_restated'
                        ELSE 'intermediate_restatement'
                    END AS vintage_class,
                    is_latest_revision
                FROM ranked
                WHERE rn = 1
            """
            frames.append(store.con.execute(ttm_sql, params).df())
        if balance_metrics:
            metric_placeholders = ", ".join("?" for _ in balance_metrics)
            params = list(balance_metrics)
            bal_sql = f"""
                WITH ranked AS (
                    SELECT
                        s.*,
                        row_number() OVER (
                            PARTITION BY s.security_id, s.canonical_metric, s.period_end
                            ORDER BY
                                s.available_at DESC NULLS LAST,
                                s.revision_sequence DESC,
                                s.source DESC,
                                s.statement_point_id DESC
                        ) AS rn
                    FROM fundamental_statement_points s
                    {bal_join}
                    WHERE s.is_latest_revision
                      AND s.period_type = 'instant'
                      AND s.canonical_metric IN ({metric_placeholders})
                      AND s.value IS NOT NULL
                      AND s.available_at IS NOT NULL
                )
                SELECT
                    statement_point_id AS source_metric_id,
                    'fundamental_statement_points' AS input_table,
                    source AS upstream_source,
                    security_id,
                    symbol,
                    cik,
                    canonical_metric AS metric_code,
                    'instant' AS basis,
                    period_start,
                    period_end,
                    fiscal_year,
                    fiscal_period,
                    value,
                    as_of_date,
                    available_at,
                    coalesce(source_accession, accession_number) AS source_accession,
                    coalesce(filed_date, as_of_date) AS filed_date,
                    CASE
                        WHEN revision_count = 1 THEN 'as_first_reported'
                        WHEN rn = 1 THEN 'most_recently_restated'
                        ELSE 'intermediate_restatement'
                    END AS vintage_class,
                    is_latest_revision
                FROM ranked
                WHERE rn = 1
            """
            frames.append(store.con.execute(bal_sql, params).df())
    finally:
        for relation in registered:
            store.con.unregister(relation)

    if not frames:
        return pd.DataFrame()
    history = pd.concat(frames, ignore_index=True)
    history = _derive_free_cash_flow_rows(history)
    if metrics:
        history = history[history["metric_code"].isin(metrics)].copy()
    return history


def refresh_fundamental_growth(
    store: DuckDBStore,
    options: FundamentalGrowthOptions | None = None,
) -> int:
    options = options or FundamentalGrowthOptions()
    store.initialize()
    inputs = load_growth_inputs(store, options)
    rows = compute_growth_rows(inputs, source=options.source, run_id=options.run_id)
    predicates = ["source = ?"]
    params: list[object] = [options.source]
    symbols = _normalized_symbols(options.symbols)
    metrics = _normalized_metrics(options.metrics)
    if symbols:
        placeholders = ", ".join("?" for _ in symbols)
        predicates.append(f"upper(symbol) IN ({placeholders})")
        params.extend(symbols)
    if metrics:
        placeholders = ", ".join("?" for _ in metrics)
        predicates.append(f"current_code IN ({placeholders})")
        params.extend(metrics)
    with store.transaction():
        store.con.execute(f"DELETE FROM fundamental_growth WHERE {' AND '.join(predicates)}", params)
        if not rows.empty:
            insert_frame(store, rows, "fundamental_growth", "fundamental_growth_insert")
    return int(len(rows))


class FundamentalGrowthDataset(Dataset):
    dataset_id = "fundamental_growth"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FundamentalGrowthOptions) -> DatasetLoadResult:
        rows = refresh_fundamental_growth(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="fundamental_growth",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"growth_formula_count": len(load_growth_formula_specs())},
        )
