"""Backtest-ready factor panel assembly helpers.

PF3-S10 consumes the S8/S9 factor-value surfaces and exposes the S2 panel
contract on decision dates. Source factor rows keep their own observation date;
the panel's ``as_of_date`` is the first date the row is safe to consume.
"""

from __future__ import annotations

import datetime as dt
import argparse
import json
from pathlib import Path
import sys
from typing import Any, Iterable

import pandas as pd

from .connection import DEFAULT_DB_PATH, connect
from .lake import DEFAULT_LAKE_ROOT, LakeExportResult, LakehouseExporter
from .panel_contract import PANEL_CONTRACT, PANEL_CONTRACT_SHA256

DEFAULT_FACTOR_PANEL_UNIVERSE_ID = "us_common_equity_liquid_v1"
PANEL_EXPORT_GATE_CHECK_NAME = "factor_panel_export_contract"

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

READ_PANEL_COLUMNS = FACTOR_PANEL_COLUMNS

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

    panel = panel.sort_values(
        ["as_of_date", "security_id", "factor_id", "available_at", "source_loaded_at", "run_id"],
        kind="mergesort",
    )
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


def _duckdb_columns(store, object_name: str) -> list[dict[str, object]]:
    rows = store.con.execute(
        """
        SELECT column_name, data_type, column_index
        FROM duckdb_columns()
        WHERE schema_name = 'main'
          AND table_name = ?
        ORDER BY column_index
        """,
        [object_name],
    ).fetchall()
    return [
        {"name": str(name), "data_type": str(data_type), "ordinal": int(ordinal)}
        for name, data_type, ordinal in rows
    ]


def _table_or_view_exists(store, object_name: str) -> bool:
    return bool(_duckdb_columns(store, object_name))


def factor_panel_contract_violations(store) -> list[dict[str, object]]:
    """Return panel-contract violations for ``v_factor_panel``."""

    violations: list[dict[str, object]] = []
    actual_columns = _duckdb_columns(store, "v_factor_panel")
    if not actual_columns:
        return [{"kind": "missing_panel_view", "object_name": "v_factor_panel"}]

    expected = [
        {
            "name": spec.name,
            "data_type": spec.data_type,
            "unit": spec.unit,
            "sign": spec.sign,
            "scale": spec.scale,
        }
        for spec in PANEL_CONTRACT
    ]
    expected_names = [str(item["name"]) for item in expected]
    actual_names = [str(item["name"]) for item in actual_columns]
    if actual_names != expected_names:
        violations.append(
            {
                "kind": "panel_column_shape_mismatch",
                "expected_columns": expected_names,
                "actual_columns": actual_names,
                "missing_columns": [name for name in expected_names if name not in actual_names],
                "extra_columns": [name for name in actual_names if name not in expected_names],
            }
        )

    actual_by_name = {str(item["name"]): item for item in actual_columns}
    for spec in expected:
        actual = actual_by_name.get(str(spec["name"]))
        if actual is not None and str(actual["data_type"]).upper() != str(spec["data_type"]).upper():
            violations.append(
                {
                    "kind": "panel_column_type_mismatch",
                    "column_name": spec["name"],
                    "expected_data_type": spec["data_type"],
                    "actual_data_type": actual["data_type"],
                }
            )

    if not _table_or_view_exists(store, "panel_contract"):
        violations.append({"kind": "missing_panel_contract_table"})
        return violations

    contract_rows = store.con.execute(
        """
        SELECT column_name, data_type, unit, sign, scale, panel_contract_sha256
        FROM panel_contract
        """
    ).fetchall()
    persisted = {
        str(column_name): {
            "data_type": str(data_type),
            "unit": str(unit),
            "sign": str(sign),
            "scale": str(scale),
            "panel_contract_sha256": str(panel_contract_sha256),
        }
        for column_name, data_type, unit, sign, scale, panel_contract_sha256 in contract_rows
    }
    if set(persisted) != set(expected_names):
        violations.append(
            {
                "kind": "panel_contract_row_mismatch",
                "expected_columns": expected_names,
                "persisted_columns": sorted(persisted),
            }
        )
    for spec in expected:
        row = persisted.get(str(spec["name"]))
        if row is None:
            continue
        for key in ("data_type", "unit", "sign", "scale"):
            if str(row[key]).lower() != str(spec[key]).lower():
                violations.append(
                    {
                        "kind": "panel_contract_metadata_mismatch",
                        "column_name": spec["name"],
                        "field": key,
                        "expected": spec[key],
                        "actual": row[key],
                    }
                )
        if row["panel_contract_sha256"] != PANEL_CONTRACT_SHA256:
            violations.append(
                {
                    "kind": "panel_contract_hash_mismatch",
                    "column_name": spec["name"],
                    "expected": PANEL_CONTRACT_SHA256,
                    "actual": row["panel_contract_sha256"],
                }
            )
    return violations


