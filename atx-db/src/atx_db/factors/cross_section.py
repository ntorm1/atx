"""Pure cross-sectional factor operators.

All operators work within the as-of cross-section. When a frame carries multiple
factor ids, the default partition is ``(factor_id, as_of_date)`` so factors do
not bleed into one another.
"""

from __future__ import annotations

from collections.abc import Sequence

import numpy as np
import pandas as pd


class CrossSectionOperatorError(ValueError):
    """Raised when a cross-sectional operator cannot run on the supplied frame."""


def _partition_columns(frame: pd.DataFrame, partition_columns: Sequence[str] | None) -> tuple[str, ...]:
    if partition_columns is not None:
        columns = tuple(partition_columns)
    elif "factor_id" in frame.columns:
        columns = ("factor_id", "as_of_date")
    else:
        columns = ("as_of_date",)
    missing = [column for column in columns if column not in frame.columns]
    if missing:
        raise CrossSectionOperatorError(f"Missing partition columns: {missing}")
    return columns


def _normalize(
    frame: pd.DataFrame,
    *,
    value_column: str,
    partition_columns: Sequence[str] | None,
) -> tuple[pd.DataFrame, tuple[str, ...]]:
    if value_column not in frame.columns:
        raise CrossSectionOperatorError(f"Missing value column: {value_column}")
    columns = _partition_columns(frame, partition_columns)
    out = frame.copy()
    out[value_column] = pd.to_numeric(out[value_column], errors="coerce")
    if "as_of_date" in out.columns:
        out["as_of_date"] = pd.to_datetime(out["as_of_date"], errors="coerce").dt.date
    return out, columns


def rank(
    frame: pd.DataFrame,
    *,
    value_column: str = "value",
    output_column: str = "value",
    partition_columns: Sequence[str] | None = None,
    ascending: bool = True,
    method: str = "min",
) -> pd.DataFrame:
    """Return cross-sectional percent rank within each as-of partition."""

    out, partitions = _normalize(frame, value_column=value_column, partition_columns=partition_columns)

    def _percent_rank(values: pd.Series) -> pd.Series:
        valid_count = int(values.notna().sum())
        if valid_count <= 1:
            return pd.Series(0.0, index=values.index).where(values.notna())
        ranks = values.rank(method=method, ascending=ascending, na_option="keep")
        return (ranks - 1.0) / float(valid_count - 1)

    out[output_column] = out.groupby(list(partitions), dropna=False)[value_column].transform(_percent_rank)
    return out


def zscore(
    frame: pd.DataFrame,
    *,
    value_column: str = "value",
    output_column: str = "value",
    partition_columns: Sequence[str] | None = None,
) -> pd.DataFrame:
    """Return sample-standardized values within each as-of partition."""

    out, partitions = _normalize(frame, value_column=value_column, partition_columns=partition_columns)

    def _z(values: pd.Series) -> pd.Series:
        mean = values.mean(skipna=True)
        std = values.std(skipna=True, ddof=1)
        if pd.isna(std) or std == 0:
            return pd.Series(pd.NA, index=values.index, dtype="Float64")
        standardized = (values - mean) / std
        return standardized.where(np.isfinite(standardized)).astype("Float64")

    out[output_column] = out.groupby(list(partitions), dropna=False)[value_column].transform(_z)
    return out


def winsorize(
    frame: pd.DataFrame,
    *,
    value_column: str = "value",
    output_column: str = "value",
    partition_columns: Sequence[str] | None = None,
    limits: float | tuple[float, float] = 0.01,
) -> pd.DataFrame:
    """Cap values at declared lower/upper cross-sectional quantiles."""

    out, partitions = _normalize(frame, value_column=value_column, partition_columns=partition_columns)
    if isinstance(limits, tuple):
        lower_limit, upper_limit = limits
    else:
        lower_limit, upper_limit = float(limits), float(limits)
    if not 0 <= lower_limit < 0.5 or not 0 <= upper_limit < 0.5:
        raise CrossSectionOperatorError("winsorize limits must be in [0, 0.5)")

    def _cap(values: pd.Series) -> pd.Series:
        if values.dropna().empty:
            return values
        lower = values.quantile(lower_limit)
        upper = values.quantile(1.0 - upper_limit)
        return values.clip(lower=lower, upper=upper)

    out[output_column] = out.groupby(list(partitions), dropna=False)[value_column].transform(_cap)
    return out


