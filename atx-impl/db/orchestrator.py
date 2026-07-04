from __future__ import annotations

import datetime as dt
import heapq
import json
import time
import traceback
import uuid
from dataclasses import dataclass
from typing import Any, Callable, Mapping

from .quality import GateDecision, GateResult, evaluate_quality_gate


RegistryEntry = type[Any] | tuple[type[Any], Any]
OptionFactory = Callable[[dict[str, Any]], Any]
Clock = Callable[[], dt.datetime]
Sleeper = Callable[[float], None]
WINDOW_WATERMARK_HINTS = (
    "available_at",
    "as_of",
    "period",
    "trade_date",
    "effective_date",
    "filing_date",
    "updated_at",
    "observation_date",
    "report_period",
    "snapshot_date",
    "consensus_date",
    "guidance_date",
    "sale_date",
    "ex_date",
    "delist_date",
    "source_file_created_at",
    "finished_at",
)
INCREMENTAL_WINDOW_PARAM_KEYS = frozenset(
    ("incremental_since", "since", "as_of_ts", "start_date")
)
DATASET_PARAMS_KEY = "__dataset_params__"


class CycleError(ValueError):
    """Raised when declared dataset dependencies contain a cycle."""

    def __init__(self, path: tuple[str, ...]) -> None:
        self.path = path
        super().__init__(
            "Cycle detected in dataset dependencies: " + " -> ".join(path)
        )


class MissingDependencyError(ValueError):
    """Raised when a dataset depends on an unknown dataset_id."""


class OrchestratorRunError(RuntimeError):
    """Raised when an orchestrator step exhausts its retry policy."""


class QualityGateError(OrchestratorRunError):
    """Raised when an opt-in critical quality gate halts a dataset step."""

    def __init__(self, dataset_id: str, gate_result: GateResult) -> None:
        self.dataset_id = dataset_id
        self.gate_result = gate_result
        checks = ", ".join(result.check_name for result in gate_result.failed_results[:5])
        suffix = f": {checks}" if checks else ""
        super().__init__(f"Quality gate halted dataset {dataset_id!r}{suffix}")


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


@dataclass(frozen=True)
class RetryPolicy:
    max_retries: int = 0
    retry_delay_seconds: float = 0.0


@dataclass(frozen=True)
class OrchestratorResult:
    run_id: str
    status: str
    dataset_order: tuple[str, ...]


@dataclass(frozen=True)
class OrchestratorBackfillResult:
    run_id: str
    status: str
    dataset_order: tuple[str, ...]
    backfill_run_id: str
    partitions_planned: int
    partitions_succeeded: int
    partitions_skipped: int
    partitions_failed: int
    rows_written: int
    requested_max_parallel: int
    effective_max_parallel: int


def _quality_gate_details(gate_result: GateResult | None) -> dict[str, Any] | None:
    if gate_result is None:
        return None
    return {
        "dataset_id": gate_result.dataset_id,
        "decision": gate_result.decision,
        "worst_severity": gate_result.worst_severity,
        "result_count": gate_result.result_count,
        "failed_count": gate_result.failed_count,
        "failed_checks": [
            {
                "check_name": result.check_name,
                "status": result.status,
                "severity": result.severity,
                "observed_value": result.observed_value,
                "threshold_value": result.threshold_value,
            }
            for result in gate_result.failed_results
        ],
    }


def _dataset_class(entry: RegistryEntry) -> type[Any]:
    if isinstance(entry, tuple):
        if not entry:
            raise ValueError("DATASET_REGISTRY entry tuple is empty")
        return entry[0]
    return entry


def _option_factory(entry: RegistryEntry) -> OptionFactory:
    if isinstance(entry, tuple):
        if len(entry) < 2 or entry[1] is None:
            return lambda params: params
        return entry[1]
    return lambda params: params


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