def _iter_available_at_values(value: Any) -> Iterable[object]:
    if isinstance(value, dict):
        for key, nested in value.items():
            if key == "available_at":
                yield nested
            else:
                yield from _iter_available_at_values(nested)
    elif isinstance(value, list):
        for item in value:
            yield from _iter_available_at_values(item)


def _lineage_lookahead_violations(store, *, max_detail_rows: int) -> tuple[int, list[dict[str, object]]]:
    rows = store.con.execute(
        """
        SELECT security_id, as_of_date, factor_id, input_lineage_json
        FROM v_factor_panel
        WHERE input_lineage_json IS NOT NULL
        """
    ).fetchall()
    count = 0
    details: list[dict[str, object]] = []
    for security_id, as_of_date, factor_id, lineage_json in rows:
        try:
            parsed = json.loads(str(lineage_json))
        except Exception as exc:
            count += 1
            if len(details) < max_detail_rows:
                details.append(
                    {
                        "kind": "invalid_input_lineage_json",
                        "security_id": str(security_id),
                        "as_of_date": str(as_of_date),
                        "factor_id": str(factor_id),
                        "error": str(exc),
                    }
                )
            continue
        for available_at in _iter_available_at_values(parsed):
            try:
                available_date = pd.Timestamp(available_at).date()
            except Exception as exc:
                count += 1
                if len(details) < max_detail_rows:
                    details.append(
                        {
                            "kind": "invalid_lineage_available_at",
                            "security_id": str(security_id),
                            "as_of_date": str(as_of_date),
                            "factor_id": str(factor_id),
                            "available_at": str(available_at),
                            "error": str(exc),
                        }
                    )
                continue
            if available_date > as_of_date:
                count += 1
                if len(details) < max_detail_rows:
                    details.append(
                        {
                            "kind": "lineage_lookahead",
                            "security_id": str(security_id),
                            "as_of_date": str(as_of_date),
                            "factor_id": str(factor_id),
                            "available_at": str(available_at),
                        }
                    )
    return count, details