def _design_matrix(group: pd.DataFrame, by: Sequence[str]) -> pd.DataFrame:
    pieces: list[pd.DataFrame] = []
    for column in by:
        if column not in group.columns:
            raise CrossSectionOperatorError(f"Missing neutralization column: {column}")
        series = group[column]
        if pd.api.types.is_numeric_dtype(series):
            pieces.append(pd.DataFrame({column: pd.to_numeric(series, errors="coerce")}, index=group.index))
        else:
            dummies = pd.get_dummies(series.astype("string"), prefix=column, drop_first=True, dtype=float)
            dummies.index = group.index
            pieces.append(dummies)
    if not pieces:
        return pd.DataFrame(index=group.index)
    return pd.concat(pieces, axis=1)


def neutralize(
    frame: pd.DataFrame,
    *,
    by: Sequence[str],
    value_column: str = "value",
    output_column: str = "value",
    partition_columns: Sequence[str] | None = None,
) -> pd.DataFrame:
    """Residualize values against covariates within each as-of cross-section."""

    out, partitions = _normalize(frame, value_column=value_column, partition_columns=partition_columns)

    def _residualize(group: pd.DataFrame) -> pd.Series:
        y = pd.to_numeric(group[value_column], errors="coerce")
        valid = y.notna()
        if valid.sum() == 0:
            return pd.Series(pd.NA, index=group.index, dtype="Float64")
        x = _design_matrix(group, by)
        if x.empty:
            return y - y.mean(skipna=True)
        x = x.apply(pd.to_numeric, errors="coerce").fillna(0.0)
        design = np.column_stack([np.ones(len(x), dtype=float), x.to_numpy(dtype=float)])
        beta, *_ = np.linalg.lstsq(design[valid.to_numpy()], y[valid].to_numpy(dtype=float), rcond=None)
        fitted = design @ beta
        residuals = y.to_numpy(dtype=float) - fitted
        return pd.Series(residuals, index=group.index).where(valid)

    residual_parts = [_residualize(group) for _, group in out.groupby(list(partitions), dropna=False)]
    out[output_column] = pd.concat(residual_parts).sort_index() if residual_parts else pd.Series(dtype="Float64")
    return out


def _operator_by_name(name: str):
    operators = {
        "rank": rank,
        "zscore": zscore,
        "winsorize": winsorize,
        "neutralize": neutralize,
    }
    try:
        return operators[name]
    except KeyError as exc:
        raise CrossSectionOperatorError(f"Unsupported PIT-safety operator: {name}") from exc


def pit_safety_report(
    input_frame: pd.DataFrame,
    *,
    transformed_frame: pd.DataFrame | None = None,
    operator: str | None = None,
    operator_kwargs: dict[str, object] | None = None,
    key_columns: Sequence[str] = ("factor_id", "security_id", "as_of_date"),
    value_column: str = "value",
    available_at_column: str = "available_at",
    tolerance: float = 1e-12,
) -> dict[str, object]:
    """Check cross-sectional operator inputs/output for clause-I leakage signatures."""

    future_input_count = 0
    if available_at_column in input_frame.columns and "as_of_date" in input_frame.columns:
        available_dates = pd.to_datetime(input_frame[available_at_column], errors="coerce").dt.date
        as_of_dates = pd.to_datetime(input_frame["as_of_date"], errors="coerce").dt.date
        future_input_count = int((available_dates > as_of_dates).fillna(False).sum())

    mismatch_count = 0
    max_abs_delta: float | None = None
    if transformed_frame is not None and operator is not None:
        expected = _operator_by_name(operator)(input_frame, value_column=value_column, **(operator_kwargs or {}))
        missing = [column for column in (*key_columns, value_column) if column not in transformed_frame.columns]
        if missing:
            raise CrossSectionOperatorError(f"Transformed frame missing columns: {missing}")
        merged = expected[list(key_columns) + [value_column]].merge(
            transformed_frame[list(key_columns) + [value_column]],
            on=list(key_columns),
            how="inner",
            suffixes=("_expected", "_actual"),
        )
        if not merged.empty:
            deltas = (
                pd.to_numeric(merged[f"{value_column}_expected"], errors="coerce")
                - pd.to_numeric(merged[f"{value_column}_actual"], errors="coerce")
            ).abs()
            mismatch_count = int((deltas > tolerance).sum())
            max_abs_delta = float(deltas.max()) if not deltas.empty else None

    status = "passed" if future_input_count == 0 and mismatch_count == 0 else "failed"
    return {
        "status": status,
        "future_input_count": future_input_count,
        "operator_mismatch_count": mismatch_count,
        "max_abs_delta": max_abs_delta,
    }
