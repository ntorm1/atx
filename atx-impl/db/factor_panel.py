"""Backtest-ready factor panel assembly helpers.

PF3-S10 consumes the S8/S9 factor-value surfaces and exposes the S2 panel
contract on decision dates. Source factor rows keep their own observation date;
the panel's ``as_of_date`` is the first date the row is safe to consume.
"""

from __future__ import annotations

import datetime as dt
from pathlib import Path
from typing import Iterable

import pandas as pd

from .connection import DEFAULT_DB_PATH
from .lake import DEFAULT_LAKE_ROOT, LakeExportResult, LakehouseExporter

DEFAULT_FACTOR_PANEL_UNIVERSE_ID = "us_common_equity_liquid_v1"

FACTOR_PANEL_COLUMNS = (
    "security_id",
    "as_of_date",
    "factor_id",
    "value",
    "available_at",
    "source_loaded_at",
    "run_id",
    "input_lineage_json",
)

_FACTOR_INPUT_COLUMNS = (
    "security_id",
    "factor_id",
    "as_of_date",
    "value",
    "available_at",
    "source_loaded_at",
    "run_id",
    "input_lineage_json",
)


def _empty_panel() -> pd.DataFrame:
    return pd.DataFrame(columns=FACTOR_PANEL_COLUMNS)


def _as_frame(frame: pd.DataFrame | Iterable[dict[str, object]] | None) -> pd.DataFrame:
    if frame is None:
        return pd.DataFrame()
    if isinstance(frame, pd.DataFrame):
        return frame.copy()
    return pd.DataFrame(frame)


def _end_of_day(value: dt.date | str | pd.Timestamp) -> pd.Timestamp:
    day = pd.Timestamp(value).normalize()
    return day + pd.Timedelta(days=1) - pd.Timedelta(microseconds=1)


def _normalize_factor_surface(frame: pd.DataFrame | Iterable[dict[str, object]] | None) -> pd.DataFrame:
    values = _as_frame(frame)
    if values.empty:
        return _empty_panel()

    missing = [column for column in _FACTOR_INPUT_COLUMNS if column not in values.columns]
    if missing:
        raise ValueError(f"factor surface is missing required columns: {missing}")

    values = values.copy()
    if "is_latest_revision" in values.columns:
        values = values[values["is_latest_revision"].fillna(False).astype(bool)]
    values = values[values["value"].notna()]
    if values.empty:
        return _empty_panel()

    source_as_of = pd.to_datetime(values["as_of_date"]).dt.normalize()
    available_at = pd.to_datetime(values["available_at"])
    decision_as_of = pd.Series(
        [max(source_date, available.normalize()) for source_date, available in zip(source_as_of, available_at)],
        index=values.index,
    )

    panel = values.loc[:, _FACTOR_INPUT_COLUMNS].copy()
    panel["as_of_date"] = decision_as_of.dt.date
    panel["available_at"] = available_at
    panel["source_loaded_at"] = pd.to_datetime(panel["source_loaded_at"])
    return panel.loc[:, FACTOR_PANEL_COLUMNS]


def _filter_asof(
    panel: pd.DataFrame,
    *,
    as_of_date: dt.date | str | None,
    as_of_ts: dt.datetime | str | None,
) -> pd.DataFrame:
    if panel.empty:
        return panel

    out = panel
    decision_cutoff = pd.Timestamp(as_of_date).date() if as_of_date is not None else None
    timestamp_cutoff = pd.Timestamp(as_of_ts) if as_of_ts is not None else None
    if timestamp_cutoff is None and decision_cutoff is not None:
        timestamp_cutoff = _end_of_day(decision_cutoff)

    if decision_cutoff is not None:
        out = out[pd.to_datetime(out["as_of_date"]).dt.date <= decision_cutoff]
    if timestamp_cutoff is not None:
        out = out[pd.to_datetime(out["available_at"]) <= timestamp_cutoff]
    return out


