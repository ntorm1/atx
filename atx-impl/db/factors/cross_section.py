"""Pure cross-sectional factor operators.

All operators work within the as-of cross-section. When a frame carries multiple
factor ids, the default partition is ``(factor_id, as_of_date)`` so factors do
not bleed into one another.
"""

from __future__ import annotations

from collections.abc import Sequence

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
        return (values - mean) / std

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