def _nonnegative_int(value: Any, name: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise ValueError(f"{name} must be >= 0")
    return parsed


def _positive_int(value: Any, name: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise ValueError(f"{name} must be positive")
    return parsed


def _partition_parallelism_limits(
    max_parallel: int,
    *,
    allow_parallel_executor: bool,
) -> tuple[int, int]:
    requested_parallel = _positive_int(max_parallel, "max_parallel")
    effective_parallel = requested_parallel if allow_parallel_executor else 1
    return requested_parallel, effective_parallel


def _nonnegative_float(value: Any, name: str) -> float:
    parsed = float(value)
    if parsed < 0:
        raise ValueError(f"{name} must be >= 0")
    return parsed


def _date_prefix(value: str) -> str:
    return value.replace("T", " ").split(" ", 1)[0]


def _max_string(values: list[str]) -> str | None:
    if not values:
        return None
    return max(values)


def _incremental_since(watermark_sets: Mapping[str, Mapping[str, str]]) -> str | None:
    available_at_values = [
        value
        for marks in watermark_sets.values()
        for name, value in marks.items()
        if "available_at" in name
    ]
    if available_at_values:
        return _max_string(available_at_values)
    bounded_values = [
        value
        for marks in watermark_sets.values()
        for name, value in marks.items()
        if any(hint in name for hint in WINDOW_WATERMARK_HINTS)
    ]
    return _max_string(bounded_values)


def create_run_manifest(
    store: Any,
    dag: DatasetDAG,
    *,
    run_id: str | None = None,
    params: Mapping[str, Any] | None = None,
    git_sha: str | None = None,
    actor: str = "orchestrator",
    started_at: dt.datetime | None = None,
    run_kind: str = "orchestrator",
    job_name: str = "refresh_quant_warehouse",
    manifest_dataset_id: str = "__orchestrator__",
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
        VALUES (?, ?, ?, NULL, ?, ?, 'running', ?, 0, 0, 0, ?, ?)
        """,
        [
            manifest_run_id,
            manifest_run_id,
            run_kind,
            job_name,
            manifest_dataset_id,
            ts,
            params_json,
            git_sha,
        ],
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


class DatasetOrchestrator:
    """PF-S2 orchestration substrate over dataset registry metadata."""

    def __init__(
        self,
        store: Any,
        registry: Mapping[str, RegistryEntry],
        *,
        actor: str = "orchestrator",
        clock: Clock | None = None,
        sleeper: Sleeper | None = None,
        retry_policy_by_dataset: Mapping[str, RetryPolicy | Mapping[str, Any]] | None = None,
        default_max_retries: int = 0,
        default_retry_delay_seconds: float = 0.0,
        watermark_refresher: Callable[[Any], Any] | None = None,
    ) -> None:
        self.store = store
        self.registry = registry
        self.dag = build_dataset_dag(registry)
        self.actor = actor
        self.clock = clock or _now_utc_naive
        self.sleeper = sleeper or time.sleep
        self.retry_policy_by_dataset = dict(retry_policy_by_dataset or {})
        self.default_retry_policy = RetryPolicy(
            max_retries=_nonnegative_int(default_max_retries, "default_max_retries"),
            retry_delay_seconds=_nonnegative_float(
                default_retry_delay_seconds,
                "default_retry_delay_seconds",
            ),
        )
        self.watermark_refresher = watermark_refresher

    def run(
        self,
        *,
        run_id: str | None = None,
        params: Mapping[str, Any] | None = None,
        git_sha: str | None = None,
        full_rebuild: bool = False,
        gate: bool = False,
    ) -> OrchestratorResult:
        self._initialize_store()
        manifest = create_run_manifest(
            self.store,
            self.dag,
            run_id=run_id,
            params={**dict(params or {}), "full_rebuild": full_rebuild},
            git_sha=git_sha,
            actor=self.actor,
            started_at=self.clock(),
        )
        return self._execute(
            manifest.run_id,
            params=dict(params or {}),
            full_rebuild=full_rebuild,
            resume=False,
            gate=gate,
        )

    def resume(
        self,
        run_id: str,
        *,
        params: Mapping[str, Any] | None = None,
        full_rebuild: bool = False,
        gate: bool = False,
    ) -> OrchestratorResult:
        self._initialize_store()
        self._require_orchestrator_run(run_id)
        steps = self._step_statuses(run_id)
        missing_steps = sorted(set(self.dag.order) - set(steps))
        if missing_steps:
            raise KeyError(
                f"Run {run_id!r} has no step row(s) for: " + ", ".join(missing_steps)
            )
        ts = self.clock()
        self.store.con.execute(
            """
            UPDATE etl_job_runs
            SET status = 'running',
                finished_at = NULL,
                error_message = NULL
            WHERE run_id = ? AND run_kind = 'orchestrator'
            """,
            [run_id],
        )
        record_audit(
            self.store,
            run_id=run_id,
            dataset_id=None,
            actor=self.actor,
            action="run_resume",
            details={
                "pending_or_failed": [
                    dataset_id
                    for dataset_id in self.dag.order
                    if steps.get(dataset_id) in {"pending", "failed", "running"}
                ],
                "full_rebuild": full_rebuild,
                "gate": gate,
            },
            ts=ts,
        )
        return self._execute(
            run_id,
            params=dict(params or {}),
            full_rebuild=full_rebuild,
            resume=True,
            gate=gate,
        )

    def run_backfill(
        self,
        dataset_id: str,
        start: dt.date | dt.datetime | str,
        end: dt.date | dt.datetime | str,
        chunk: int | dt.timedelta | str,
        *,
        run_id: str | None = None,
        params: Mapping[str, Any] | None = None,
        git_sha: str | None = None,
        include_dependencies: bool = True,
        max_parallel: int = 1,
        executor: Callable[[Any, Any, Mapping[str, Any]], Any] | None = None,
    ) -> OrchestratorBackfillResult:
        return self._run_partition_driver(
            mode="backfill",
            dataset_id=dataset_id,
            start=start,
            end=end,
            chunk=chunk,
            run_id=run_id,
            params=params,
            git_sha=git_sha,
            include_dependencies=include_dependencies,
            max_parallel=max_parallel,
            executor=executor,
        )

    def run_maintenance(
        self,
        dataset_id: str,
        start: dt.date | dt.datetime | str,
        end: dt.date | dt.datetime | str,
        chunk: int | dt.timedelta | str,
        *,
        run_id: str | None = None,
        params: Mapping[str, Any] | None = None,
        git_sha: str | None = None,
        include_dependencies: bool = False,
        max_parallel: int = 1,
        executor: Callable[[Any, Any, Mapping[str, Any]], Any] | None = None,
    ) -> OrchestratorBackfillResult:
        return self._run_partition_driver(
            mode="maintenance",
            dataset_id=dataset_id,
            start=start,
            end=end,
            chunk=chunk,
            run_id=run_id,
            params=params,
            git_sha=git_sha,
            include_dependencies=include_dependencies,
            max_parallel=max_parallel,
            executor=executor,
        )

    def _run_partition_driver(
        self,
        *,
        mode: str,
        dataset_id: str,
        start: dt.date | dt.datetime | str,
        end: dt.date | dt.datetime | str,
        chunk: int | dt.timedelta | str,
        run_id: str | None,
        params: Mapping[str, Any] | None,
        git_sha: str | None,
        include_dependencies: bool,
        max_parallel: int,
        executor: Callable[[Any, Any, Mapping[str, Any]], Any] | None,
    ) -> OrchestratorBackfillResult:
        from . import backfill as backfill_driver

        self._initialize_store()
        dataset_order = self._partition_dataset_order(
            dataset_id,
            include_dependencies=include_dependencies,
        )
        requested_parallel, effective_parallel = _partition_parallelism_limits(
            max_parallel,
            allow_parallel_executor=executor is not None,
        )
        manifest_params = {
            **dict(params or {}),
            "mode": mode,
            "dataset_id": dataset_id,
            "start": start,
            "end": end,
            "chunk": chunk,
            "include_dependencies": include_dependencies,
            "max_parallel": effective_parallel,
            "requested_max_parallel": requested_parallel,
            "effective_max_parallel": effective_parallel,
            "dead_letter": True,
        }
        manifest = create_run_manifest(
            self.store,
            self._manifest_dag_for_order(dataset_order),
            run_id=run_id,
            params=manifest_params,
            git_sha=git_sha,
            actor=self.actor,
            started_at=self.clock(),
            run_kind=mode,
            job_name=f"warehouse_{mode}",
        )
        before_by_dataset = {
            current_dataset_id: self._watermark_snapshot(current_dataset_id)
            for current_dataset_id in dataset_order
        }
        self._mark_partition_steps_running(
            manifest.run_id,
            dataset_order,
            mode=mode,
            before_by_dataset=before_by_dataset,
        )

        retry_policy_by_dataset = {
            current_dataset_id: self._retry_policy(current_dataset_id)
            for current_dataset_id in dataset_order
        }
        driver = (
            backfill_driver.run_backfill
            if mode == "backfill"
            else backfill_driver.run_maintenance
        )
        try:
            backfill_result = driver(
                self.store,
                dataset_id,
                start,
                end,
                chunk,
                registry=self.registry,
                params=dict(params or {}),
                backfill_run_id=manifest.run_id,
                include_dependencies=include_dependencies,
                executor=executor,
                clock=self.clock,
                retry_policy=retry_policy_by_dataset,
                sleeper=self.sleeper,
                max_parallel=requested_parallel,
                dead_letter=True,
            )
        except Exception as exc:
            self._mark_partition_steps_failed(
                manifest.run_id,
                dataset_order,
                mode=mode,
                error_message=str(exc),
            )
            self._finish_run_kind(
                manifest.run_id,
                mode,
                "failed",
                error_message=str(exc),
            )
            record_audit(
                self.store,
                run_id=manifest.run_id,
                dataset_id=None,
                actor=self.actor,
                action=f"{mode}_run_fail",
                details={"error": str(exc), "dataset_id": dataset_id},
                ts=self.clock(),
            )
            raise

        self._finish_partition_steps(
            manifest.run_id,
            dataset_order,
            mode=mode,
            before_by_dataset=before_by_dataset,
            partition_results=backfill_result.partition_results,
        )
        self._finish_run_kind(manifest.run_id, mode, backfill_result.status)
        record_audit(
            self.store,
            run_id=manifest.run_id,
            dataset_id=None,
            actor=self.actor,
            action=(
                f"{mode}_run_partial"
                if backfill_result.status == "partial"
                else f"{mode}_run_succeed"
            ),
            details={
                "dataset_id": dataset_id,
                "dataset_order": list(backfill_result.dataset_order),
                "partitions_planned": backfill_result.partitions_planned,
                "partitions_succeeded": backfill_result.partitions_succeeded,
                "partitions_skipped": backfill_result.partitions_skipped,
                "partitions_failed": backfill_result.partitions_failed,
                "rows_written": backfill_result.rows_written,
                "max_parallel": backfill_result.effective_max_parallel,
                "requested_max_parallel": backfill_result.requested_max_parallel,
                "effective_max_parallel": backfill_result.effective_max_parallel,
            },
            ts=self.clock(),
        )
        return OrchestratorBackfillResult(
            run_id=manifest.run_id,
            status=backfill_result.status,
            dataset_order=backfill_result.dataset_order,
            backfill_run_id=backfill_result.backfill_run_id,
            partitions_planned=backfill_result.partitions_planned,
            partitions_succeeded=backfill_result.partitions_succeeded,
            partitions_skipped=backfill_result.partitions_skipped,
            partitions_failed=backfill_result.partitions_failed,
            rows_written=backfill_result.rows_written,
            requested_max_parallel=backfill_result.requested_max_parallel,
            effective_max_parallel=backfill_result.effective_max_parallel,
        )

    def _initialize_store(self) -> None:
        initialize = getattr(self.store, "initialize", None)
        if callable(initialize):
            initialize()

    def _partition_dataset_order(
        self,
        dataset_id: str,
        *,
        include_dependencies: bool,
    ) -> tuple[str, ...]:
        if dataset_id not in self.registry:
            raise KeyError(f"Unknown dataset_id {dataset_id!r}")
        if not include_dependencies:
            return (dataset_id,)

        needed: set[str] = {dataset_id}

        def visit(current: str) -> None:
            for dependency in self.dag.dependencies_of(current):
                if dependency not in needed:
                    needed.add(dependency)
                    visit(dependency)

        visit(dataset_id)
        return tuple(candidate for candidate in self.dag.order if candidate in needed)

    def _manifest_dag_for_order(self, dataset_order: tuple[str, ...]) -> DatasetDAG:
        selected = set(dataset_order)
        dependencies = {
            dataset_id: tuple(
                dependency
                for dependency in self.dag.dependencies_of(dataset_id)
                if dependency in selected
            )
            for dataset_id in dataset_order
        }
        children: dict[str, list[str]] = {dataset_id: [] for dataset_id in dataset_order}
        for dataset_id, dataset_dependencies in dependencies.items():
            for dependency in dataset_dependencies:
                children[dependency].append(dataset_id)
        return DatasetDAG(
            dependencies=dependencies,
            children={
                dataset_id: tuple(sorted(dataset_children))
                for dataset_id, dataset_children in children.items()
            },
            order=dataset_order,
        )

    def _mark_partition_steps_running(
        self,
        run_id: str,
        dataset_order: tuple[str, ...],
        *,
        mode: str,
        before_by_dataset: Mapping[str, Mapping[str, Any]],
    ) -> None:
        ts = self.clock()
        for dataset_id in dataset_order:
            before_json = _json_dumps(
                {
                    "mode": mode,
                    "watermark_before": before_by_dataset[dataset_id],
                }
            )
            self.store.con.execute(
                """
                UPDATE etl_job_steps
                SET status = 'running',
                    started_at = coalesce(started_at, ?),
                    watermark_before = ?,
                    error = NULL
                WHERE run_id = ? AND dataset_id = ?
                """,
                [ts, before_json, run_id, dataset_id],
            )
            record_audit(
                self.store,
                run_id=run_id,
                dataset_id=dataset_id,
                actor=self.actor,
                action=f"{mode}_step_start",
                details={"watermark_before": before_by_dataset[dataset_id]},
                ts=ts,
            )

    def _partition_step_summaries(
        self,
        dataset_order: tuple[str, ...],
        partition_results: tuple[Any, ...],
    ) -> dict[str, dict[str, Any]]:
        summaries: dict[str, dict[str, Any]] = {
            dataset_id: {
                "partitions": 0,
                "succeeded": 0,
                "skipped": 0,
                "failed": 0,
                "dead_lettered": 0,
                "rows_written": 0,
                "partition_keys": [],
                "dead_letter_keys": [],
                "errors": [],
            }
            for dataset_id in dataset_order
        }
        for result in partition_results:
            partition = result.partition
            dataset_id = partition.dataset_id
            summary = summaries.setdefault(
                dataset_id,
                {
                    "partitions": 0,
                    "succeeded": 0,
                    "skipped": 0,
                    "failed": 0,
                    "dead_lettered": 0,
                    "rows_written": 0,
                    "partition_keys": [],
                    "dead_letter_keys": [],
                    "errors": [],
                },
            )
            status = str(result.status)
            summary["partitions"] += 1
            summary["partition_keys"].append(partition.partition_key)
            summary["rows_written"] += int(result.rows_written or 0)
            if status == "succeeded":
                summary["succeeded"] += 1
            elif status == "skipped":
                summary["skipped"] += 1
            elif status == "dead_lettered":
                summary["failed"] += 1
                summary["dead_lettered"] += 1
                summary["dead_letter_keys"].append(partition.partition_key)
                if result.error:
                    summary["errors"].append(str(result.error))
            else:
                summary["failed"] += 1
                if result.error:
                    summary["errors"].append(str(result.error))
        return summaries

    def _step_status_from_partition_summary(self, summary: Mapping[str, Any]) -> str:
        if int(summary.get("failed", 0)) > 0:
            return "failed"
        if int(summary.get("partitions", 0)) == 0:
            return "skipped"
        if int(summary.get("succeeded", 0)) == 0 and int(summary.get("skipped", 0)) > 0:
            return "skipped"
        return "succeeded"

    def _finish_partition_steps(
        self,
        run_id: str,
        dataset_order: tuple[str, ...],
        *,
        mode: str,
        before_by_dataset: Mapping[str, Mapping[str, Any]],
        partition_results: tuple[Any, ...],
    ) -> None:
        summaries = self._partition_step_summaries(dataset_order, partition_results)
        for dataset_id in dataset_order:
            summary = summaries[dataset_id]
            status = self._step_status_from_partition_summary(summary)
            finished_at = self.clock()
            after_json = _json_dumps(
                {
                    "mode": mode,
                    "partition_summary": summary,
                }
            )
            before_json = _json_dumps(
                {
                    "mode": mode,
                    "watermark_before": before_by_dataset[dataset_id],
                }
            )
            error = None
            if status == "failed":
                dead_lettered = int(summary.get("dead_lettered", 0))
                error = (
                    f"{dead_lettered} partition(s) dead-lettered"
                    if dead_lettered
                    else "partition execution failed"
                )
            self.store.con.execute(
                """
                UPDATE etl_job_steps
                SET status = ?,
                    rows = ?,
                    finished_at = ?,
                    watermark_before = ?,
                    watermark_after = ?,
                    error = ?
                WHERE run_id = ? AND dataset_id = ?
                """,
                [
                    status,
                    int(summary["rows_written"]),
                    finished_at,
                    before_json,
                    after_json,
                    error,
                    run_id,
                    dataset_id,
                ],
            )
            record_audit(
                self.store,
                run_id=run_id,
                dataset_id=dataset_id,
                actor=self.actor,
                action=(
                    f"{mode}_step_dead_letter"
                    if int(summary.get("dead_lettered", 0)) > 0
                    else f"{mode}_step_{status}"
                ),
                details=summary,
                ts=finished_at,
            )

    def _mark_partition_steps_failed(
        self,
        run_id: str,
        dataset_order: tuple[str, ...],
        *,
        mode: str,
        error_message: str,
    ) -> None:
        ts = self.clock()
        for dataset_id in dataset_order:
            self.store.con.execute(
                """
                UPDATE etl_job_steps
                SET status = 'failed',
                    finished_at = ?,
                    error = ?
                WHERE run_id = ? AND dataset_id = ?
                  AND status IN ('pending', 'running')
                """,
                [ts, error_message, run_id, dataset_id],
            )
            record_audit(
                self.store,
                run_id=run_id,
                dataset_id=dataset_id,
                actor=self.actor,
                action=f"{mode}_step_fail",
                details={"error": error_message},
                ts=ts,
            )

    def _finish_run_kind(
        self,
        run_id: str,
        run_kind: str,
        status: str,
        *,
        error_message: str | None = None,
    ) -> None:
        self.store.con.execute(
            """
            UPDATE etl_job_runs
            SET status = ?,
                finished_at = ?,
                error_message = ?
            WHERE run_id = ? AND run_kind = ?
            """,
            [status, self.clock(), error_message, run_id, run_kind],
        )

    def _execute(
        self,
        run_id: str,
        *,
        params: dict[str, Any],
        full_rebuild: bool,
        resume: bool,
        gate: bool,
    ) -> OrchestratorResult:
        forced_stale: set[str] = set()
        partial_due_to_gate = False
        try:
            for dataset_id in self.dag.order:
                status = self._step_status(run_id, dataset_id)
                if resume and status in {"succeeded", "skipped"}:
                    continue
                if status not in {"pending", "failed", "running"}:
                    continue

                before = self._watermark_snapshot(dataset_id)
                before_json = _json_dumps(before)
                previous_after_json = self._previous_successful_watermark_after(
                    run_id,
                    dataset_id,
                )
                forced = dataset_id in forced_stale
                should_skip = (
                    not full_rebuild
                    and not forced
                    and previous_after_json is not None
                    and previous_after_json == before_json
                )
                if should_skip:
                    self._skip_step(run_id, dataset_id, before_json, before)
                    continue

                gate_decision = self._run_step(
                    run_id,
                    dataset_id,
                    base_params=params,
                    watermark_before=before,
                    watermark_before_json=before_json,
                    full_rebuild=full_rebuild,
                    forced_by_upstream=forced,
                    gate=gate,
                )
                if gate_decision == "partial":
                    partial_due_to_gate = True
                forced_stale.update(self.dag.children_of(dataset_id))
        except QualityGateError as exc:
            self._finish_run(run_id, "failed", error_message=str(exc))
            record_audit(
                self.store,
                run_id=run_id,
                dataset_id=None,
                actor=self.actor,
                action="run_quality_gate_halt",
                details={"error": str(exc), "dataset_id": exc.dataset_id},
                ts=self.clock(),
            )
            raise
        except Exception as exc:
            self._finish_run(
                run_id,
                self._failure_status(run_id),
                error_message=str(exc),
            )
            record_audit(
                self.store,
                run_id=run_id,
                dataset_id=None,
                actor=self.actor,
                action="run_fail",
                details={"error": str(exc)},
                ts=self.clock(),
            )
            raise

        final_status = "partial" if partial_due_to_gate else "succeeded"
        self._finish_run(run_id, final_status)
        record_audit(
            self.store,
            run_id=run_id,
            dataset_id=None,
            actor=self.actor,
            action="run_partial" if partial_due_to_gate else "run_succeed",
            details={"dataset_count": len(self.dag.order), "gate": gate},
            ts=self.clock(),
        )
        return OrchestratorResult(
            run_id=run_id,
            status=final_status,
            dataset_order=self.dag.order,
        )

    def _failure_status(self, run_id: str) -> str:
        completed = self.store.con.execute(
            """
            SELECT count(*)
            FROM etl_job_steps
            WHERE run_id = ? AND status IN ('succeeded', 'skipped')
            """,
            [run_id],
        ).fetchone()[0]
        return "partial" if int(completed or 0) > 0 else "failed"

    def _require_orchestrator_run(self, run_id: str) -> None:
        row = self.store.con.execute(
            """
            SELECT coalesce(run_kind, 'dataset')
            FROM etl_job_runs
            WHERE run_id = ?
              AND parent_run_id IS NULL
            ORDER BY started_at DESC, job_run_id DESC
            LIMIT 1
            """,
            [run_id],
        ).fetchone()
        if row is None:
            raise KeyError(f"No orchestrator run found for run_id {run_id!r}")
        run_kind = str(row[0])
        if run_kind != "orchestrator":
            raise ValueError(
                "resume() only supports run_kind='orchestrator'; "
                f"run {run_id!r} has run_kind={run_kind!r}"
            )

    def _step_statuses(self, run_id: str) -> dict[str, str]:
        rows = self.store.con.execute(
            """
            SELECT dataset_id, status
            FROM etl_job_steps
            WHERE run_id = ?
            """,
            [run_id],
        ).fetchall()
        if not rows:
            raise KeyError(f"No orchestrator run steps found for run_id {run_id!r}")
        return {str(dataset_id): str(status) for dataset_id, status in rows}

    def _step_status(self, run_id: str, dataset_id: str) -> str:
        row = self.store.con.execute(
            """
            SELECT status
            FROM etl_job_steps
            WHERE run_id = ? AND dataset_id = ?
            """,
            [run_id, dataset_id],
        ).fetchone()
        if row is None:
            raise KeyError(f"No step row for run_id={run_id!r} dataset_id={dataset_id!r}")
        return str(row[0])

    def _watermarks_for(self, dataset_id: str) -> dict[str, str]:
        rows = self.store.con.execute(
            """
            SELECT watermark_name, watermark_value
            FROM dataset_watermarks
            WHERE dataset_id = ?
            ORDER BY watermark_name
            """,
            [dataset_id],
        ).fetchall()
        return {str(name): str(value) for name, value in rows}

    def _watermark_snapshot(self, dataset_id: str) -> dict[str, Any]:
        dependencies = self.dag.dependencies_of(dataset_id)
        self_watermarks = self._watermarks_for(dataset_id)
        upstream = {
            dependency: self._watermarks_for(dependency)
            for dependency in dependencies
        }
        since_sources = upstream if upstream else {dataset_id: self_watermarks}
        since = _incremental_since(since_sources)
        return {
            "dataset_id": dataset_id,
            "self": self_watermarks,
            "upstream": upstream,
            "incremental_since": since,
        }

    def _previous_successful_watermark_after(
        self,
        current_run_id: str,
        dataset_id: str,
    ) -> str | None:
        row = self.store.con.execute(
            """
            SELECT s.watermark_after
            FROM etl_job_steps s
            JOIN etl_job_runs r ON r.run_id = s.run_id
            WHERE s.dataset_id = ?
              AND s.run_id <> ?
              AND s.status IN ('succeeded', 'skipped')
              AND s.watermark_after IS NOT NULL
              AND coalesce(r.run_kind, 'dataset') = 'orchestrator'
            ORDER BY coalesce(s.finished_at, r.finished_at, r.started_at) DESC,
                     s.run_id DESC
            LIMIT 1
            """,
            [dataset_id, current_run_id],
        ).fetchone()
        return None if row is None else str(row[0])

    def _skip_step(
        self,
        run_id: str,
        dataset_id: str,
        watermark_json: str,
        watermark_snapshot: Mapping[str, Any],
    ) -> None:
        ts = self.clock()
        self.store.con.execute(
            """
            UPDATE etl_job_steps
            SET status = 'skipped',
                rows = 0,
                started_at = ?,
                finished_at = ?,
                watermark_before = ?,
                watermark_after = ?,
                error = NULL
            WHERE run_id = ? AND dataset_id = ?
            """,
            [ts, ts, watermark_json, watermark_json, run_id, dataset_id],
        )
        record_audit(
            self.store,
            run_id=run_id,
            dataset_id=dataset_id,
            actor=self.actor,
            action="step_skip_incremental",
            details={"watermark": dict(watermark_snapshot)},
            ts=ts,
        )

    def _run_step(
        self,
        run_id: str,
        dataset_id: str,
        *,
        base_params: dict[str, Any],
        watermark_before: Mapping[str, Any],
        watermark_before_json: str,
        full_rebuild: bool,
        forced_by_upstream: bool,
        gate: bool,
    ) -> GateDecision:
        retry_policy = self._retry_policy(dataset_id)
        max_attempts = retry_policy.max_retries + 1
        params = self._params_for_step(
            self._base_params_for_dataset(base_params, dataset_id),
            watermark_before=watermark_before,
            full_rebuild=full_rebuild,
            forced_by_upstream=forced_by_upstream,
        )
        dataset_cls = _dataset_class(self.registry[dataset_id])
        option_factory = _option_factory(self.registry[dataset_id])
        dataset = dataset_cls()
        options = option_factory(params)
        error_message: str | None = None

        for attempt in range(1, max_attempts + 1):
            started_at = self.clock()
            self.store.con.execute(
                """
                UPDATE etl_job_steps
                SET status = 'running',
                    started_at = coalesce(started_at, ?),
                    watermark_before = ?,
                    error = NULL
                WHERE run_id = ? AND dataset_id = ?
                """,
                [started_at, watermark_before_json, run_id, dataset_id],
            )
            record_audit(
                self.store,
                run_id=run_id,
                dataset_id=dataset_id,
                actor=self.actor,
                action="step_start",
                details={"attempt": attempt, "max_attempts": max_attempts},
                ts=started_at,
            )
            try:
                result = dataset.run(self.store, options)
            except Exception as exc:
                error_message = f"{exc}\n{traceback.format_exc(limit=20)}"
                failed_at = self.clock()
                self.store.con.execute(
                    """
                    UPDATE etl_job_steps
                    SET status = 'failed',
                        finished_at = ?,
                        error = ?
                    WHERE run_id = ? AND dataset_id = ?
                    """,
                    [failed_at, error_message, run_id, dataset_id],
                )
                if attempt < max_attempts:
                    delay = retry_policy.retry_delay_seconds * (2 ** (attempt - 1))
                    record_audit(
                        self.store,
                        run_id=run_id,
                        dataset_id=dataset_id,
                        actor=self.actor,
                        action="step_retry",
                        details={
                            "attempt": attempt,
                            "next_attempt": attempt + 1,
                            "delay_seconds": delay,
                            "error": str(exc),
                        },
                        ts=failed_at,
                    )
                    if delay > 0:
                        self.sleeper(delay)
                    continue
                record_audit(
                    self.store,
                    run_id=run_id,
                    dataset_id=dataset_id,
                    actor=self.actor,
                    action="step_fail",
                    details={"attempt": attempt, "error": str(exc)},
                    ts=failed_at,
                )
                raise OrchestratorRunError(
                    f"Dataset {dataset_id!r} failed after {attempt} attempt(s)"
                ) from exc

            self._refresh_watermarks()
            after = self._watermark_snapshot(dataset_id)
            after_json = _json_dumps(after)
            gate_result: GateResult | None = None
            gate_decision: GateDecision = "pass"
            if gate:
                gate_result = evaluate_quality_gate(self.store, dataset_id)
                gate_decision = gate_result.decision
                if gate_decision == "halt":
                    failed_at = self.clock()
                    error_message = str(QualityGateError(dataset_id, gate_result))
                    self.store.con.execute(
                        """
                        UPDATE etl_job_steps
                        SET status = 'failed',
                            finished_at = ?,
                            watermark_before = ?,
                            watermark_after = ?,
                            error = ?
                        WHERE run_id = ? AND dataset_id = ?
                        """,
                        [
                            failed_at,
                            watermark_before_json,
                            after_json,
                            error_message,
                            run_id,
                            dataset_id,
                        ],
                    )
                    record_audit(
                        self.store,
                        run_id=run_id,
                        dataset_id=dataset_id,
                        actor=self.actor,
                        action="step_quality_gate_halt",
                        details=_quality_gate_details(gate_result),
                        ts=failed_at,
                    )
                    raise QualityGateError(dataset_id, gate_result)
                if gate_decision == "partial":
                    record_audit(
                        self.store,
                        run_id=run_id,
                        dataset_id=dataset_id,
                        actor=self.actor,
                        action="step_quality_gate_degrade",
                        details=_quality_gate_details(gate_result),
                        ts=self.clock(),
                    )
                elif gate_result.failed_count:
                    record_audit(
                        self.store,
                        run_id=run_id,
                        dataset_id=dataset_id,
                        actor=self.actor,
                        action="step_quality_gate_warn",
                        details=_quality_gate_details(gate_result),
                        ts=self.clock(),
                    )
            finished_at = self.clock()
            self.store.con.execute(
                """
                UPDATE etl_job_steps
                SET status = 'succeeded',
                    rows = ?,
                    finished_at = ?,
                    watermark_before = ?,
                    watermark_after = ?,
                    error = NULL
                WHERE run_id = ? AND dataset_id = ?
                """,
                [
                    int(getattr(result, "rows_loaded", 0) or 0),
                    finished_at,
                    watermark_before_json,
                    after_json,
                    run_id,
                    dataset_id,
                ],
            )
            record_audit(
                self.store,
                run_id=run_id,
                dataset_id=dataset_id,
                actor=self.actor,
                action="step_succeed",
                details={
                    "attempt": attempt,
                    "rows": int(getattr(result, "rows_loaded", 0) or 0),
                    "watermark_before": dict(watermark_before),
                    "watermark_after": after,
                    "quality_gate": _quality_gate_details(gate_result) if gate_result else None,
                },
                ts=finished_at,
            )
            return gate_decision

        raise OrchestratorRunError(error_message or f"Dataset {dataset_id!r} failed")

    def _base_params_for_dataset(
        self,
        base_params: Mapping[str, Any],
        dataset_id: str,
    ) -> dict[str, Any]:
        params = {
            key: value
            for key, value in dict(base_params).items()
            if key != DATASET_PARAMS_KEY
        }
        dataset_params_by_id = base_params.get(DATASET_PARAMS_KEY, {})
        if isinstance(dataset_params_by_id, Mapping):
            dataset_params = dataset_params_by_id.get(dataset_id, {})
            if isinstance(dataset_params, Mapping):
                params.update(dict(dataset_params))
        return params

    def _params_for_step(
        self,
        base_params: dict[str, Any],
        *,
        watermark_before: Mapping[str, Any],
        full_rebuild: bool,
        forced_by_upstream: bool,
    ) -> dict[str, Any]:
        params = dict(base_params)
        since = watermark_before.get("incremental_since")
        if full_rebuild:
            for key in INCREMENTAL_WINDOW_PARAM_KEYS:
                params.pop(key, None)
        params["full_rebuild"] = full_rebuild
        params["forced_by_upstream"] = forced_by_upstream
        params["watermark_before"] = watermark_before
        if not full_rebuild and since is not None:
            since_text = str(since)
            params["incremental_since"] = since_text
            params["since"] = since_text
            params["as_of_ts"] = since_text
            params["start_date"] = _date_prefix(since_text)
        return params

    def _retry_policy(self, dataset_id: str) -> RetryPolicy:
        configured = self.retry_policy_by_dataset.get(dataset_id)
        if configured is not None:
            if isinstance(configured, RetryPolicy):
                return configured
            return RetryPolicy(
                max_retries=_nonnegative_int(
                    configured.get("max_retries", 0),
                    "max_retries",
                ),
                retry_delay_seconds=_nonnegative_float(
                    configured.get("retry_delay_seconds", 0.0),
                    "retry_delay_seconds",
                ),
            )

        try:
            row = self.store.con.execute(
                """
                SELECT coalesce(max_retries, 0), coalesce(retry_delay_seconds, 0)
                FROM etl_job_definitions
                WHERE dataset_id = ? AND enabled
                ORDER BY created_at, job_name
                LIMIT 1
                """,
                [dataset_id],
            ).fetchone()
        except Exception:
            row = None
        if row is None:
            return self.default_retry_policy
        return RetryPolicy(
            max_retries=_nonnegative_int(row[0], "max_retries"),
            retry_delay_seconds=_nonnegative_float(row[1], "retry_delay_seconds"),
        )

    def _refresh_watermarks(self) -> None:
        if self.watermark_refresher is not None:
            self.watermark_refresher(self.store)

    def _finish_run(
        self,
        run_id: str,
        status: str,
        *,
        error_message: str | None = None,
    ) -> None:
        self.store.con.execute(
            """
            UPDATE etl_job_runs
            SET status = ?,
                finished_at = ?,
                error_message = ?
            WHERE run_id = ? AND run_kind = 'orchestrator'
            """,
            [status, self.clock(), error_message, run_id],
        )