def _apply_universe_filter(
    panel: pd.DataFrame,
    universe_membership: pd.DataFrame | Iterable[dict[str, object]] | None,
    *,
    universe_id: str,
    as_of_ts: dt.datetime | str | None,
) -> pd.DataFrame:
    membership = _as_frame(universe_membership)
    if panel.empty or membership.empty:
        return panel

    required = ("universe_id", "security_id", "valid_from", "valid_to", "as_of_date", "is_member", "available_at")
    missing = [column for column in required if column not in membership.columns]
    if missing:
        raise ValueError(f"universe membership is missing required columns: {missing}")

    members = membership.copy()
    members = members[members["universe_id"] == universe_id]
    members = members[members["is_member"].fillna(False).astype(bool)]
    if "is_latest_revision" in members.columns:
        members = members[members["is_latest_revision"].fillna(False).astype(bool)]
    if members.empty:
        return _empty_panel()

    members["valid_from"] = pd.to_datetime(members["valid_from"]).dt.date
    members["valid_to"] = pd.to_datetime(members["valid_to"]).dt.date
    members["membership_as_of_date"] = pd.to_datetime(members["as_of_date"]).dt.date
    members["membership_available_at"] = pd.to_datetime(members["available_at"])
    if "source_loaded_at" in members.columns:
        members["membership_source_loaded_at"] = pd.to_datetime(members["source_loaded_at"])
    else:
        members["membership_source_loaded_at"] = pd.Timestamp.min
    if "source" not in members.columns:
        members["source"] = ""

    left = panel.reset_index(drop=True).reset_index(names="_panel_row")
    merged = left.merge(members, on="security_id", how="inner", suffixes=("", "_membership"))
    if merged.empty:
        return _empty_panel()

    panel_dates = pd.to_datetime(merged["as_of_date"]).dt.date
    if as_of_ts is None:
        membership_cutoffs = panel_dates.map(_end_of_day)
    else:
        membership_cutoffs = pd.Series(pd.Timestamp(as_of_ts), index=merged.index)

    valid_to = merged["valid_to"]
    visibility_mask = merged["membership_available_at"].isna() | (
        merged["membership_available_at"] <= membership_cutoffs
    )
    membership_mask = (
        (merged["valid_from"] <= panel_dates)
        & (valid_to.isna() | (valid_to >= panel_dates))
        & (merged["membership_as_of_date"] <= panel_dates)
        & visibility_mask
    )
    matched = merged[membership_mask].copy()
    if matched.empty:
        return _empty_panel()

    matched = matched.sort_values(
        ["_panel_row", "valid_from", "membership_available_at", "membership_source_loaded_at", "source"],
        ascending=[True, False, False, False, False],
    )
    keep_rows = matched.drop_duplicates("_panel_row", keep="first")["_panel_row"].to_numpy()
    return left[left["_panel_row"].isin(keep_rows)].loc[:, FACTOR_PANEL_COLUMNS].reset_index(drop=True)


def assemble_factor_panel_long(
    fundamental_factors: pd.DataFrame | Iterable[dict[str, object]] | None = None,
    cross_domain_factors: pd.DataFrame | Iterable[dict[str, object]] | None = None,
    *,
    universe_membership: pd.DataFrame | Iterable[dict[str, object]] | None = None,
    as_of_date: dt.date | str | None = None,
    as_of_ts: dt.datetime | str | None = None,
    universe_id: str = DEFAULT_FACTOR_PANEL_UNIVERSE_ID,
) -> pd.DataFrame:
    """Assemble the long factor panel with decision-date PIT gating.

    ``as_of_date`` filters to panel decision dates up to that date. If ``as_of_ts``
    is omitted, availability is checked at end-of-day for the requested date.
    """

    frames = [
        _normalize_factor_surface(fundamental_factors),
        _normalize_factor_surface(cross_domain_factors),
    ]
    panel = pd.concat([frame for frame in frames if not frame.empty], ignore_index=True)
    if panel.empty:
        return _empty_panel()

    panel = _filter_asof(panel, as_of_date=as_of_date, as_of_ts=as_of_ts)
    panel = _apply_universe_filter(
        panel,
        universe_membership,
        universe_id=universe_id,
        as_of_ts=as_of_ts,
    )
    if panel.empty:
        return _empty_panel()

    panel = panel.sort_values(["as_of_date", "security_id", "factor_id", "available_at", "source_loaded_at"])
    panel = panel.drop_duplicates(["security_id", "as_of_date", "factor_id"], keep="last")
    return panel.loc[:, FACTOR_PANEL_COLUMNS].reset_index(drop=True)


def pivot_factor_panel_wide(panel_long: pd.DataFrame) -> pd.DataFrame:
    """Pivot a long factor panel into one row per security/date cross-section."""

    if panel_long.empty:
        return pd.DataFrame(columns=["security_id", "as_of_date"])
    missing = [column for column in ("security_id", "as_of_date", "factor_id", "value") if column not in panel_long.columns]
    if missing:
        raise ValueError(f"panel is missing required columns: {missing}")
    wide = panel_long.pivot_table(
        index=["security_id", "as_of_date"],
        columns="factor_id",
        values="value",
        aggfunc="last",
    )
    return wide.reset_index().sort_values(["as_of_date", "security_id"]).reset_index(drop=True)


def export_factor_panel(
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    lake_root: Path | str = DEFAULT_LAKE_ROOT,
    incremental: bool = True,
) -> LakeExportResult:
    """Export the long factor panel through the governed lake exporter."""

    return LakehouseExporter(db_path=db_path, lake_root=lake_root).export_objects(
        ("v_factor_panel",),
        incremental=incremental,
    )[0]
