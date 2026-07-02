from __future__ import annotations

import datetime as dt
import heapq
import json
import time
import traceback
import uuid
from dataclasses import dataclass
from typing import Any, Callable, Mapping


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
        )

    def resume(
        self,
        run_id: str,
        *,
        params: Mapping[str, Any] | None = None,
        full_rebuild: bool = False,
    ) -> OrchestratorResult:
        self._initialize_store()
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
            },
            ts=ts,
        )
        return self._execute(
            run_id,
            params=dict(params or {}),
            full_rebuild=full_rebuild,
            resume=True,
        )

    def _initialize_store(self) -> None:
        initialize = getattr(self.store, "initialize", None)
        if callable(initialize):
            initialize()

    def _execute(
        self,
        run_id: str,
        *,
        params: dict[str, Any],
        full_rebuild: bool,
        resume: bool,
    ) -> OrchestratorResult:
        forced_stale: set[str] = set()
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

                self._run_step(
                    run_id,
                    dataset_id,
                    base_params=params,
                    watermark_before=before,
                    watermark_before_json=before_json,
                    full_rebuild=full_rebuild,
                    forced_by_upstream=forced,
                )
                forced_stale.update(self.dag.children_of(dataset_id))
        except Exception as exc:
            self._finish_run(run_id, "failed", error_message=str(exc))
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

        self._finish_run(run_id, "succeeded")
        record_audit(
            self.store,
            run_id=run_id,
            dataset_id=None,
            actor=self.actor,
            action="run_succeed",
            details={"dataset_count": len(self.dag.order)},
            ts=self.clock(),
        )
        return OrchestratorResult(
            run_id=run_id,
            status="succeeded",
            dataset_order=self.dag.order,
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
    ) -> None:
        retry_policy = self._retry_policy(dataset_id)
        max_attempts = retry_policy.max_retries + 1
        params = self._params_for_step(
            base_params,
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
                },
                ts=finished_at,
            )
            return

        raise OrchestratorRunError(error_message or f"Dataset {dataset_id!r} failed")

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
