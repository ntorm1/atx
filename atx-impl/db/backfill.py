from __future__ import annotations

import datetime as dt
import concurrent.futures
import re
import traceback
import time
import uuid
from dataclasses import dataclass
from typing import Any, Callable, Mapping, Sequence

from .dataset import DatasetLoadResult
from .orchestrator import RegistryEntry, RetryPolicy, _incremental_since, build_dataset_dag


Clock = Callable[[], dt.datetime]
PartitionExecutor = Callable[[Any, "Partition", Mapping[str, Any]], Any]
Sleeper = Callable[[float], None]
RetryPolicyConfig = RetryPolicy | Mapping[str, Any] | Mapping[str, RetryPolicy | Mapping[str, Any]] | None


@dataclass(frozen=True, order=True)
class Partition:
    dataset_id: str
    window_lo: dt.date
    window_hi: dt.date
    partition_key: str


@dataclass(frozen=True)
class PartitionResult:
    partition: Partition
    status: str
    rows_written: int = 0
    watermark_after: str | None = None
    attempts: int = 0
    error: str | None = None


@dataclass(frozen=True)
class BackfillRunResult:
    backfill_run_id: str
    dataset_id: str
    status: str
    dataset_order: tuple[str, ...]
    partitions_planned: int
    partitions_succeeded: int
    partitions_skipped: int
    partitions_failed: int
    rows_written: int
    partition_results: tuple[PartitionResult, ...]


@dataclass(frozen=True)
class _ChunkSpec:
    amount: int
    unit: str
    label: str


_CHUNK_RE = re.compile(r"^\s*(\d+)\s*([A-Za-z]*)\s*$")


def _now_utc_naive() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)