def factor_panel_export_gate_report(store, *, max_detail_rows: int = 50) -> dict[str, object]:
    """Return S10 export-boundary contract/lookahead gate diagnostics."""

    violations = factor_panel_contract_violations(store)
    if any(item["kind"] == "missing_panel_view" for item in violations):
        return {"violation_count": float(len(violations)), "violations": violations}

    required = {"security_id", "as_of_date", "factor_id", "value", "available_at", "input_lineage_json"}
    actual = {str(item["name"]) for item in _duckdb_columns(store, "v_factor_panel")}
    if not required <= actual:
        return {"violation_count": float(len(violations)), "violations": violations}

    future_count = int(
        store.con.execute(
            """
            SELECT count(*)
            FROM v_factor_panel
            WHERE CAST(available_at AS DATE) > as_of_date
            """
        ).fetchone()[0]
    )
    if future_count:
        detail_rows = store.con.execute(
            """
            SELECT security_id, as_of_date, factor_id, available_at
            FROM v_factor_panel
            WHERE CAST(available_at AS DATE) > as_of_date
            ORDER BY security_id, as_of_date, factor_id
            LIMIT ?
            """,
            [max_detail_rows],
        ).fetchall()
        violations.append(
            {
                "kind": "panel_available_at_lookahead",
                "count": future_count,
                "rows": [
                    {
                        "security_id": str(security_id),
                        "as_of_date": str(as_of_date),
                        "factor_id": str(factor_id),
                        "available_at": str(available_at),
                    }
                    for security_id, as_of_date, factor_id, available_at in detail_rows
                ],
            }
        )

    lineage_count, lineage_details = _lineage_lookahead_violations(store, max_detail_rows=max_detail_rows)
    if lineage_count:
        violations.append(
            {
                "kind": "panel_input_lineage_lookahead",
                "count": lineage_count,
                "rows": lineage_details,
            }
        )

    if not _table_or_view_exists(store, "universe_membership"):
        violations.append({"kind": "missing_universe_membership"})
    else:
        non_member_count = int(
            store.con.execute(
                """
                SELECT count(*)
                FROM v_factor_panel p
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM universe_membership u
                    WHERE u.universe_id = ?
                      AND u.security_id = p.security_id
                      AND u.valid_from <= p.as_of_date
                      AND (u.valid_to IS NULL OR u.valid_to >= p.as_of_date)
                      AND u.as_of_date <= p.as_of_date
                      AND u.is_member
                      AND u.is_latest_revision
                      AND (
                          u.available_at IS NULL
                          OR CAST(u.available_at AS DATE) <= p.as_of_date
                      )
                )
                """,
                [DEFAULT_FACTOR_PANEL_UNIVERSE_ID],
            ).fetchone()[0]
        )
        if non_member_count:
            detail_rows = store.con.execute(
                """
                SELECT p.security_id, p.as_of_date, p.factor_id
                FROM v_factor_panel p
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM universe_membership u
                    WHERE u.universe_id = ?
                      AND u.security_id = p.security_id
                      AND u.valid_from <= p.as_of_date
                      AND (u.valid_to IS NULL OR u.valid_to >= p.as_of_date)
                      AND u.as_of_date <= p.as_of_date
                      AND u.is_member
                      AND u.is_latest_revision
                      AND (
                          u.available_at IS NULL
                          OR CAST(u.available_at AS DATE) <= p.as_of_date
                      )
                )
                ORDER BY p.security_id, p.as_of_date, p.factor_id
                LIMIT ?
                """,
                [DEFAULT_FACTOR_PANEL_UNIVERSE_ID, max_detail_rows],
            ).fetchall()
            violations.append(
                {
                    "kind": "panel_universe_membership_violation",
                    "count": non_member_count,
                    "rows": [
                        {
                            "security_id": str(security_id),
                            "as_of_date": str(as_of_date),
                            "factor_id": str(factor_id),
                        }
                        for security_id, as_of_date, factor_id in detail_rows
                    ],
                }
            )

    observed = 0.0
    for item in violations:
        observed += float(item.get("count", 1.0))
    return {"violation_count": observed, "violations": violations}


def assert_factor_panel_export_ready(store) -> None:
    """Raise if the factor panel violates its contract or PIT export boundary."""

    report = factor_panel_export_gate_report(store)
    violation_count = float(report["violation_count"])
    if violation_count == 0.0:
        return
    violations = report["violations"]
    kinds = sorted({str(item.get("kind", "unknown")) for item in violations})  # type: ignore[union-attr]
    raise ValueError(
        f"factor panel export gate failed with {violation_count:g} violation(s): "
        + ", ".join(kinds)
    )


def _normalize_filter_values(values: Iterable[str] | None) -> list[str]:
    if values is None:
        return []
    return [str(value) for value in values if str(value)]


