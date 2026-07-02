from __future__ import annotations

import datetime as dt
import heapq
import json
import uuid
from dataclasses import dataclass
from typing import Any, Mapping


RegistryEntry = type[Any] | tuple[type[Any], Any]


class CycleError(ValueError):
    """Raised when declared dataset dependencies contain a cycle."""

    def __init__(self, path: tuple[str, ...]) -> None:
        self.path = path
        super().__init__(
            "Cycle detected in dataset dependencies: " + " -> ".join(path)
        )


class MissingDependencyError(ValueError):
    """Raised when a dataset depends on an unknown dataset_id."""


@dataclass(frozen=True)
class DatasetDAG:
    dependencies: dict[str, tuple[str, ...]]
    children: dict[str, tuple[str, ...]]
    order: tuple[str, ...]

    def dependencies_of(self, dataset_id: str) -> tuple[str, ...]:
        return self.dependencies[dataset_id]

    def children_of(self, dataset_id: str) -> tuple[str, ...]:
        return self.children[dataset_id]


@dataclass(frozen=True)
class RunManifest:
    run_id: str
    dataset_order: tuple[str, ...]


def _dataset_class(entry: RegistryEntry) -> type[Any]:
    if isinstance(entry, tuple):
        if not entry:
            raise ValueError("DATASET_REGISTRY entry tuple is empty")
        return entry[0]
    return entry


def declared_dependencies(dataset_cls: type[Any]) -> tuple[str, ...]:
    raw = getattr(dataset_cls, "depends_on", ())
    if raw is None:
        return ()
    if isinstance(raw, str):
        raise TypeError(
            f"{dataset_cls.__name__}.depends_on must be a tuple[str, ...], not str"
        )
    return tuple(sorted({str(dataset_id) for dataset_id in raw if str(dataset_id)}))


def build_dataset_dag(registry: Mapping[str, RegistryEntry]) -> DatasetDAG:
    """Build a deterministic DAG from dataset_id keyed registry metadata."""

    dependencies: dict[str, tuple[str, ...]] = {}
    for dataset_id, entry in registry.items():
        dataset_cls = _dataset_class(entry)
        declared_id = getattr(dataset_cls, "dataset_id", dataset_id)
        if declared_id != dataset_id:
            raise ValueError(
                f"DATASET_REGISTRY key {dataset_id!r} does not match "
                f"{dataset_cls.__name__}.dataset_id {declared_id!r}"
            )
        dependencies[dataset_id] = declared_dependencies(dataset_cls)

    missing = sorted(
        {
            dependency
            for dataset_dependencies in dependencies.values()
            for dependency in dataset_dependencies
            if dependency not in dependencies
        }
    )
    if missing:
        raise MissingDependencyError(
            "Dataset dependencies reference unknown dataset_id(s): "
            + ", ".join(missing)
        )

    children: dict[str, list[str]] = {dataset_id: [] for dataset_id in dependencies}
    for dataset_id, dataset_dependencies in dependencies.items():
        for dependency in dataset_dependencies:
            children[dependency].append(dataset_id)

    visiting: dict[str, int] = {}
    visited: set[str] = set()
    stack: list[str] = []

    def check_cycles(dataset_id: str) -> None:
        if dataset_id in visited:
            return
        if dataset_id in visiting:
            start = visiting[dataset_id]
            raise CycleError(tuple([*stack[start:], dataset_id]))
        visiting[dataset_id] = len(stack)
        stack.append(dataset_id)
        for dependency in dependencies[dataset_id]:
            check_cycles(dependency)
        stack.pop()
        visiting.pop(dataset_id, None)
        visited.add(dataset_id)

    for dataset_id in sorted(dependencies):
        check_cycles(dataset_id)

    ordered_children = {
        dataset_id: tuple(sorted(dataset_children))
        for dataset_id, dataset_children in children.items()
    }
    remaining_dependencies = {
        dataset_id: len(dataset_dependencies)
        for dataset_id, dataset_dependencies in dependencies.items()
    }
    ready = [
        dataset_id
        for dataset_id, dependency_count in remaining_dependencies.items()
        if dependency_count == 0
    ]
    heapq.heapify(ready)
    order: list[str] = []
    while ready:
        dataset_id = heapq.heappop(ready)
        order.append(dataset_id)
        for child in ordered_children[dataset_id]:
            remaining_dependencies[child] -= 1
            if remaining_dependencies[child] == 0:
                heapq.heappush(ready, child)

    return DatasetDAG(
        dependencies=dependencies,
        children=ordered_children,
        order=tuple(order),
    )


def _now_utc_naive() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)


def _json_dumps(value: Any) -> str:
    return json.dumps(value, default=str, sort_keys=True)


def create_run_manifest(
    store: Any,
    dag: DatasetDAG,
    *,
    run_id: str | None = None,
    params: Mapping[str, Any] | None = None,
    git_sha: str | None = None,
    actor: str = "orchestrator",
    started_at: dt.datetime | None = None,
) -> RunManifest:
    """Create the S2 parent run row, pending step rows, and run_start audit row."""

    manifest_run_id = run_id or str(uuid.uuid4())
    ts = started_at or _now_utc_naive()
    params_json = _json_dumps(dict(params or {}))
    con = store.con

    con.execute(
        """
        INSERT INTO etl_job_runs (
            job_run_id,
            run_id,
            run_kind,
            parent_run_id,
            job_name,
            dataset_id,
            status,
            started_at,
            attempt_count,
            max_retries,
            retry_delay_seconds,
            params_json,
            git_sha
        )
        VALUES (?, ?, 'orchestrator', NULL, 'refresh_quant_warehouse',
                '__orchestrator__', 'running', ?, 0, 0, 0, ?, ?)
        """,
        [manifest_run_id, manifest_run_id, ts, params_json, git_sha],
    )

    for dataset_id in dag.order:
        con.execute(
            """
            INSERT INTO etl_job_steps (
                run_id,
                dataset_id,
                status,
                rows,
                started_at,
                finished_at,
                watermark_before,
                watermark_after,
                error
            )
            VALUES (?, ?, 'pending', NULL, NULL, NULL, NULL, NULL, NULL)
            """,
            [manifest_run_id, dataset_id],
        )

    record_audit(
        store,
        run_id=manifest_run_id,
        dataset_id=None,
        actor=actor,
        action="run_start",
        details={"dataset_order": list(dag.order), "dataset_count": len(dag.order)},
        ts=ts,
    )
    return RunManifest(run_id=manifest_run_id, dataset_order=dag.order)


def record_audit(
    store: Any,
    *,
    run_id: str,
    dataset_id: str | None,
    actor: str,
    action: str,
    details: Mapping[str, Any] | None = None,
    ts: dt.datetime | None = None,
) -> str:
    audit_id = str(uuid.uuid4())
    store.con.execute(
        """
        INSERT INTO etl_job_audit (
            audit_id,
            run_id,
            dataset_id,
            actor,
            ts,
            action,
            details_json
        )
        VALUES (?, ?, ?, ?, ?, ?, ?)
        """,
        [
            audit_id,
            run_id,
            dataset_id,
            actor,
            ts or _now_utc_naive(),
            action,
            _json_dumps(dict(details or {})),
        ],
    )
    return audit_id