def _positive_int(value: Any, name: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise ValueError(f"{name} must be positive")
    return parsed


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


def _coerce_retry_policy(value: RetryPolicy | Mapping[str, Any] | None) -> RetryPolicy | None:
    if value is None:
        return None
    if isinstance(value, RetryPolicy):
        return value
    if not isinstance(value, Mapping):
        raise TypeError("retry_policy must be a RetryPolicy or mapping")
    return RetryPolicy(
        max_retries=_nonnegative_int(value.get("max_retries", 0), "max_retries"),
        retry_delay_seconds=_nonnegative_float(
            value.get("retry_delay_seconds", 0.0),
            "retry_delay_seconds",
        ),
    )


def _retry_policy_for_dataset(
    retry_policy: RetryPolicyConfig,
    dataset_id: str,
) -> RetryPolicy | None:
    if retry_policy is None or isinstance(retry_policy, RetryPolicy):
        return retry_policy
    if not isinstance(retry_policy, Mapping):
        raise TypeError("retry_policy must be a RetryPolicy or mapping")
    if "max_retries" in retry_policy or "retry_delay_seconds" in retry_policy:
        return _coerce_retry_policy(retry_policy)
    return _coerce_retry_policy(retry_policy.get(dataset_id))


def _default_registry() -> Mapping[str, RegistryEntry]:
    from .jobs import DATASET_REGISTRY

    return DATASET_REGISTRY


def _dataset_class(entry: RegistryEntry) -> type[Any]:
    if isinstance(entry, tuple):
        if not entry:
            raise ValueError("DATASET_REGISTRY entry tuple is empty")
        return entry[0]
    return entry


def _option_factory(entry: RegistryEntry) -> Callable[[dict[str, Any]], Any]:
    if isinstance(entry, tuple):
        if len(entry) < 2 or entry[1] is None:
            return lambda params: params
        return entry[1]
    return lambda params: params


def _coerce_date(value: dt.date | dt.datetime | str, name: str) -> dt.date:
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    if isinstance(value, str):
        text = value.strip()
        if not text:
            raise ValueError(f"{name} must not be empty")
        return dt.date.fromisoformat(text.replace("T", " ").split(" ", 1)[0])
    raise TypeError(f"{name} must be a date, datetime, or ISO date string")


def _parse_chunk(chunk: int | dt.timedelta | str) -> _ChunkSpec:
    if isinstance(chunk, bool):
        raise TypeError("chunk must be a positive day count, timedelta, or duration string")
    if isinstance(chunk, int):
        if chunk <= 0:
            raise ValueError("chunk must be positive")
        return _ChunkSpec(chunk, "day", f"{chunk}d")
    if isinstance(chunk, dt.timedelta):
        if chunk <= dt.timedelta(0):
            raise ValueError("chunk must be positive")
        if chunk.seconds or chunk.microseconds:
            raise ValueError("date backfill chunks must be whole days")
        return _ChunkSpec(chunk.days, "day", f"{chunk.days}d")
    if not isinstance(chunk, str):
        raise TypeError("chunk must be a positive day count, timedelta, or duration string")

    match = _CHUNK_RE.match(chunk)
    if match is None:
        raise ValueError(f"Unsupported chunk value {chunk!r}")
    amount = int(match.group(1))
    if amount <= 0:
        raise ValueError("chunk must be positive")
    raw_unit = (match.group(2) or "d").lower()
    unit_map = {
        "d": "day",
        "day": "day",
        "days": "day",
        "w": "week",
        "week": "week",
        "weeks": "week",
        "m": "month",
        "mo": "month",
        "mon": "month",
        "month": "month",
        "months": "month",
        "q": "quarter",
        "qr": "quarter",
        "qtr": "quarter",
        "quarter": "quarter",
        "quarters": "quarter",
        "y": "year",
        "yr": "year",
        "year": "year",
        "years": "year",
    }
    unit = unit_map.get(raw_unit)
    if unit is None:
        raise ValueError(f"Unsupported chunk unit {raw_unit!r}")
    suffix = {"day": "d", "week": "w", "month": "mo", "quarter": "q", "year": "y"}[unit]
    return _ChunkSpec(amount, unit, f"{amount}{suffix}")


def _last_day_of_month(year: int, month: int) -> int:
    if month == 12:
        next_month = dt.date(year + 1, 1, 1)
    else:
        next_month = dt.date(year, month + 1, 1)
    return (next_month - dt.timedelta(days=1)).day


def _add_months(value: dt.date, months: int) -> dt.date:
    month_index = (value.year * 12 + value.month - 1) + months
    year = month_index // 12
    month = month_index % 12 + 1
    day = min(value.day, _last_day_of_month(year, month))
    return dt.date(year, month, day)


def _advance(value: dt.date, chunk: _ChunkSpec) -> dt.date:
    if chunk.unit == "day":
        return value + dt.timedelta(days=chunk.amount)
    if chunk.unit == "week":
        return value + dt.timedelta(weeks=chunk.amount)
    if chunk.unit == "month":
        return _add_months(value, chunk.amount)
    if chunk.unit == "quarter":
        return _add_months(value, chunk.amount * 3)
    if chunk.unit == "year":
        return _add_months(value, chunk.amount * 12)
    raise ValueError(f"Unsupported chunk unit {chunk.unit!r}")


def _partition_key(dataset_id: str, window_lo: dt.date, window_hi: dt.date) -> str:
    return f"{dataset_id}:{window_lo.isoformat()}:{window_hi.isoformat()}"


def plan_backfill(
    dataset_id: str,
    start: dt.date | dt.datetime | str,
    end: dt.date | dt.datetime | str,
    chunk: int | dt.timedelta | str,
    *,
    registry: Mapping[str, RegistryEntry] | None = None,
) -> list[Partition]:
    """Return deterministic half-open partitions for ``dataset_id`` over ``[start, end)``."""

    selected_registry = _default_registry() if registry is None else registry
    if dataset_id not in selected_registry:
        raise KeyError(f"Unknown dataset_id {dataset_id!r}")
    window_start = _coerce_date(start, "start")
    window_end = _coerce_date(end, "end")
    if window_start >= window_end:
        raise ValueError("start must be before end")
    chunk_spec = _parse_chunk(chunk)

    partitions: list[Partition] = []
    current = window_start
    while current < window_end:
        next_hi = min(_advance(current, chunk_spec), window_end)
        if next_hi <= current:
            raise ValueError(f"chunk {chunk!r} did not advance the window")
        partitions.append(
            Partition(
                dataset_id=dataset_id,
                window_lo=current,
                window_hi=next_hi,
                partition_key=_partition_key(dataset_id, current, next_hi),
            )
        )
        current = next_hi
    return partitions


def _dependency_order(
    dataset_id: str,
    registry: Mapping[str, RegistryEntry],
    *,
    include_dependencies: bool,
) -> tuple[str, ...]:
    if dataset_id not in registry:
        raise KeyError(f"Unknown dataset_id {dataset_id!r}")
    if not include_dependencies:
        return (dataset_id,)

    dag = build_dataset_dag(registry)
    needed: set[str] = {dataset_id}

    def visit(current: str) -> None:
        for dependency in dag.dependencies_of(current):
            if dependency not in needed:
                needed.add(dependency)
                visit(dependency)

    visit(dataset_id)
    return tuple(candidate for candidate in dag.order if candidate in needed)


def _insert_run_header(
    store: Any,
    *,
    backfill_run_id: str,
    dataset_id: str,
    start: dt.date,
    end: dt.date,
    chunk_label: str,
    ts: dt.datetime,
) -> str:
    existing = store.con.execute(
        """
        SELECT dataset_id, start_date, end_date, chunk, status
        FROM backfill_run
        WHERE backfill_run_id = ?
        """,
        [backfill_run_id],
    ).fetchone()
    if existing is not None:
        existing_dataset_id = str(existing[0])
        existing_start = _coerce_date(existing[1], "backfill_run.start_date")
        existing_end = _coerce_date(existing[2], "backfill_run.end_date")
        existing_chunk = str(existing[3])
        existing_status = str(existing[4])
        if (
            existing_dataset_id != dataset_id
            or existing_start != start
            or existing_end != end
            or existing_chunk != chunk_label
        ):
            raise ValueError(
                "backfill_run_id already exists for a different backfill window: "
                f"{backfill_run_id!r}"
            )
        if existing_status in {"running", "failed"}:
            store.con.execute(
                """
                UPDATE backfill_run
                SET status = 'running',
                    finished_at = NULL,
                    error_message = NULL
                WHERE backfill_run_id = ?
                """,
                [backfill_run_id],
            )
        return existing_status

    store.con.execute(
        """
        INSERT INTO backfill_run (
            backfill_run_id, dataset_id, start_date, end_date, chunk,
            status, started_at, finished_at, error_message
        )
        VALUES (?, ?, ?, ?, ?, 'running', ?, NULL, NULL)
        """,
        [backfill_run_id, dataset_id, start, end, chunk_label, ts],
    )
    return "inserted"


def _finish_run(
    store: Any,
    *,
    backfill_run_id: str,
    status: str,
    finished_at: dt.datetime,
    error_message: str | None = None,
) -> None:
    store.con.execute(
        """
        UPDATE backfill_run
        SET status = ?,
            finished_at = ?,
            error_message = ?
        WHERE backfill_run_id = ?
        """,
        [status, finished_at, error_message, backfill_run_id],
    )


def _watermark_row(store: Any, partition: Partition) -> tuple[Any, ...] | None:
    return store.con.execute(
        """
        SELECT status, window_lo, window_hi, rows_written, watermark_after, run_id, updated_at
        FROM backfill_watermark
        WHERE dataset_id = ? AND partition_key = ?
        """,
        [partition.dataset_id, partition.partition_key],
    ).fetchone()


def _dataset_watermarks(store: Any, dataset_id: str) -> dict[str, str]:
    rows = store.con.execute(
        """
        SELECT watermark_name, watermark_value
        FROM dataset_watermarks
        WHERE dataset_id = ?
        ORDER BY watermark_name
        """,
        [dataset_id],
    ).fetchall()
    return {str(name): str(value) for name, value in rows}


def _watermark_sources_for_dataset(
    store: Any,
    dataset_id: str,
    registry: Mapping[str, RegistryEntry],
) -> dict[str, dict[str, str]]:
    dag = build_dataset_dag(registry)
    dependencies = dag.dependencies_of(dataset_id)
    self_watermarks = _dataset_watermarks(store, dataset_id)
    upstream = {
        dependency: _dataset_watermarks(store, dependency)
        for dependency in dependencies
    }
    return upstream if upstream else {dataset_id: self_watermarks}


def _watermark_name_matches_partition(name: str, partition: Partition) -> bool:
    return partition.partition_key in name or (
        partition.window_lo.isoformat() in name
        and partition.window_hi.isoformat() in name
    )


def _parse_watermark_datetime(value: Any) -> dt.datetime | None:
    if isinstance(value, dt.datetime):
        parsed = value
    elif isinstance(value, dt.date):
        parsed = dt.datetime.combine(value, dt.time.min)
    else:
        text = str(value).strip()
        if not text:
            return None
        if text.endswith("Z"):
            text = text[:-1] + "+00:00"
        try:
            parsed = dt.datetime.fromisoformat(text.replace("T", " "))
        except ValueError:
            try:
                parsed = dt.datetime.combine(dt.date.fromisoformat(text), dt.time.min)
            except ValueError:
                return None
    if parsed.tzinfo is not None:
        parsed = parsed.astimezone(dt.timezone.utc).replace(tzinfo=None)
    return parsed


def _compare_watermark_values(left: Any, right: Any) -> int:
    left_parsed = _parse_watermark_datetime(left)
    right_parsed = _parse_watermark_datetime(right)
    if left_parsed is not None and right_parsed is not None:
        return (left_parsed > right_parsed) - (left_parsed < right_parsed)
    left_text = str(left)
    right_text = str(right)
    return (left_text > right_text) - (left_text < right_text)


def _watermark_less_than(left: Any, right: Any) -> bool:
    return _compare_watermark_values(left, right) < 0


def _sources_have_current_watermark(sources: Mapping[str, Mapping[str, str]]) -> bool:
    return _incremental_since(sources) is not None


def _sources_have_partition_scope(
    sources: Mapping[str, Mapping[str, str]],
    partitions: Sequence[Partition],
) -> bool:
    return any(
        _watermark_name_matches_partition(name, partition)
        for marks in sources.values()
        for name in marks
        for partition in partitions
    )


def _coarse_partition_watermark(current: str | None, partition: Partition) -> str | None:
    if current is None:
        return None
    if _watermark_less_than(current, partition.window_lo.isoformat()):
        return None
    if _watermark_less_than(partition.window_hi.isoformat(), current):
        return partition.window_hi.isoformat()
    return current


def _current_partition_watermark(
    sources: Mapping[str, Mapping[str, str]],
    partition: Partition,
    *,
    has_partition_scope: bool,
) -> str | None:
    partition_sources: dict[str, dict[str, str]] = {}
    for source_dataset_id, marks in sources.items():
        matching = {
            name: value
            for name, value in marks.items()
            if _watermark_name_matches_partition(name, partition)
        }
        if matching:
            partition_sources[source_dataset_id] = matching

    current = _incremental_since(partition_sources) if partition_sources else None
    if current is not None:
        return current
    if has_partition_scope:
        return None
    return _coarse_partition_watermark(_incremental_since(sources), partition)


def _should_run_maintenance_partition(
    existing: Sequence[Any] | None,
    *,
    current_watermark: str | None,
    has_current_watermarks: bool = False,
) -> bool:
    if existing is None:
        return current_watermark is not None or not has_current_watermarks
    if str(existing[0]) != "succeeded":
        return True
    if current_watermark is None:
        return False
    recorded = existing[4]
    if recorded is None:
        return True
    return _watermark_less_than(recorded, current_watermark)


def _mark_partition_running(
    store: Any,
    partition: Partition,
    *,
    backfill_run_id: str,
    ts: dt.datetime,
) -> None:
    store.con.execute(
        """
        INSERT OR REPLACE INTO backfill_watermark (
            dataset_id, partition_key, window_lo, window_hi, status,
            rows_written, watermark_after, run_id, updated_at
        )
        VALUES (?, ?, ?, ?, 'running', 0, NULL, ?, ?)
        """,
        [
            partition.dataset_id,
            partition.partition_key,
            partition.window_lo,
            partition.window_hi,
            backfill_run_id,
            ts,
        ],
    )


def _mark_partition_succeeded(
    store: Any,
    partition: Partition,
    *,
    rows_written: int,
    watermark_after: str,
    backfill_run_id: str,
    ts: dt.datetime,
) -> None:
    store.con.execute(
        """
        INSERT OR REPLACE INTO backfill_watermark (
            dataset_id, partition_key, window_lo, window_hi, status,
            rows_written, watermark_after, run_id, updated_at
        )
        VALUES (?, ?, ?, ?, 'succeeded', ?, ?, ?, ?)
        """,
        [
            partition.dataset_id,
            partition.partition_key,
            partition.window_lo,
            partition.window_hi,
            rows_written,
            watermark_after,
            backfill_run_id,
            ts,
        ],
    )


def _mark_partition_failed(
    store: Any,
    partition: Partition,
    *,
    backfill_run_id: str,
    ts: dt.datetime,
) -> None:
    store.con.execute(
        """
        INSERT OR REPLACE INTO backfill_watermark (
            dataset_id, partition_key, window_lo, window_hi, status,
            rows_written, watermark_after, run_id, updated_at
        )
        VALUES (?, ?, ?, ?, 'failed', 0, NULL, ?, ?)
        """,
        [
            partition.dataset_id,
            partition.partition_key,
            partition.window_lo,
            partition.window_hi,
            backfill_run_id,
            ts,
        ],
    )


def _record_dead_letter(
    store: Any,
    partition: Partition,
    *,
    backfill_run_id: str,
    error: str,
    attempts: int,
    ts: dt.datetime,
) -> None:
    store.con.execute(
        """
        INSERT OR REPLACE INTO backfill_dead_letter (
            dataset_id, partition_key, run_id, error, attempts, dead_lettered_at
        )
        VALUES (?, ?, ?, ?, ?, ?)
        """,
        [
            partition.dataset_id,
            partition.partition_key,
            backfill_run_id,
            error,
            attempts,
            ts,
        ],
    )


def _clear_partition_dead_letter(
    store: Any,
    partition: Partition,
) -> None:
    store.con.execute(
        """
        DELETE FROM backfill_dead_letter
        WHERE dataset_id = ? AND partition_key = ?
        """,
        [partition.dataset_id, partition.partition_key],
    )


def _partition_params(
    base_params: Mapping[str, Any],
    partition: Partition,
    *,
    backfill_run_id: str,
) -> dict[str, Any]:
    end_inclusive = partition.window_hi - dt.timedelta(days=1)
    params = dict(base_params)
    params.update(
        {
            "start_date": partition.window_lo,
            "end_date": end_inclusive,
            "window_lo": partition.window_lo,
            "window_hi": partition.window_hi,
            "window_hi_exclusive": partition.window_hi,
            "partition_key": partition.partition_key,
            "backfill_partition_key": partition.partition_key,
            "backfill_run_id": backfill_run_id,
            "run_id": backfill_run_id,
            "full_rebuild": True,
        }
    )
    return params


def _maintenance_partition_params(
    base_params: Mapping[str, Any],
    partition: Partition,
    *,
    backfill_run_id: str,
    current_watermark: str | None,
) -> dict[str, Any]:
    params = _partition_params(
        base_params,
        partition,
        backfill_run_id=backfill_run_id,
    )
    params["full_rebuild"] = False
    params["maintenance"] = True
    if current_watermark is not None:
        params["maintenance_watermark"] = current_watermark
        params["current_watermark"] = current_watermark
    return params


def _default_executor(
    store: Any,
    partition: Partition,
    params: Mapping[str, Any],
    *,
    registry: Mapping[str, RegistryEntry],
) -> Any:
    entry = registry[partition.dataset_id]
    dataset_cls = _dataset_class(entry)
    option_factory = _option_factory(entry)
    dataset = dataset_cls()
    options = option_factory(dict(params))
    return dataset.run(store, options)


def _rows_written(result: Any) -> int:
    if isinstance(result, DatasetLoadResult):
        return int(result.rows_loaded or 0)
    if isinstance(result, int):
        return int(result)
    if isinstance(result, Mapping):
        for key in ("rows_written", "rows_loaded", "rows"):
            if key in result and result[key] is not None:
                return int(result[key])
        return 0
    for attr in ("rows_written", "rows_loaded", "rows"):
        value = getattr(result, attr, None)
        if value is not None:
            return int(value)
    return 0


def _watermark_after(result: Any, partition: Partition) -> str:
    if isinstance(result, DatasetLoadResult):
        value = result.details.get("watermark_after")
        if value is not None:
            return str(value)
    if isinstance(result, Mapping):
        for key in ("watermark_after", "max_available_at", "watermark"):
            value = result.get(key)
            if value is not None:
                return str(value)
    value = getattr(result, "watermark_after", None)
    if value is not None:
        return str(value)
    return partition.window_hi.isoformat()


def _maintenance_watermark_after(
    result: Any,
    partition: Partition,
    *,
    current_watermark: str | None,
) -> str:
    if current_watermark is None:
        return _watermark_after(result, partition)
    return current_watermark


def _validate_watermark_window(partition: Partition, row: Sequence[Any]) -> None:
    stored_lo = _coerce_date(row[1], "backfill_watermark.window_lo")
    stored_hi = _coerce_date(row[2], "backfill_watermark.window_hi")
    if stored_lo != partition.window_lo or stored_hi != partition.window_hi:
        raise ValueError(
            "backfill_watermark partition window mismatch for "
            f"{partition.dataset_id!r} {partition.partition_key!r}"
        )


def _initialize_store(store: Any) -> None:
    initialize = getattr(store, "initialize", None)
    if callable(initialize):
        initialize()


@dataclass(frozen=True)
class _ExecutionOutcome:
    result: Any = None
    attempts: int = 0
    error: str | None = None
    exception: BaseException | None = None


def _execute_with_retry(
    store: Any,
    partition: Partition,
    partition_params: Mapping[str, Any],
    run_executor: PartitionExecutor,
    *,
    retry_policy: RetryPolicy | None,
    sleeper: Sleeper,
) -> _ExecutionOutcome:
    max_attempts = (retry_policy.max_retries + 1) if retry_policy is not None else 1
    delay_base = retry_policy.retry_delay_seconds if retry_policy is not None else 0.0

    for attempt in range(1, max_attempts + 1):
        try:
            return _ExecutionOutcome(
                result=run_executor(store, partition, partition_params),
                attempts=attempt,
            )
        except Exception as exc:
            error = f"{exc}\n{traceback.format_exc(limit=20)}"
            if attempt < max_attempts:
                delay = delay_base * (2 ** (attempt - 1))
                if delay > 0:
                    sleeper(delay)
                continue
            return _ExecutionOutcome(
                attempts=attempt,
                error=error,
                exception=exc,
            )

    return _ExecutionOutcome(
        attempts=max_attempts,
        error=f"Partition {partition.partition_key!r} failed without an exception",
    )


def _partition_result_from_outcome(
    partition: Partition,
    outcome: _ExecutionOutcome,
    *,
    watermark_after: Callable[[Any, Partition], str],
) -> PartitionResult:
    if outcome.exception is not None:
        return PartitionResult(
            partition=partition,
            status="failed",
            attempts=outcome.attempts,
            error=outcome.error,
        )
    rows = _rows_written(outcome.result)
    watermark = watermark_after(outcome.result, partition)
    return PartitionResult(
        partition=partition,
        status="succeeded",
        rows_written=rows,
        watermark_after=watermark,
        attempts=outcome.attempts,
    )


def _persist_partition_result(
    store: Any,
    result: PartitionResult,
    *,
    backfill_run_id: str,
    dead_letter: bool,
    now: Clock,
) -> PartitionResult:
    partition = result.partition
    if result.status == "succeeded":
        _mark_partition_succeeded(
            store,
            partition,
            rows_written=result.rows_written,
            watermark_after=result.watermark_after or partition.window_hi.isoformat(),
            backfill_run_id=backfill_run_id,
            ts=now(),
        )
        _clear_partition_dead_letter(store, partition)
        return result

    _mark_partition_failed(
        store,
        partition,
        backfill_run_id=backfill_run_id,
        ts=now(),
    )
    if dead_letter:
        _record_dead_letter(
            store,
            partition,
            backfill_run_id=backfill_run_id,
            error=result.error or "partition failed",
            attempts=result.attempts,
            ts=now(),
        )
        return PartitionResult(
            partition=partition,
            status="dead_lettered",
            rows_written=0,
            watermark_after=None,
            attempts=result.attempts,
            error=result.error,
        )
    return result


def _execute_partition_jobs(
    store: Any,
    indexed_partitions: Sequence[tuple[int, Partition]],
    *,
    backfill_run_id: str,
    params_for_partition: Callable[[Partition], Mapping[str, Any]],
    watermark_after: Callable[[Any, Partition], str],
    run_executor: PartitionExecutor,
    retry_policy: RetryPolicyConfig,
    max_parallel: int,
    dead_letter: bool,
    sleeper: Sleeper,
    now: Clock,
    allow_parallel_executor: bool,
) -> dict[int, PartitionResult]:
    if not indexed_partitions:
        return {}

    requested_parallel = _positive_int(max_parallel, "max_parallel")
    # The default registry executor receives the caller's DuckDB connection, so it
    # stays serialized. Tests or callers that inject a partition executor can opt
    # into true fan-out when that executor owns its write-safety.
    effective_parallel = requested_parallel if allow_parallel_executor else 1
    results: dict[int, PartitionResult] = {}

    def run_one(index: int, partition: Partition) -> tuple[int, PartitionResult, BaseException | None]:
        policy = _retry_policy_for_dataset(retry_policy, partition.dataset_id)
        outcome = _execute_with_retry(
            store,
            partition,
            params_for_partition(partition),
            run_executor,
            retry_policy=policy,
            sleeper=sleeper,
        )
        result = _partition_result_from_outcome(
            partition,
            outcome,
            watermark_after=watermark_after,
        )
        return index, result, outcome.exception

    def persist(index: int, result: PartitionResult, exc: BaseException | None) -> None:
        persisted = _persist_partition_result(
            store,
            result,
            backfill_run_id=backfill_run_id,
            dead_letter=dead_letter,
            now=now,
        )
        results[index] = persisted
        if exc is not None and not dead_letter:
            raise exc

    if effective_parallel == 1:
        for index, partition in indexed_partitions:
            _mark_partition_running(
                store,
                partition,
                backfill_run_id=backfill_run_id,
                ts=now(),
            )
            completed_index, result, exc = run_one(index, partition)
            persist(completed_index, result, exc)
        return results

    pending: dict[concurrent.futures.Future[tuple[int, PartitionResult, BaseException | None]], None] = {}
    next_position = 0

    def submit_next(
        pool: concurrent.futures.ThreadPoolExecutor,
    ) -> None:
        nonlocal next_position
        if next_position >= len(indexed_partitions):
            return
        index, partition = indexed_partitions[next_position]
        next_position += 1
        _mark_partition_running(
            store,
            partition,
            backfill_run_id=backfill_run_id,
            ts=now(),
        )
        pending[pool.submit(run_one, index, partition)] = None

    with concurrent.futures.ThreadPoolExecutor(max_workers=effective_parallel) as pool:
        for _ in range(effective_parallel):
            submit_next(pool)
        while pending:
            done, _not_done = concurrent.futures.wait(
                pending,
                return_when=concurrent.futures.FIRST_COMPLETED,
            )
            for future in done:
                pending.pop(future, None)
                completed_index, result, exc = future.result()
                persist(completed_index, result, exc)
                submit_next(pool)

    return results


def run_backfill(
    store: Any,
    dataset_id: str,
    start: dt.date | dt.datetime | str,
    end: dt.date | dt.datetime | str,
    chunk: int | dt.timedelta | str,
    *,
    registry: Mapping[str, RegistryEntry] | None = None,
    params: Mapping[str, Any] | None = None,
    backfill_run_id: str | None = None,
    include_dependencies: bool = True,
    executor: PartitionExecutor | None = None,
    clock: Clock | None = None,
    retry_policy: RetryPolicyConfig = None,
    sleeper: Sleeper | None = None,
    max_parallel: int = 1,
    dead_letter: bool = False,
) -> BackfillRunResult:
    """Execute a windowed backfill and persist per-partition progress.

    Completed partitions are strict no-ops: their existing ``backfill_watermark`` rows
    are read but not updated, and the dataset executor is not called.

    The built-in registry executor uses the caller's DuckDB connection and is
    therefore serialized even if ``max_parallel`` is greater than one. Injected
    executors can fan out up to ``max_parallel`` when they manage their own write
    safety; results are still returned in deterministic partition order.
    """

    selected_registry = _default_registry() if registry is None else registry
    window_start = _coerce_date(start, "start")
    window_end = _coerce_date(end, "end")
    chunk_spec = _parse_chunk(chunk)
    run_id = backfill_run_id or str(uuid.uuid4())
    now = clock or _now_utc_naive

    _initialize_store(store)
    dataset_order = _dependency_order(
        dataset_id,
        selected_registry,
        include_dependencies=include_dependencies,
    )
    planned = tuple(
        partition
        for current_dataset_id in dataset_order
        for partition in plan_backfill(
            current_dataset_id,
            window_start,
            window_end,
            chunk_spec.label,
            registry=selected_registry,
        )
    )

    initial_run_status = _insert_run_header(
        store,
        backfill_run_id=run_id,
        dataset_id=dataset_id,
        start=window_start,
        end=window_end,
        chunk_label=chunk_spec.label,
        ts=now(),
    )

    results: list[PartitionResult] = []
    run_executor = executor
    if run_executor is None:
        run_executor = lambda s, p, partition_params: _default_executor(
            s,
            p,
            partition_params,
            registry=selected_registry,
        )
        allow_parallel_executor = False
    else:
        allow_parallel_executor = True

    try:
        result_slots: list[PartitionResult | None] = [None] * len(planned)
        jobs: list[tuple[int, Partition]] = []
        for index, partition in enumerate(planned):
            existing = _watermark_row(store, partition)
            if existing is not None:
                _validate_watermark_window(partition, existing)
                if str(existing[0]) == "succeeded":
                    result_slots[index] = PartitionResult(
                        partition=partition,
                        status="skipped",
                        rows_written=0,
                        watermark_after=None if existing[4] is None else str(existing[4]),
                    )
                    continue

            jobs.append((index, partition))

        completed = _execute_partition_jobs(
            store,
            jobs,
            backfill_run_id=run_id,
            params_for_partition=lambda partition: _partition_params(
                params or {},
                partition,
                backfill_run_id=run_id,
            ),
            watermark_after=_watermark_after,
            run_executor=run_executor,
            retry_policy=retry_policy,
            max_parallel=max_parallel,
            dead_letter=dead_letter,
            sleeper=sleeper or time.sleep,
            now=now,
            allow_parallel_executor=allow_parallel_executor,
        )
        for index, result in completed.items():
            result_slots[index] = result
        results = [
            result
            for result in result_slots
            if result is not None
        ]
    except Exception as exc:
        error_message = f"{exc}\n{traceback.format_exc(limit=20)}"
        _finish_run(
            store,
            backfill_run_id=run_id,
            status="failed",
            finished_at=now(),
            error_message=error_message,
        )
        raise

    succeeded = sum(1 for result in results if result.status == "succeeded")
    skipped = sum(1 for result in results if result.status == "skipped")
    failed = sum(1 for result in results if result.status in {"failed", "dead_lettered"})
    status = "succeeded" if failed == 0 else ("partial" if dead_letter else "failed")
    if not (initial_run_status == "succeeded" and succeeded == 0 and failed == 0):
        _finish_run(
            store,
            backfill_run_id=run_id,
            status=status,
            finished_at=now(),
        )
    return BackfillRunResult(
        backfill_run_id=run_id,
        dataset_id=dataset_id,
        status=status,
        dataset_order=dataset_order,
        partitions_planned=len(planned),
        partitions_succeeded=succeeded,
        partitions_skipped=skipped,
        partitions_failed=failed,
        rows_written=sum(result.rows_written for result in results),
        partition_results=tuple(results),
    )


def run_maintenance(
    store: Any,
    dataset_id: str,
    start: dt.date | dt.datetime | str,
    end: dt.date | dt.datetime | str,
    chunk: int | dt.timedelta | str,
    *,
    registry: Mapping[str, RegistryEntry] | None = None,
    params: Mapping[str, Any] | None = None,
    backfill_run_id: str | None = None,
    include_dependencies: bool = False,
    executor: PartitionExecutor | None = None,
    clock: Clock | None = None,
    retry_policy: RetryPolicyConfig = None,
    sleeper: Sleeper | None = None,
    max_parallel: int = 1,
    dead_letter: bool = False,
) -> BackfillRunResult:
    """Execute only partitions whose recorded watermark is stale or missing.

    Current watermarks use the same dataset/upstream reduction as the dataset
    orchestrator: dependency watermarks drive downstream datasets, and datasets
    with no dependencies compare against their own watermarks. Partition-scoped
    watermark names are preferred when present; otherwise the orchestrator-style
    coarse watermark is used as the deterministic fallback.
    """

    selected_registry = _default_registry() if registry is None else registry
    window_start = _coerce_date(start, "start")
    window_end = _coerce_date(end, "end")
    chunk_spec = _parse_chunk(chunk)
    run_id = backfill_run_id or str(uuid.uuid4())
    now = clock or _now_utc_naive

    _initialize_store(store)
    dataset_order = _dependency_order(
        dataset_id,
        selected_registry,
        include_dependencies=include_dependencies,
    )
    candidates = tuple(
        partition
        for current_dataset_id in dataset_order
        for partition in plan_backfill(
            current_dataset_id,
            window_start,
            window_end,
            chunk_spec.label,
            registry=selected_registry,
        )
    )
    partitions_by_dataset = {
        current_dataset_id: tuple(
            partition
            for partition in candidates
            if partition.dataset_id == current_dataset_id
        )
        for current_dataset_id in dataset_order
    }
    sources_by_dataset = {
        current_dataset_id: _watermark_sources_for_dataset(
            store,
            current_dataset_id,
            selected_registry,
        )
        for current_dataset_id in dataset_order
    }
    partition_scope_by_dataset = {
        current_dataset_id: _sources_have_partition_scope(
            sources_by_dataset[current_dataset_id],
            partitions_by_dataset[current_dataset_id],
        )
        for current_dataset_id in dataset_order
    }
    source_watermarks_by_dataset = {
        current_dataset_id: _sources_have_current_watermark(
            sources_by_dataset[current_dataset_id],
        )
        for current_dataset_id in dataset_order
    }

    current_watermarks: dict[str, str | None] = {}
    scheduled: list[Partition] = []
    for partition in candidates:
        existing = _watermark_row(store, partition)
        if existing is not None:
            _validate_watermark_window(partition, existing)
        current_watermark = _current_partition_watermark(
            sources_by_dataset[partition.dataset_id],
            partition,
            has_partition_scope=partition_scope_by_dataset[partition.dataset_id],
        )
        current_watermarks[partition.partition_key] = current_watermark
        if _should_run_maintenance_partition(
            existing,
            current_watermark=current_watermark,
            has_current_watermarks=source_watermarks_by_dataset[partition.dataset_id],
        ):
            scheduled.append(partition)

    initial_run_status = _insert_run_header(
        store,
        backfill_run_id=run_id,
        dataset_id=dataset_id,
        start=window_start,
        end=window_end,
        chunk_label=chunk_spec.label,
        ts=now(),
    )

    results: list[PartitionResult] = []
    run_executor = executor
    if run_executor is None:
        run_executor = lambda s, p, partition_params: _default_executor(
            s,
            p,
            partition_params,
            registry=selected_registry,
        )
        allow_parallel_executor = False
    else:
        allow_parallel_executor = True

    try:
        indexed = list(enumerate(scheduled))

        def maintenance_watermark_after(result: Any, partition: Partition) -> str:
            return _maintenance_watermark_after(
                result,
                partition,
                current_watermark=current_watermarks[partition.partition_key],
            )

        completed = _execute_partition_jobs(
            store,
            indexed,
            backfill_run_id=run_id,
            params_for_partition=lambda partition: _maintenance_partition_params(
                params or {},
                partition,
                backfill_run_id=run_id,
                current_watermark=current_watermarks[partition.partition_key],
            ),
            watermark_after=maintenance_watermark_after,
            run_executor=run_executor,
            retry_policy=retry_policy,
            max_parallel=max_parallel,
            dead_letter=dead_letter,
            sleeper=sleeper or time.sleep,
            now=now,
            allow_parallel_executor=allow_parallel_executor,
        )
        results = [
            completed[index]
            for index in sorted(completed)
        ]
    except Exception as exc:
        error_message = f"{exc}\n{traceback.format_exc(limit=20)}"
        _finish_run(
            store,
            backfill_run_id=run_id,
            status="failed",
            finished_at=now(),
            error_message=error_message,
        )
        raise

    succeeded = sum(1 for result in results if result.status == "succeeded")
    failed = sum(1 for result in results if result.status in {"failed", "dead_lettered"})
    status = "succeeded" if failed == 0 else ("partial" if dead_letter else "failed")
    if not (initial_run_status == "succeeded" and succeeded == 0 and failed == 0):
        _finish_run(
            store,
            backfill_run_id=run_id,
            status=status,
            finished_at=now(),
        )
    return BackfillRunResult(
        backfill_run_id=run_id,
        dataset_id=dataset_id,
        status=status,
        dataset_order=dataset_order,
        partitions_planned=len(scheduled),
        partitions_succeeded=succeeded,
        partitions_skipped=0,
        partitions_failed=failed,
        rows_written=sum(result.rows_written for result in results),
        partition_results=tuple(results),
    )


__all__ = [
    "BackfillRunResult",
    "Partition",
    "PartitionResult",
    "plan_backfill",
    "run_backfill",
    "run_maintenance",
]
