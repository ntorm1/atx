"""PIT-safe factor dependency DAG and minimal compute engine."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
from dataclasses import dataclass
from typing import Iterable

import pandas as pd

from ..connection import DuckDBStore
from ..warehouse import insert_frame, json_dumps
from .catalog import FactorDefinition, factor_definitions_frame, legacy_factor_definitions, validate_catalog


SOURCE_NAME = "atx-impl factor engine"
FACTOR_VALUE_COLUMNS = [
    "factor_value_id",
    "factor_id",
    "factor_name",
    "family",
    "security_id",
    "symbol",
    "as_of_date",
    "value",
    "available_at",
    "input_ids_json",
    "input_lineage_json",
    "is_latest_revision",
    "run_id",
    "source",
]


class FactorGraphError(ValueError):
    """Raised when factor dependency graph ordering or compute fails."""


@dataclass(frozen=True)
class FactorBuildResult:
    frame: pd.DataFrame
    topological_order: tuple[str, ...]
    manifest: dict[str, object]


def _hash_id(prefix: str, *parts: object) -> str:
    payload = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(f"{prefix}|{payload}".encode("utf-8")).hexdigest()


def _parse_input_id(input_id: str) -> tuple[str, str]:
    kind, sep, value = input_id.partition(":")
    if not sep:
        return "factor", kind
    return kind, value


def factor_dependency_edges_frame(rows: Iterable[FactorDefinition] | None = None) -> pd.DataFrame:
    """Build deterministic dependency-edge rows from factor definitions."""

    definitions = tuple(legacy_factor_definitions() if rows is None else rows)
    validate_catalog(definitions)
    edge_rows: list[dict[str, object]] = []
    for definition in sorted(definitions, key=lambda item: item.factor_id):
        for input_id in definition.input_ids:
            dependency_type, dependency_name = _parse_input_id(input_id)
            edge_rows.append(
                {
                    "dependency_id": _hash_id("factor_dependency", definition.factor_id, dependency_type, dependency_name),
                    "factor_id": definition.factor_id,
                    "dependency_type": dependency_type,
                    "dependency_name": dependency_name,
                    "dependency_factor_id": dependency_name if dependency_type == "factor" else None,
                    "dependency_metric_id": dependency_name if dependency_type in {"metric", "ratio"} else None,
                    "dependency_source_id": dependency_name if dependency_type == "source" else None,
                    "dependency_depth": 1,
                    "expression": definition.expression,
                    "lookback_days": definition.lookback_days,
                    "is_direct": True,
                    "source": SOURCE_NAME,
                }
            )
    if not edge_rows:
        return pd.DataFrame(
            columns=[
                "dependency_id",
                "factor_id",
                "dependency_type",
                "dependency_name",
                "dependency_factor_id",
                "dependency_metric_id",
                "dependency_source_id",
                "dependency_depth",
                "expression",
                "lookback_days",
                "is_direct",
                "source",
            ]
        )
    return pd.DataFrame(edge_rows).drop_duplicates(subset=["dependency_id"]).sort_values(
        ["factor_id", "dependency_type", "dependency_name"]
    ).reset_index(drop=True)


def _target_closure(factor_ids: set[str], graph: dict[str, set[str]]) -> set[str]:
    closure: set[str] = set()

    def visit(factor_id: str) -> None:
        if factor_id in closure:
            return
        closure.add(factor_id)
        for dependency in graph.get(factor_id, set()):
            visit(dependency)

    for factor_id in factor_ids:
        visit(factor_id)
    return closure


def topological_factor_order(
    rows: Iterable[FactorDefinition],
    *,
    target_factor_ids: Iterable[str] | None = None,
) -> tuple[str, ...]:
    """Return dependency-first factor order, rejecting cycles."""

    definitions = {row.factor_id: row for row in rows}
    validate_catalog(definitions.values())
    graph: dict[str, set[str]] = {factor_id: set() for factor_id in definitions}
    for definition in definitions.values():
        for input_id in definition.input_ids:
            dependency_type, dependency_name = _parse_input_id(input_id)
            if dependency_type == "factor":
                graph[definition.factor_id].add(dependency_name)

    targets = set(definitions if target_factor_ids is None else target_factor_ids)
    unknown_targets = sorted(targets - set(definitions))
    if unknown_targets:
        raise FactorGraphError(f"Unknown target factors: {unknown_targets}")
    included = _target_closure(targets, graph)
    order: list[str] = []
    visiting: list[str] = []
    visited: set[str] = set()

    def visit(factor_id: str) -> None:
        if factor_id in visited or factor_id not in included:
            return
        if factor_id in visiting:
            cycle = visiting[visiting.index(factor_id) :] + [factor_id]
            raise FactorGraphError("Cyclic factor dependency: " + " -> ".join(cycle))
        visiting.append(factor_id)
        for dependency in sorted(graph.get(factor_id, set())):
            visit(dependency)
        visiting.pop()
        visited.add(factor_id)
        order.append(factor_id)

    for factor_id in sorted(included):
        visit(factor_id)
    return tuple(order)


def _normalize_factor_values(frame: pd.DataFrame) -> pd.DataFrame:
    if frame.empty:
        return pd.DataFrame(columns=["factor_id", "security_id", "symbol", "as_of_date", "value", "available_at"])
    out = frame.copy()
    if "symbol" not in out.columns:
        out["symbol"] = pd.NA
    out["factor_id"] = out["factor_id"].astype("string")
    out["security_id"] = out["security_id"].astype("string")
    out["as_of_date"] = pd.to_datetime(out["as_of_date"], errors="coerce").dt.date
    out["available_at"] = pd.to_datetime(out["available_at"], errors="coerce")
    out["value"] = pd.to_numeric(out["value"], errors="coerce")
    out = out.dropna(subset=["factor_id", "security_id", "as_of_date", "available_at", "value"])
    return out[["factor_id", "security_id", "symbol", "as_of_date", "value", "available_at"]].reset_index(drop=True)


def _factor_value_id(factor_id: str, security_id: str, as_of_date: object, available_at: object, run_id: str | None) -> str:
    return _hash_id("factor_value", factor_id, security_id, as_of_date, available_at, run_id)


def _lineage_for(group: pd.DataFrame, dependencies: tuple[str, ...]) -> str:
    lineage = []
    by_factor = {str(row.factor_id): row for row in group.itertuples(index=False)}
    for dependency in dependencies:
        row = by_factor[dependency]
        lineage.append(
            {
                "factor_id": dependency,
                "security_id": str(row.security_id),
                "as_of_date": row.as_of_date.isoformat() if isinstance(row.as_of_date, dt.date) else str(row.as_of_date),
                "available_at": pd.Timestamp(row.available_at).isoformat(),
                "value": float(row.value),
            }
        )
    return json_dumps(lineage)


def compute_factor_rows(
    input_values: pd.DataFrame,
    rows: Iterable[FactorDefinition],
    *,
    target_factor_ids: Iterable[str],
    run_id: str | None = None,
    source: str = SOURCE_NAME,
) -> FactorBuildResult:
    """Compute simple expression factors in dependency order from PIT input rows."""

    definitions = {row.factor_id: row for row in rows}
    order = topological_factor_order(definitions.values(), target_factor_ids=target_factor_ids)
    targets = set(target_factor_ids)
    available = _normalize_factor_values(input_values)
    outputs: list[pd.DataFrame] = []

    for factor_id in order:
        if factor_id in set(available["factor_id"].astype(str)):
            continue
        definition = definitions[factor_id]
        factor_dependencies = tuple(
            value for kind, value in (_parse_input_id(input_id) for input_id in definition.input_ids) if kind == "factor"
        )
        if not factor_dependencies:
            continue
        missing = sorted(set(factor_dependencies) - set(available["factor_id"].astype(str)))
        if missing:
            raise FactorGraphError(f"{factor_id} missing computed inputs: {missing}")
        needed = available[available["factor_id"].isin(factor_dependencies)].copy()
        group_sizes = needed.groupby(["security_id", "as_of_date"])["factor_id"].nunique()
        complete_keys = group_sizes[group_sizes == len(factor_dependencies)].reset_index()[["security_id", "as_of_date"]]
        if complete_keys.empty:
            continue
        needed = needed.merge(complete_keys, on=["security_id", "as_of_date"], how="inner")
        # Reduce to one row per (factor_id, security_id, symbol, as_of_date) --
        # the latest-available revision -- BEFORE pivoting, so `value` and
        # `available_at` are always derived from the same revision instead of
        # `pivot_table(aggfunc="last")` picking an arbitrary (input-order
        # dependent) revision while availability is separately `max()`-ed over
        # all revisions. Mirrors the fundamental_families.py latest-revision
        # pattern; "value" is the deterministic tiebreak since this frame has
        # no unique row id.
        needed = (
            needed.sort_values(
                ["factor_id", "security_id", "symbol", "as_of_date", "available_at", "value"],
                kind="mergesort",
                na_position="first",
            )
            .groupby(["factor_id", "security_id", "symbol", "as_of_date"], as_index=False, dropna=False)
            .tail(1)
        )
        value_wide = needed.pivot_table(
            index=["security_id", "symbol", "as_of_date"],
            columns="factor_id",
            values="value",
            aggfunc="last",
            dropna=False,
        )
        try:
            values = value_wide.eval(definition.expression, engine="python")
        except Exception as exc:  # pragma: no cover - exercised by future operator tests.
            raise FactorGraphError(f"{factor_id} expression is not supported by the minimal S7-1 engine") from exc
        availability = (
            needed.groupby(["security_id", "symbol", "as_of_date"], dropna=False)["available_at"].max().rename("available_at")
        )
        lineage_rows = []
        for key, group in needed.groupby(["security_id", "symbol", "as_of_date"], dropna=False):
            security_id, symbol, as_of_date = key
            lineage_rows.append(
                {
                    "security_id": security_id,
                    "symbol": symbol,
                    "as_of_date": as_of_date,
                    "input_lineage_json": _lineage_for(group, factor_dependencies),
                }
            )
        lineage = pd.DataFrame(lineage_rows).set_index(["security_id", "symbol", "as_of_date"])[
            "input_lineage_json"
        ]
        derived = pd.concat([values.rename("value"), availability, lineage], axis=1).reset_index()
        derived = derived.dropna(subset=["value", "available_at"])
        if derived.empty:
            continue
        derived["factor_id"] = factor_id
        derived["factor_name"] = definition.factor_name
        derived["family"] = definition.family
        derived["input_ids_json"] = definition.input_ids_json
        derived["is_latest_revision"] = True
        derived["run_id"] = run_id
        derived["source"] = source
        derived["factor_value_id"] = [
            _factor_value_id(factor_id, row.security_id, row.as_of_date, row.available_at, run_id)
            for row in derived.itertuples(index=False)
        ]
        available = pd.concat(
            [
                available,
                derived[["factor_id", "security_id", "symbol", "as_of_date", "value", "available_at"]],
            ],
            ignore_index=True,
        )
        if factor_id in targets:
            outputs.append(derived[FACTOR_VALUE_COLUMNS])

    frame = pd.concat(outputs, ignore_index=True) if outputs else pd.DataFrame(columns=FACTOR_VALUE_COLUMNS)
    manifest = {
        "manifest_id": _hash_id("factor_build_manifest", tuple(sorted(targets)), run_id, len(input_values), len(frame)),
        "run_id": run_id,
        "factor_ids": sorted(targets),
        "topological_order": list(order),
        "input_row_count": int(len(input_values)),
        "output_row_count": int(len(frame)),
        "min_as_of_date": None if frame.empty else frame["as_of_date"].min(),
        "max_as_of_date": None if frame.empty else frame["as_of_date"].max(),
        "min_available_at": None if frame.empty else frame["available_at"].min(),
        "max_available_at": None if frame.empty else frame["available_at"].max(),
        "source": source,
    }
    return FactorBuildResult(frame=frame, topological_order=order, manifest=manifest)


def _definition_rows_from_store(store: DuckDBStore) -> tuple[FactorDefinition, ...]:
    frame = store.con.execute(
        """
        SELECT
            factor_id,
            factor_name,
            family,
            description,
            expression,
            input_ids_json,
            direction,
            lookback_days,
            neutralization_spec_json,
            unit,
            sign,
            scale,
            is_point_in_time_safe,
            available_at_policy,
            declared_in,
            owner,
            source
        FROM factor_definition
        ORDER BY factor_id
        """
    ).df()
    if frame.empty:
        return ()
    return tuple(FactorDefinition(**row) for row in frame.to_dict(orient="records"))


def refresh_factor_dependency_graph(store: DuckDBStore) -> dict[str, int]:
    """Refresh factor DAG edges from ``factor_definition`` into ``factor_dependency_edges``."""

    rows = _definition_rows_from_store(store)
    edges = factor_dependency_edges_frame(rows)
    with store.transaction():
        store.con.execute("DELETE FROM factor_dependency_edges")
        if not edges.empty:
            insert_frame(store, edges, "factor_dependency_edges", "factor_dependency_edges_insert")
    return {"factor_definitions": len(rows), "dependency_edges": int(len(edges))}


def factor_build_manifest_frame(result: FactorBuildResult) -> pd.DataFrame:
    manifest = result.manifest
    return pd.DataFrame(
        [
            {
                "manifest_id": manifest["manifest_id"],
                "run_id": manifest["run_id"],
                "factor_ids_json": json_dumps(manifest["factor_ids"]),
                "topological_order_json": json_dumps(manifest["topological_order"]),
                "input_row_count": manifest["input_row_count"],
                "output_row_count": manifest["output_row_count"],
                "output_min_as_of_date": manifest["min_as_of_date"],
                "output_max_as_of_date": manifest["max_as_of_date"],
                "min_available_at": manifest["min_available_at"],
                "max_available_at": manifest["max_available_at"],
                "params_json": json_dumps({}),
                "source": manifest["source"],
            }
        ]
    )


def legacy_factor_dependency_edges_frame() -> pd.DataFrame:
    """Return S7 migration seed edges for legacy factor definitions."""

    return factor_dependency_edges_frame(legacy_factor_definitions())


def legacy_factor_definitions_frame() -> pd.DataFrame:
    """Compatibility helper for migration/tests needing the seed frame."""

    return factor_definitions_frame(legacy_factor_definitions())
