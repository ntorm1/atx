from __future__ import annotations

import datetime as dt
import re
import traceback
import uuid
from dataclasses import dataclass
from typing import Any, Callable, Mapping, Sequence

from .dataset import DatasetLoadResult
from .orchestrator import RegistryEntry, build_dataset_dag


Clock = Callable[[], dt.datetime]
PartitionExecutor = Callable[[Any, "Partition", Mapping[str, Any]], Any]


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
) -> None:
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
) -> BackfillRunResult:
    """Execute a windowed backfill and persist per-partition progress.

    Completed partitions are strict no-ops: their existing ``backfill_watermark`` rows
    are read but not updated, and the dataset executor is not called.
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

    _insert_run_header(
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

    try:
        for partition in planned:
            existing = _watermark_row(store, partition)
            if existing is not None:
                _validate_watermark_window(partition, existing)
                if str(existing[0]) == "succeeded":
                    results.append(
                        PartitionResult(
                            partition=partition,
                            status="skipped",
                            rows_written=0,
                            watermark_after=None if existing[4] is None else str(existing[4]),
                        )
                    )
                    continue

            _mark_partition_running(
                store,
                partition,
                backfill_run_id=run_id,
                ts=now(),
            )
            partition_params = _partition_params(
                params or {},
                partition,
                backfill_run_id=run_id,
            )
            try:
                execution_result = run_executor(store, partition, partition_params)
            except Exception:
                _mark_partition_failed(
                    store,
                    partition,
                    backfill_run_id=run_id,
                    ts=now(),
                )
                raise

            rows = _rows_written(execution_result)
            watermark_after = _watermark_after(execution_result, partition)
            _mark_partition_succeeded(
                store,
                partition,
                rows_written=rows,
                watermark_after=watermark_after,
                backfill_run_id=run_id,
                ts=now(),
            )
            results.append(
                PartitionResult(
                    partition=partition,
                    status="succeeded",
                    rows_written=rows,
                    watermark_after=watermark_after,
                )
            )
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
    failed = sum(1 for result in results if result.status == "failed")
    status = "succeeded" if failed == 0 else "failed"
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


__all__ = [
    "BackfillRunResult",
    "Partition",
    "PartitionResult",
    "plan_backfill",
    "run_backfill",
]