def _read_panel_asof_active(
    active,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime,
    factor_ids: Iterable[str] | None,
    security_ids: Iterable[str] | None,
) -> pd.DataFrame:
    factor_values = _normalize_filter_values(factor_ids)
    security_values = _normalize_filter_values(security_ids)
    registered: list[str] = []
    joins = []
    try:
        if factor_values:
            frame = pd.DataFrame({"factor_id": factor_values})
            active.con.register("factor_panel_factor_filter", frame)
            registered.append("factor_panel_factor_filter")
            joins.append("JOIN factor_panel_factor_filter ff ON ff.factor_id = p.factor_id")
        if security_values:
            frame = pd.DataFrame({"security_id": security_values})
            active.con.register("factor_panel_security_filter", frame)
            registered.append("factor_panel_security_filter")
            joins.append("JOIN factor_panel_security_filter sf ON sf.security_id = p.security_id")
        sql = f"""
            WITH params AS (
                SELECT CAST(? AS DATE) AS as_of_date,
                       CAST(? AS TIMESTAMP) AS as_of_ts
            ),
            eligible AS (
                SELECT p.*
                FROM v_factor_panel p
                {' '.join(joins)}
                CROSS JOIN params
                WHERE p.as_of_date <= params.as_of_date
                  AND p.available_at <= params.as_of_ts
            ),
            ranked AS (
                SELECT
                    security_id,
                    factor_id,
                    value,
                    available_at,
                    source_loaded_at,
                    run_id,
                    input_lineage_json,
                    row_number() OVER (
                        PARTITION BY security_id, factor_id
                        ORDER BY as_of_date DESC,
                                 available_at DESC,
                                 source_loaded_at DESC
                    ) AS rn
                FROM eligible
            )
            SELECT
                security_id,
                CAST(? AS DATE) AS as_of_date,
                factor_id,
                value,
                available_at,
                source_loaded_at,
                run_id,
                input_lineage_json
            FROM ranked
            WHERE rn = 1
            ORDER BY security_id, factor_id
        """
        return active.con.execute(sql, [as_of_date, as_of_ts, as_of_date]).df()
    finally:
        for relation in registered:
            active.con.unregister(relation)


def read_panel_asof(
    as_of_date: dt.date | str,
    *,
    as_of_ts: dt.datetime | str | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    store=None,
    factor_ids: Iterable[str] | None = None,
    security_ids: Iterable[str] | None = None,
    wide: bool = False,
) -> pd.DataFrame:
    """Read the latest PIT-safe factor cross-section for a decision date."""

    resolved_date = pd.Timestamp(as_of_date).date()
    resolved_ts = pd.Timestamp(as_of_ts).to_pydatetime() if as_of_ts is not None else _end_of_day(resolved_date).to_pydatetime()

    if store is not None:
        long = _read_panel_asof_active(
            store,
            as_of_date=resolved_date,
            as_of_ts=resolved_ts,
            factor_ids=factor_ids,
            security_ids=security_ids,
        )
    else:
        with connect(db_path, read_only=False) as opened:
            long = _read_panel_asof_active(
                opened,
                as_of_date=resolved_date,
                as_of_ts=resolved_ts,
                factor_ids=factor_ids,
                security_ids=security_ids,
            )
    if wide:
        return pivot_factor_panel_wide(long)
    return long.loc[:, READ_PANEL_COLUMNS] if not long.empty else pd.DataFrame(columns=READ_PANEL_COLUMNS)


