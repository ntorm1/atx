"""Fail-closed canonical panel validation."""

from __future__ import annotations

import polars as pl

REQUIRED_PANEL_COLUMNS = ("date", "asset_id", "signal", "forward_return")


class PanelValidationError(ValueError):
    """The research panel violates a no-look-ahead or data-integrity contract."""


def _materialize(frame: pl.DataFrame | pl.LazyFrame) -> pl.DataFrame:
    if isinstance(frame, pl.LazyFrame):
        return frame.collect(engine="streaming")
    if not isinstance(frame, pl.DataFrame):
        raise TypeError("panel must be a polars DataFrame or LazyFrame")
    return frame


def validate_panel(frame: pl.DataFrame | pl.LazyFrame) -> pl.DataFrame:
    """Validate and canonicalize one signal/forward-return panel.

    Validation is deliberately strict: null/non-finite observations, duplicate natural keys,
    future-known signals, and non-forward labels fail rather than being silently dropped.
    """

    panel = _materialize(frame)
    missing = sorted(set(REQUIRED_PANEL_COLUMNS).difference(panel.columns))
    if missing:
        raise PanelValidationError(f"panel is missing required columns: {missing}")

    casts: list[pl.Expr] = [
        pl.col("date").cast(pl.Date, strict=True),
        pl.col("asset_id").cast(pl.String, strict=True),
        pl.col("signal").cast(pl.Float64, strict=True),
        pl.col("forward_return").cast(pl.Float64, strict=True),
    ]
    if "available_at" in panel.columns:
        casts.append(pl.col("available_at").cast(pl.Datetime("us"), strict=True))
    if "forward_end_date" in panel.columns:
        casts.append(pl.col("forward_end_date").cast(pl.Date, strict=True))
    for optional_float in ("adv_usd", "borrow_rate"):
        if optional_float in panel.columns:
            casts.append(pl.col(optional_float).cast(pl.Float64, strict=True))
    if "group" in panel.columns:
        casts.append(pl.col("group").cast(pl.String, strict=True))
    panel = panel.with_columns(casts)

    critical = list(REQUIRED_PANEL_COLUMNS)
    if panel.select(pl.any_horizontal(pl.col(*critical).is_null()).any()).item():
        raise PanelValidationError("panel contains nulls in required columns")
    if panel.select(
        (~pl.col("signal").is_finite() | ~pl.col("forward_return").is_finite()).any()
    ).item():
        raise PanelValidationError("panel contains non-finite signal or forward return")
    duplicate_count = (
        panel.group_by("date", "asset_id")
        .len()
        .filter(pl.col("len") > 1)
        .height
    )
    if duplicate_count:
        raise PanelValidationError(
            f"panel contains {duplicate_count} duplicate (date, asset_id) keys"
        )
    if "available_at" in panel.columns and panel.select(
        (pl.col("available_at").dt.date() > pl.col("date")).any()
    ).item():
        raise PanelValidationError("panel contains signals known after formation date")
    if "forward_end_date" in panel.columns and panel.select(
        (pl.col("forward_end_date") <= pl.col("date")).any()
    ).item():
        raise PanelValidationError("forward_end_date must be strictly after formation date")
    if "adv_usd" in panel.columns:
        invalid_adv = pl.col("adv_usd").is_not_null() & (
            ~pl.col("adv_usd").is_finite() | (pl.col("adv_usd") <= 0)
        )
        if panel.select(invalid_adv.any()).item():
            raise PanelValidationError("adv_usd must be positive and finite when supplied")
    if "borrow_rate" in panel.columns:
        invalid_borrow = pl.col("borrow_rate").is_not_null() & (
            ~pl.col("borrow_rate").is_finite() | (pl.col("borrow_rate") < 0)
        )
        if panel.select(invalid_borrow.any()).item():
            raise PanelValidationError("borrow_rate must be non-negative and finite")
    return panel.sort("date", "asset_id")