def describe_factor_panel(db_path: Path | str = DEFAULT_DB_PATH, *, store=None) -> dict[str, object]:
    """Return lightweight catalog and row-count metadata for the factor panel."""

    def _run(active) -> dict[str, object]:
        row = active.con.execute(
            """
            SELECT
                count(*)::BIGINT AS row_count,
                count(DISTINCT security_id)::BIGINT AS security_count,
                count(DISTINCT factor_id)::BIGINT AS factor_count,
                min(as_of_date) AS min_as_of_date,
                max(as_of_date) AS max_as_of_date
            FROM v_factor_panel
            """
        ).fetchone()
        contract = active.con.execute(
            """
            SELECT expected_schema_sha256
            FROM lake_export_object_contract
            WHERE object_name = 'v_factor_panel'
            """
        ).fetchone()
        partition = active.con.execute(
            """
            SELECT partition_columns_json, watermark_column
            FROM lake_partition_specs
            WHERE object_name = 'v_factor_panel'
            """
        ).fetchone()
        return {
            "object_name": "v_factor_panel",
            "row_count": int(row[0]),
            "security_count": int(row[1]),
            "factor_count": int(row[2]),
            "min_as_of_date": None if row[3] is None else str(row[3]),
            "max_as_of_date": None if row[4] is None else str(row[4]),
            "expected_schema_sha256": None if contract is None else str(contract[0]),
            "partition_columns": [] if partition is None else json.loads(str(partition[0])),
            "watermark_column": None if partition is None else str(partition[1]),
            "panel_contract_sha256": PANEL_CONTRACT_SHA256,
        }

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=False) as opened:
        return _run(opened)


def export_factor_panel(
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    lake_root: Path | str = DEFAULT_LAKE_ROOT,
    incremental: bool = True,
) -> LakeExportResult:
    """Export the long factor panel through the governed lake exporter."""

    with connect(db_path, read_only=False) as store:
        assert_factor_panel_export_ready(store)
    return LakehouseExporter(db_path=db_path, lake_root=lake_root).export_objects(
        ("v_factor_panel",),
        incremental=incremental,
    )[0]


def _parse_date(value: str) -> dt.date:
    return pd.Timestamp(value).date()


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Read and export the governed PF3 factor panel.")
    parser.add_argument("--db-path", default=str(DEFAULT_DB_PATH))
    subparsers = parser.add_subparsers(dest="command", required=True)

    export_parser = subparsers.add_parser("export", help="Export v_factor_panel to the lake.")
    export_parser.add_argument("--lake-root", default=str(DEFAULT_LAKE_ROOT))
    export_parser.add_argument("--full", action="store_true", help="Disable incremental partition skipping.")

    read_parser = subparsers.add_parser("read", help="Print a PIT factor cross-section as CSV.")
    read_parser.add_argument("--as-of", required=True, type=_parse_date)
    read_parser.add_argument("--as-of-ts")
    read_parser.add_argument("--wide", action="store_true")
    read_parser.add_argument("--factor-id", action="append", dest="factor_ids")
    read_parser.add_argument("--security-id", action="append", dest="security_ids")
    read_parser.add_argument("--limit", type=int, default=0)

    subparsers.add_parser("describe", help="Print panel metadata as JSON.")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    db_path = Path(args.db_path)
    if args.command == "export":
        result = export_factor_panel(
            db_path,
            lake_root=Path(args.lake_root),
            incremental=not args.full,
        )
        payload = {
            "export_run_id": result.export_run_id,
            "object_name": result.object_name,
            "rows": result.rows,
            "byte_count": result.byte_count,
            "sha256": result.sha256,
            "output_path": str(result.output_path),
            "manifest_path": str(result.manifest_path),
        }
        sys.stdout.write(json.dumps(payload, sort_keys=True) + "\n")
        return 0
    if args.command == "read":
        frame = read_panel_asof(
            args.as_of,
            as_of_ts=args.as_of_ts,
            db_path=db_path,
            factor_ids=args.factor_ids,
            security_ids=args.security_ids,
            wide=args.wide,
        )
        if args.limit and args.limit > 0:
            frame = frame.head(args.limit)
        sys.stdout.write(frame.to_csv(index=False))
        return 0
    if args.command == "describe":
        sys.stdout.write(json.dumps(describe_factor_panel(db_path), sort_keys=True) + "\n")
        return 0
    parser.error(f"Unsupported command: {args.command}")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
